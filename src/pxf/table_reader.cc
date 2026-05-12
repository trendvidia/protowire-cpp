// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/table_reader.h"

#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "protowire/detail/base64.h"
#include "protowire/pxf.h"  // Unmarshal, UnmarshalOptions
#include "protowire/pxf/parser.h"

namespace protowire::pxf {

namespace {

// Chunk size for std::istream pulls. Larger reduces syscall pressure;
// smaller bounds per-row peak buffer occupancy. 4 KiB matches typical
// row sizes and the Go reference's streamPullSize.
constexpr size_t kStreamPullSize = 4096;

bool IsIdentPart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// SkipResult is the byte offset past a string / bytes literal / comment
// starting at `i`. Returned value semantics mirror the Go reference's
// skipStringOrComment:
//   - returns `i` unchanged if the byte at `i` is not an opener
//   - returns -1 if the construct is incomplete (caller pulls more)
//   - sets `*err` to non-OK for malformed constructs that can't be
//     fixed by more bytes (unterminated single-line string already
//     containing a newline)
//
// Strings, triple-quoted strings, bytes literals (`b"..."`), `#`
// line comments, `//` line comments, and `/* ... */` block comments
// are all handled.
int SkipStringOrComment(std::string_view input, int i, Status* err);

int SkipSimpleString(std::string_view input, int i, Status* err) {
  int j = i + 1;
  while (j < static_cast<int>(input.size())) {
    char c = input[j];
    if (c == '\\') {
      if (j + 1 >= static_cast<int>(input.size())) return -1;
      j += 2;
      continue;
    }
    if (c == '"') return j + 1;
    if (c == '\n') {
      *err = Status::Error("pxf: unterminated string literal");
      return 0;
    }
    ++j;
  }
  return -1;
}

int SkipTripleString(std::string_view input, int i) {
  int j = i + 3;
  int n = static_cast<int>(input.size());
  while (j + 2 < n) {
    if (input[j] == '"' && input[j + 1] == '"' && input[j + 2] == '"') return j + 3;
    ++j;
  }
  return -1;
}

int SkipBytesLiteral(std::string_view input, int i, Status* err) {
  int j = i + 2;  // past `b"`
  int n = static_cast<int>(input.size());
  while (j < n) {
    char c = input[j];
    if (c == '"') return j + 1;
    if (c == '\n') {
      *err = Status::Error("pxf: unterminated bytes literal");
      return 0;
    }
    ++j;
  }
  return -1;
}

int SkipLineComment(std::string_view input, int from) {
  int j = from;
  int n = static_cast<int>(input.size());
  while (j < n && input[j] != '\n') ++j;
  return j;
}

int SkipBlockComment(std::string_view input, int from) {
  int j = from;
  int n = static_cast<int>(input.size());
  while (j + 1 < n) {
    if (input[j] == '*' && input[j + 1] == '/') return j + 2;
    ++j;
  }
  return -1;
}

int SkipStringOrComment(std::string_view input, int i, Status* err) {
  int n = static_cast<int>(input.size());
  if (i >= n) return i;
  char c = input[i];
  if (c == '"') {
    if (i + 2 < n && input[i + 1] == '"' && input[i + 2] == '"') {
      return SkipTripleString(input, i);
    }
    return SkipSimpleString(input, i, err);
  }
  if (c == 'b' && i + 1 < n && input[i + 1] == '"') {
    return SkipBytesLiteral(input, i, err);
  }
  if (c == '#') return SkipLineComment(input, i + 1);
  if (c == '/' && i + 1 < n && input[i + 1] == '/') return SkipLineComment(input, i + 2);
  if (c == '/' && i + 1 < n && input[i + 1] == '*') return SkipBlockComment(input, i + 2);
  return i;
}

// Returns (offset, found, err). On error, `*err` is set non-OK.
int FindAtTable(std::string_view input, bool* found, Status* err) {
  *found = false;
  int i = 0;
  int n = static_cast<int>(input.size());
  static constexpr std::string_view kAtTable = "@table";
  while (i < n) {
    int j = SkipStringOrComment(input, i, err);
    if (!err->ok()) return 0;
    if (j == -1) return 0;  // incomplete — pull more
    if (j != i) {
      i = j;
      continue;
    }
    if (input[i] == '@' && i + static_cast<int>(kAtTable.size()) <= n &&
        input.substr(i, kAtTable.size()) == kAtTable) {
      int after = i + static_cast<int>(kAtTable.size());
      if (after == n) return 0;  // could be `@table` + more bytes — conservative
      if (!IsIdentPart(input[after])) {
        *found = true;
        return i;
      }
    }
    ++i;
  }
  return 0;
}

int FindNextChar(std::string_view input, int start_from, char ch, bool* found, Status* err) {
  *found = false;
  int i = start_from;
  int n = static_cast<int>(input.size());
  while (i < n) {
    int j = SkipStringOrComment(input, i, err);
    if (!err->ok()) return 0;
    if (j == -1) return 0;
    if (j != i) {
      i = j;
      continue;
    }
    if (input[i] == ch) {
      *found = true;
      return i;
    }
    ++i;
  }
  return 0;
}

// FindMatchingParenSafe returns the index of the `)` matching the `(`
// at `open_idx`. Sets `*found` to true on success; on incomplete
// input, returns 0 with `*found=false` and `*err` OK; on malformed
// string / comment inside, sets `*err` non-OK.
int FindMatchingParenSafe(std::string_view input, int open_idx, bool* found, Status* err) {
  *found = false;
  int depth = 1;
  int i = open_idx + 1;
  int n = static_cast<int>(input.size());
  while (i < n) {
    int j = SkipStringOrComment(input, i, err);
    if (!err->ok()) return 0;
    if (j == -1) return 0;
    if (j != i) {
      i = j;
      continue;
    }
    char c = input[i];
    if (c == '(') {
      ++depth;
      ++i;
    } else if (c == ')') {
      --depth;
      if (depth == 0) {
        *found = true;
        return i;
      }
      ++i;
    } else {
      ++i;
    }
  }
  return 0;
}

// Locates the closing `)` of the first complete `@table TYPE ( cols )`
// header in `input`. Returns the index of that `)`; `*found` is false
// when more bytes are needed.
int ScanHeaderEnd(std::string_view input, bool* found, Status* err) {
  *found = false;
  bool ok = false;
  int at_idx = FindAtTable(input, &ok, err);
  if (!err->ok()) return 0;
  if (!ok) return 0;
  bool lp_ok = false;
  int lparen = FindNextChar(
      input, at_idx + static_cast<int>(std::string_view("@table").size()), '(', &lp_ok, err);
  if (!err->ok()) return 0;
  if (!lp_ok) return 0;
  bool rp_ok = false;
  int end = FindMatchingParenSafe(input, lparen, &rp_ok, err);
  if (!err->ok()) return 0;
  if (!rp_ok) return 0;
  *found = true;
  return end;
}

// FindNextRow locates the next `( ... )` row in `input`, skipping
// leading whitespace + comments. Sets *found true when a complete row
// is in the buffer; returns inclusive [start, end] bounds via *out_start
// / *out_end. Sets *out_done true when the next significant byte isn't
// `(` — i.e. the row sequence is over.
void FindNextRow(std::string_view input,
                 bool* found,
                 bool* out_done,
                 int* out_start,
                 int* out_end,
                 Status* err) {
  *found = false;
  *out_done = false;
  int i = 0;
  int n = static_cast<int>(input.size());
  while (i < n) {
    char c = input[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++i;
      continue;
    }
    int j = SkipStringOrComment(input, i, err);
    if (!err->ok()) return;
    if (j == -1) return;
    if (j != i) {
      i = j;
      continue;
    }
    break;
  }
  if (i >= n) return;
  if (input[i] != '(') {
    *out_done = true;
    return;
  }
  bool ok = false;
  int end = FindMatchingParenSafe(input, i, &ok, err);
  if (!err->ok()) return;
  if (!ok) return;
  *found = true;
  *out_start = i;
  *out_end = end;
}

// ---- BindRow helpers ------------------------------------------------------

// Append a single cell value as PXF text. v1 cells are scalar-shaped
// (no list, no block), so we only handle the leaf-value variants — list
// and block AST nodes are unreachable because the parser rejects them
// at row parse time.
Status AppendCellValue(std::string& out, const ValuePtr& cell) {
  return std::visit(
      [&out](const auto& p) -> Status {
        using T = std::decay_t<decltype(*p)>;
        if constexpr (std::is_same_v<T, StringVal>) {
          // Re-quote with escapes for `"` and `\`. Other shapes round-
          // trip verbatim because the lexer accepts UTF-8 in strings.
          out.push_back('"');
          for (char c : p->value) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
          }
          out.push_back('"');
          return Status::OK();
        } else if constexpr (std::is_same_v<T, IntVal>) {
          out.append(p->raw);
          return Status::OK();
        } else if constexpr (std::is_same_v<T, FloatVal>) {
          out.append(p->raw);
          return Status::OK();
        } else if constexpr (std::is_same_v<T, BoolVal>) {
          out.append(p->value ? "true" : "false");
          return Status::OK();
        } else if constexpr (std::is_same_v<T, BytesVal>) {
          out.append("b\"");
          out.append(detail::Base64EncodeStd(p->value));
          out.push_back('"');
          return Status::OK();
        } else if constexpr (std::is_same_v<T, NullVal>) {
          out.append("null");
          return Status::OK();
        } else if constexpr (std::is_same_v<T, IdentVal>) {
          out.append(p->name);
          return Status::OK();
        } else if constexpr (std::is_same_v<T, TimestampVal>) {
          out.append(p->raw);
          return Status::OK();
        } else if constexpr (std::is_same_v<T, DurationVal>) {
          out.append(p->raw);
          return Status::OK();
        } else {
          return Status::Error(
              "pxf: BindRow: unexpected cell value type (v1 @table cells are scalar-shaped)");
        }
      },
      cell);
}

}  // namespace

// ---- TableReader public API -----------------------------------------------

StatusOr<std::unique_ptr<TableReader>> TableReader::Create(std::istream* src) {
  if (src == nullptr) return Status::Error("pxf: TableReader: null istream");
  auto tr = std::unique_ptr<TableReader>(new TableReader());
  tr->src_ = src;
  if (Status s = tr->ReadHeader(); !s.ok()) return s;
  return tr;
}

Status TableReader::Next(TableRow* out) {
  if (!err_.ok()) return err_;
  if (finished_) return Status::OK();
  for (;;) {
    bool found = false;
    bool done = false;
    int start = 0;
    int end = 0;
    Status scan_err;
    FindNextRow(pending_, &found, &done, &start, &end, &scan_err);
    if (!scan_err.ok()) {
      err_ = scan_err;
      return err_;
    }
    if (found) {
      std::string_view row_bytes(pending_.data() + start, end - start + 1);
      // Parse the row via the AST parser. We wrap it in a synthetic
      // `@table T (col0,col1,...) <row>` so the existing parser
      // accepts it and arity-checks against our column count.
      std::string synthetic;
      synthetic.reserve(row_bytes.size() + 32 + columns_.size() * 8);
      synthetic.append("@table _.Row (");
      for (size_t i = 0; i < columns_.size(); ++i) {
        if (i != 0) synthetic.push_back(',');
        synthetic.append(columns_[i]);
      }
      synthetic.append(")\n");
      synthetic.append(row_bytes.data(), row_bytes.size());
      auto doc = Parse(synthetic);
      // Advance past the consumed bytes whether parse succeeded or not
      // — on failure we don't want to retry the same bad row forever.
      pending_.erase(0, static_cast<size_t>(end) + 1);
      if (!doc.ok()) {
        err_ = doc.status();
        return err_;
      }
      if (doc->tables.empty() || doc->tables[0].rows.empty()) {
        err_ = Status::Error("pxf: TableReader: synthetic row parse produced no row");
        return err_;
      }
      *out = std::move(doc->tables[0].rows[0]);
      return Status::OK();
    }
    if (done) {
      finished_ = true;
      return Status::OK();
    }
    if (src_eof_) {
      finished_ = true;
      return Status::OK();
    }
    if (Status s = Pull(kStreamPullSize); !s.ok()) {
      err_ = s;
      return err_;
    }
  }
}

Status TableReader::Scan(google::protobuf::Message* msg) {
  TableRow row;
  if (Status s = Next(&row); !s.ok()) return s;
  if (finished_) return Status::OK();
  return BindRow(msg, columns_, row);
}

std::unique_ptr<std::istream> TableReader::Tail() {
  std::ostringstream buf;
  if (!pending_.empty()) buf.write(pending_.data(), static_cast<std::streamsize>(pending_.size()));
  if (src_ != nullptr && !src_eof_) buf << src_->rdbuf();
  return std::make_unique<std::istringstream>(buf.str());
}

// ---- TableReader internals ------------------------------------------------

Status TableReader::Pull(size_t n) {
  if (src_eof_) return Status::OK();
  std::string buf(n, '\0');
  src_->read(buf.data(), static_cast<std::streamsize>(n));
  std::streamsize got = src_->gcount();
  if (got > 0) pending_.append(buf.data(), static_cast<size_t>(got));
  if (src_->eof()) {
    src_eof_ = true;
    return Status::OK();
  }
  if (src_->bad()) return Status::Error("pxf: TableReader: istream read error");
  // `fail` without `eof` is unusual; treat as a read error.
  if (src_->fail() && got == 0) return Status::Error("pxf: TableReader: istream read failed");
  return Status::OK();
}

Status TableReader::ReadHeader() {
  for (;;) {
    bool found = false;
    Status scan_err;
    int header_end = ScanHeaderEnd(pending_, &found, &scan_err);
    if (!scan_err.ok()) return scan_err;
    if (found) {
      std::string_view header(pending_.data(), static_cast<size_t>(header_end) + 1);
      auto doc = Parse(header);
      if (!doc.ok()) return doc.status();
      if (doc->tables.empty()) {
        return Status::Error("pxf: no @table directive in stream");
      }
      type_ = std::move(doc->tables[0].type);
      columns_ = std::move(doc->tables[0].columns);
      directives_ = std::move(doc->directives);
      pending_.erase(0, static_cast<size_t>(header_end) + 1);
      return Status::OK();
    }
    if (src_eof_) {
      return Status::Error("pxf: no @table directive in stream");
    }
    if (static_cast<int>(pending_.size()) >= kDefaultHeaderMaxBytes) {
      return Status::Error(
          "pxf: @table header exceeds 65536 bytes; raise the budget or check that the input "
          "begins with `@table TYPE (cols)`");
    }
    if (Status s = Pull(kStreamPullSize); !s.ok()) return s;
  }
}

// ---- BindRow free function ------------------------------------------------

Status BindRow(google::protobuf::Message* msg,
               const std::vector<std::string>& columns,
               const TableRow& row) {
  if (columns.size() != row.cells.size()) {
    return Status::Error(std::string("pxf: BindRow: ") + std::to_string(columns.size()) +
                         " columns vs " + std::to_string(row.cells.size()) + " cells");
  }
  // Render the row as a synthetic PXF body: one `<column> = <value>`
  // entry per non-nullopt cell. Empty cells stay absent (the decoder
  // applies pxf.default / pxf.required as if the field weren't named).
  std::string body;
  for (size_t i = 0; i < columns.size(); ++i) {
    const auto& cell = row.cells[i];
    if (!cell.has_value()) continue;
    body.append(columns[i]);
    body.append(" = ");
    if (Status s = AppendCellValue(body, *cell); !s.ok()) return s;
    body.push_back('\n');
  }
  // SkipValidate avoids re-running the reserved-name check per row —
  // TableReader::Create / the materializing UnmarshalFull already
  // validated the descriptor once at bind time.
  UnmarshalOptions opts;
  opts.skip_validate = true;
  return Unmarshal(body, msg, opts);
}

}  // namespace protowire::pxf

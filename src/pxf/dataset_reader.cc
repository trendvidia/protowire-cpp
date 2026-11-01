// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/dataset_reader.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dataset_paths.h"
#include "parser_internal.h"
#include "protowire/limits.h"
#include "protowire/pxf.h"  // Unmarshal, UnmarshalOptions
#include "protowire/pxf/format.h"
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
  static constexpr std::string_view kAtDataset = "@dataset";
  while (i < n) {
    int j = SkipStringOrComment(input, i, err);
    if (!err->ok()) return 0;
    if (j == -1) return 0;  // incomplete — pull more
    if (j != i) {
      i = j;
      continue;
    }
    if (input[i] == '@' && i + static_cast<int>(kAtDataset.size()) <= n &&
        input.substr(i, kAtDataset.size()) == kAtDataset) {
      int after = i + static_cast<int>(kAtDataset.size());
      if (after == n) return 0;  // could be `@dataset` + more bytes — conservative
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

// Locates the closing `)` of the first complete `@dataset TYPE ( cols )`
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
      input, at_idx + static_cast<int>(std::string_view("@dataset").size()), '(', &lp_ok, err);
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

// BodyNode is one level of the block tree a dotted row body is written
// from. col is the column of a cell, or -1 for a block.
struct BodyNode {
  std::string name;
  int col = -1;
  std::vector<std::unique_ptr<BodyNode>> children;
};

// CellSpan is the lines of the body one column's entry takes.
struct CellSpan {
  int col;
  int first;
  int last;
};

// BodyWriter writes a row as a PXF body, recording the lines each column's
// entry takes so that a decode error can name the column. line_ is the
// number of the next line to be written, and the body opens with an empty
// line, so a decoder error without a position (1:1 or none) falls in no
// column.
class BodyWriter {
 public:
  explicit BodyWriter(const DatasetRow& row) : row_(row) { body_.push_back('\n'); }

  void Cell(const std::string& name, int col) {
    const auto& cell = row_.cells[col];
    if (!cell.has_value()) return;  // callers pass non-empty cells
    const size_t start = body_.size();
    body_.append(name);
    body_.append(" = ");
    body_.append(FormatValue(*cell));
    body_.push_back('\n');
    const int lines = static_cast<int>(std::count(body_.begin() + start, body_.end(), '\n'));
    spans_.push_back(CellSpan{col, line_, line_ + lines - 1});
    line_ += lines;
  }

  void Children(const BodyNode& n) {
    for (const auto& ch : n.children) {
      if (ch->col >= 0) {
        Cell(ch->name, ch->col);
        continue;
      }
      // An error on the block's own lines (a second oneof member, say) is
      // laid at the first column in it. Its span comes after its cells',
      // so a cell's wins.
      const int first = line_;
      body_.append(ch->name);
      body_.append(" {\n");
      ++line_;
      Children(*ch);
      body_.append("}\n");
      spans_.push_back(CellSpan{FirstColumn(*ch), first, line_});
      ++line_;
    }
  }

  const std::string& body() const { return body_; }
  const std::vector<CellSpan>& spans() const { return spans_; }

 private:
  static int FirstColumn(const BodyNode& n) {
    const BodyNode* at = &n;
    while (at->col < 0) at = at->children.front().get();
    return at->col;
  }

  const DatasetRow& row_;
  std::string body_;
  int line_ = 2;
  std::vector<CellSpan> spans_;
};

// WriteRowBody renders the row's non-empty cells. Cells that share a path
// prefix go in one block, and a prefix with no non-empty cell writes no
// block, so its sub-message stays absent (draft §3.4.4). Per-cell blocks
// would reject two columns that reach the same oneof member.
void WriteRowBody(const std::vector<std::string>& columns, const DatasetRow& row, BodyWriter* w) {
  const bool dotted = std::any_of(columns.begin(), columns.end(), [](const std::string& c) {
    return c.find('.') != std::string::npos;
  });
  if (!dotted) {
    for (size_t i = 0; i < columns.size(); ++i) {
      if (row.cells[i].has_value()) w->Cell(columns[i], static_cast<int>(i));
    }
    return;
  }
  BodyNode root;
  for (size_t i = 0; i < columns.size(); ++i) {
    if (!row.cells[i].has_value()) continue;
    BodyNode* n = &root;
    std::string_view rest = columns[i];
    for (size_t dot = rest.find('.'); dot != std::string_view::npos; dot = rest.find('.')) {
      const std::string_view seg = rest.substr(0, dot);
      BodyNode* next = nullptr;
      for (const auto& ch : n->children) {
        if (ch->col < 0 && ch->name == seg) next = ch.get();
      }
      if (next == nullptr) {
        n->children.push_back(std::make_unique<BodyNode>());
        next = n->children.back().get();
        next->name = std::string(seg);
      }
      n = next;
      rest.remove_prefix(dot + 1);
    }
    auto leaf = std::make_unique<BodyNode>();
    leaf->name = std::string(rest);
    leaf->col = static_cast<int>(i);
    n->children.push_back(std::move(leaf));
  }
  w->Children(root);
}

// HeaderError reports a broken column rule at the row being bound, as a
// decode error is reported. A row built by hand has no position to give.
Status HeaderError(std::string msg, Position pos) {
  if (pos.line > 0) return Status::Error(pos.line, pos.column, std::move(msg));
  return Status::Error("pxf: " + msg);
}

// AtRow moves a decode error to the row it came from, and names the column
// whose entry it fell in. The decoder read a synthetic body, so its
// positions are lines of that body. A hand-built row has no position to
// give and keeps the body's, counted without the empty first line.
Status AtRow(const Status& st,
             Position pos,
             const std::vector<std::string>& columns,
             const std::vector<CellSpan>& spans) {
  const auto at = st.position();
  std::string msg = st.message();
  if (at.has_value()) {
    for (const CellSpan& sp : spans) {
      if (at->first >= sp.first && at->first <= sp.last) {
        msg = "column " + columns[sp.col] + ": " + msg;
        break;
      }
    }
  }
  if (pos.line > 0) return Status::Error(pos.line, pos.column, std::move(msg));
  if (at.has_value()) return Status::Error(at->first - 1, at->second, std::move(msg));
  return Status::Error(std::move(msg));
}

// BindCheckedRow binds a row whose columns are known to suit msg's type.
// UnmarshalFull rather than Unmarshal because its post-decode pass is what
// gives an empty cell the field's (pxf.default), rejects it for
// (pxf.required), and lets a null cell satisfy required (draft §3.4.4).
Status BindCheckedRow(google::protobuf::Message* msg,
                      const std::vector<std::string>& columns,
                      const DatasetRow& row,
                      const UnmarshalOptions& opts) {
  BodyWriter w(row);
  WriteRowBody(columns, row, &w);
  auto r = UnmarshalFull(w.body(), msg, opts);
  if (r.ok()) return Status::OK();
  return AtRow(r.status(), row.pos, columns, w.spans());
}

}  // namespace

// ---- DatasetReader public API -----------------------------------------------

StatusOr<std::unique_ptr<DatasetReader>> DatasetReader::Create(std::istream* src) {
  return Create(src, UnmarshalOptions{});
}

StatusOr<std::unique_ptr<DatasetReader>> DatasetReader::Create(std::istream* src,
                                                               UnmarshalOptions opts) {
  if (src == nullptr) return Status::Error("pxf: DatasetReader: null istream");
  auto tr = std::unique_ptr<DatasetReader>(new DatasetReader());
  tr->src_ = src;
  tr->opts_ = opts;
  if (Status s = tr->ReadHeader(); !s.ok()) return s;
  return tr;
}

Status DatasetReader::Next(DatasetRow* out) {
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
      auto parsed =
          ParseDatasetRow(row_bytes,
                          static_cast<int>(columns_.size()),
                          opts_.max_nesting_depth > 0 ? opts_.max_nesting_depth : kMaxNestingDepth);
      // Advance past the consumed bytes whether parse succeeded or not
      // — on failure we don't want to retry the same bad row forever.
      pending_.erase(0, static_cast<size_t>(end) + 1);
      if (!parsed.ok()) {
        err_ = parsed.status();
        return err_;
      }
      *out = std::move(parsed).consume();
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

Status DatasetReader::Scan(google::protobuf::Message* msg) {
  DatasetRow row;
  if (Status s = Next(&row); !s.ok()) return s;
  if (finished_) return Status::OK();
  if (row.cells.size() != columns_.size()) {
    return Status::Error(std::string("pxf: BindRow: ") + std::to_string(columns_.size()) +
                         " columns vs " + std::to_string(row.cells.size()) + " cells");
  }
  // The schema and the column paths depend on msg's type and the header,
  // never on the row, so they are checked once per type. A reader remembers
  // one type, which is the usual case, and a caller alternating types pays
  // only for what it does.
  const auto* desc = msg->GetDescriptor();
  if (desc != checked_type_) {
    if (!opts_.skip_validate) {
      if (Status s = CheckDescriptorSchema(desc); !s.ok()) return s;
    }
    if (std::string path_err = CheckColumnPaths(desc, columns_); !path_err.empty()) {
      return HeaderError(std::move(path_err), row.pos);
    }
    checked_type_ = desc;
  }
  UnmarshalOptions opts = opts_;
  opts.skip_validate = true;
  return BindCheckedRow(msg, columns_, row, opts);
}

std::unique_ptr<std::istream> DatasetReader::Tail() {
  std::ostringstream buf;
  if (!pending_.empty()) buf.write(pending_.data(), static_cast<std::streamsize>(pending_.size()));
  if (src_ != nullptr && !src_eof_) buf << src_->rdbuf();
  return std::make_unique<std::istringstream>(buf.str());
}

// ---- DatasetReader internals ------------------------------------------------

Status DatasetReader::Pull(size_t n) {
  if (src_eof_) return Status::OK();
  std::string buf(n, '\0');
  src_->read(buf.data(), static_cast<std::streamsize>(n));
  std::streamsize got = src_->gcount();
  if (got > 0) pending_.append(buf.data(), static_cast<size_t>(got));
  if (src_->eof()) {
    src_eof_ = true;
    return Status::OK();
  }
  if (src_->bad()) return Status::Error("pxf: DatasetReader: istream read error");
  // `fail` without `eof` is unusual; treat as a read error.
  if (src_->fail() && got == 0) return Status::Error("pxf: DatasetReader: istream read failed");
  return Status::OK();
}

Status DatasetReader::ReadHeader() {
  for (;;) {
    bool found = false;
    Status scan_err;
    int header_end = ScanHeaderEnd(pending_, &found, &scan_err);
    if (!scan_err.ok()) return scan_err;
    if (found) {
      std::string_view header(pending_.data(), static_cast<size_t>(header_end) + 1);
      auto doc = Parse(header);
      if (!doc.ok()) return doc.status();
      if (doc->datasets.empty()) {
        return Status::Error("pxf: no @dataset directive in stream");
      }
      type_ = std::move(doc->datasets[0].type);
      columns_ = std::move(doc->datasets[0].columns);
      directives_ = std::move(doc->directives);
      pending_.erase(0, static_cast<size_t>(header_end) + 1);
      return Status::OK();
    }
    if (src_eof_) {
      return Status::Error("pxf: no @dataset directive in stream");
    }
    if (static_cast<int>(pending_.size()) >= kDefaultHeaderMaxBytes) {
      return Status::Error(
          "pxf: @dataset header exceeds 65536 bytes; raise the budget or check that the input "
          "begins with `@dataset TYPE (cols)`");
    }
    if (Status s = Pull(kStreamPullSize); !s.ok()) return s;
  }
}

// ---- BindRow free function ------------------------------------------------

Status BindRow(google::protobuf::Message* msg,
               const std::vector<std::string>& columns,
               const DatasetRow& row) {
  return BindRow(msg, columns, row, UnmarshalOptions{});
}

Status BindRow(google::protobuf::Message* msg,
               const std::vector<std::string>& columns,
               const DatasetRow& row,
               UnmarshalOptions opts) {
  if (columns.size() != row.cells.size()) {
    return Status::Error(std::string("pxf: BindRow: ") + std::to_string(columns.size()) +
                         " columns vs " + std::to_string(row.cells.size()) + " cells");
  }
  if (std::string path_err = CheckColumnPaths(msg->GetDescriptor(), columns); !path_err.empty()) {
    return HeaderError(std::move(path_err), row.pos);
  }
  return BindCheckedRow(msg, columns, row, opts);
}

}  // namespace protowire::pxf

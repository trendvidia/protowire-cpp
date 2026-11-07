// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/dataset_reader.h"

#include <algorithm>
#include <cstring>
#include <optional>
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
#include "protowire/pxf/lexer.h"
#include "protowire/pxf/parser.h"

namespace protowire::pxf {

namespace {

// Chunk size for std::istream pulls. Larger reduces syscall pressure;
// smaller bounds per-row peak buffer occupancy. 4 KiB matches typical
// row sizes and the Go reference's streamPullSize.
constexpr size_t kStreamPullSize = 4096;

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
  if (at.has_value()) {
    return Status::Error(at->first > 1 ? at->first - 1 : at->first, at->second, std::move(msg));
  }
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
  tr->opts_ = std::move(opts);
  if (Status s = tr->ReadHeader(); !s.ok()) return s;
  return tr;
}

Status DatasetReader::Next(DatasetRow* out) {
  if (!err_.ok()) return err_;
  if (finished_) return Status::OK();
  if (row_start_ < 0) {
    if (auto row = TryWholeRow(); row.has_value()) {
      *out = std::move(*row);
      return Status::OK();
    }
  }
  for (;;) {
    int start = 0;
    int end = 0;
    switch (ScanRow(&start, &end)) {
      case RowScan::kRow: {
        auto parsed = ParseRow(start, end);
        Consume(static_cast<size_t>(end));
        if (!parsed.ok()) return Fail(parsed.status());
        *out = std::move(parsed).consume();
        return Status::OK();
      }
      case RowScan::kBad:
        // Parse the rest of the buffer, not just through the token that
        // stopped the scan: the lexer may reject a token before its end (an
        // oversized bytes literal), and cut there it would read as
        // unterminated instead.
        return Fail(RowError(start));
      case RowScan::kEnd:
        // The directive and what precedes it stay buffered, for Tail.
        finished_ = true;
        return Status::OK();
      case RowScan::kNotRow: {
        const Position at = PosAt(static_cast<size_t>(start));
        return Fail(Status::Error(at.line, at.column, kDatasetBesideEntries));
      }
      case RowScan::kNeedMore:
        break;
    }
    // At the end of the source only spaces and comments may remain; anything
    // else is a row cut short.
    if (src_eof_) {
      if (start >= 0) return Fail(RowError(start));
      Consume(pending_.size());
      finished_ = true;
      return Status::OK();
    }
    if (Status s = Pull(kStreamPullSize); !s.ok()) return Fail(s);
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
  // The caller's limits but max_message_size, which is left at its default,
  // as protowire-go binds a row: the row's bytes were capped as they were
  // read, and the body it stands for repeats the column names, so it is
  // wider than the row.
  UnmarshalOptions opts = opts_;
  opts.skip_validate = true;
  opts.max_message_size = 0;
  return BindCheckedRow(msg, columns_, row, opts);
}

std::unique_ptr<std::istream> DatasetReader::Tail() {
  std::ostringstream buf;
  if (!pending_.empty()) buf.write(pending_.data(), static_cast<std::streamsize>(pending_.size()));
  if (src_ != nullptr && !src_eof_) buf << src_->rdbuf();
  return std::make_unique<std::istringstream>(buf.str());
}

// ---- DatasetReader internals ------------------------------------------------

// PosAt is the stream position of pending_[i], counted as the lexer counts:
// a byte is a column, and a newline starts the next line.
Position DatasetReader::PosAt(size_t i) const {
  Position p = base_;
  for (size_t k = 0; k < i; ++k) {
    if (pending_[k] == '\n') {
      ++p.line;
      p.column = 1;
    } else {
      ++p.column;
    }
  }
  p.offset += static_cast<int>(i);
  return p;
}

// Consume drops the first n pending bytes, moving base_ and the scan's
// offsets past them.
void DatasetReader::Consume(size_t n) {
  base_ = PosAt(n);
  pending_.erase(0, n);
  const int k = static_cast<int>(n);
  scan_pos_ = std::max(scan_pos_ - k, 0);
  if (row_start_ >= 0) row_start_ = row_start_ >= k ? row_start_ - k : -1;
  // Bytes are consumed only between tokens, when none is waiting.
  waiting_ = false;
}

DatasetReader::~DatasetReader() = default;

Status DatasetReader::Fail(Status st) {
  err_ = std::move(st);
  return err_;
}

int DatasetReader::MaxBytesLiteral() const {
  return opts_.max_bytes_literal_length > 0 ? opts_.max_bytes_literal_length
                                            : kMaxBytesLiteralLength;
}

// ParseRow parses pending_ from `start` as one row, with the parser Parse
// uses, at its stream position. `end` bounds the bytes it may read.
StatusOr<DatasetRow> DatasetReader::ParseRow(int start, int end, int* end_offset) const {
  return ParseDatasetRow(std::string_view(pending_).substr(start, end - start),
                         static_cast<int>(columns_.size()),
                         opts_.max_nesting_depth > 0 ? opts_.max_nesting_depth : kMaxNestingDepth,
                         MaxBytesLiteral(),
                         PosAt(static_cast<size_t>(start)),
                         end_offset);
}

// TryWholeRow is the fast path for a row already in the buffer: the row
// parser runs on the buffer directly, so the row is lexed once. If it
// parses a complete row, every token in it ended before its `)`, so it is
// the row Parse reads, and it is consumed. Anything else (a row cut by a
// read, a malformed row, no row) returns nothing, and ScanRow finds the
// boundary. Called only between rows.
std::optional<DatasetRow> DatasetReader::TryWholeRow() {
  int end_offset = 0;
  auto row = ParseRow(scan_pos_, static_cast<int>(pending_.size()), &end_offset);
  if (!row.ok()) return std::nullopt;
  Consume(static_cast<size_t>(end_offset - base_.offset));
  return std::move(row).consume();
}

// RowError is the error Parse reports for the row starting at pending_[start],
// which the scan found cannot close well. Every token before the one that
// stopped the scan is the same as Parse sees, so the row parser fails where
// Parse fails on the whole document.
Status DatasetReader::RowError(int start) const {
  auto row = ParseRow(start, static_cast<int>(pending_.size()));
  if (!row.ok()) return row.status();
  // Unreachable: the bytes hold no `)` that closes the row.
  return Status::Error("pxf: DatasetReader: row at offset " + std::to_string(start) +
                       " neither closes nor fails");
}

// ScanRow finds the next row with the lexer Parse uses, resuming at
// scan_pos_, so a complete token is not lexed again on the next read, and a
// token still open is lexed again only once bytes arrive that could end it
// (Resume): a row costs time linear in its size however many reads it
// arrives in (#168).
// Between rows it skips newlines and comments: `(` starts a row, a directive
// ends the sequence, and anything else is not a row. Inside a row it stops
// at the `)` that closes it, or at a `(` (no value holds one) or a token the
// lexer rejects, which make the row bad. Returns kNeedMore with *start the
// open row's `(`, or -1 between rows, when the buffer ends first.
DatasetReader::RowScan DatasetReader::ScanRow(int* start, int* end) {
  if (!Resume()) {
    *start = row_start_;
    return RowScan::kNeedMore;
  }
  Lexer lx(std::string_view(pending_).substr(scan_pos_), Position{1, 1, scan_pos_});
  lx.SetMaxBytesLiteral(MaxBytesLiteral());
  for (;;) {
    const int from = lx.CurrentPos().offset;
    const Token tok = lx.Next();
    const int tok_end = lx.CurrentPos().offset;
    scanned_ += static_cast<size_t>(tok_end - from);
    if (tok.kind == TokenKind::kEOF || !Complete(tok_end)) {
      if (!src_eof_) Wait(tok.pos.offset);
      *start = row_start_;
      return RowScan::kNeedMore;
    }
    const int at = tok.pos.offset;
    if (tok.kind == TokenKind::kNewline || tok.kind == TokenKind::kComment) {
      scan_pos_ = tok_end;
      continue;
    }
    if (row_start_ < 0) {
      switch (tok.kind) {
        case TokenKind::kLParen:
          row_start_ = at;
          scan_pos_ = tok_end;
          continue;
        case TokenKind::kAtType:
        case TokenKind::kAtDirective:
        case TokenKind::kAtDataset:
        case TokenKind::kAtProto:
          *start = at;
          return RowScan::kEnd;
        default:
          *start = at;
          return RowScan::kNotRow;
      }
    }
    switch (tok.kind) {
      case TokenKind::kRParen:
        *start = row_start_;
        *end = tok_end;
        row_start_ = -1;
        scan_pos_ = tok_end;
        return RowScan::kRow;
      case TokenKind::kLParen:
      case TokenKind::kIllegal:
        *start = row_start_;
        *end = tok_end;
        return RowScan::kBad;
      default:
        scan_pos_ = tok_end;
    }
  }
}

Status DatasetReader::Pull(size_t n) {
  if (src_eof_) return Status::OK();
  std::string buf(n, '\0');
  src_->read(buf.data(), static_cast<std::streamsize>(n));
  std::streamsize got = src_->gcount();
  if (got > 0) {
    pending_.append(buf.data(), static_cast<size_t>(got));
    // A stream that never ends a row cannot grow the buffer without bound.
    const int max_size = opts_.max_message_size > 0 ? opts_.max_message_size : kMaxMessageSize;
    if (pending_.size() > static_cast<size_t>(max_size)) {
      return Status::Error(
          "dataset input holds " + std::to_string(pending_.size()) +
          " bytes without a row boundary, MaxMessageSize=" + std::to_string(max_size));
    }
  }
  if (src_->eof()) {
    src_eof_ = true;
    return Status::OK();
  }
  if (src_->bad()) return Status::Error("pxf: DatasetReader: istream read error");
  // `fail` without `eof` is unusual; treat as a read error.
  if (src_->fail() && got == 0) return Status::Error("pxf: DatasetReader: istream read failed");
  return Status::OK();
}

// Complete reports whether a token ending at pending_[tok_end] is known
// complete: the source has ended, or a few bytes follow it. The lexer looks
// at most one byte past a token to end it (the `.` of `1.5`, the `p` of a
// precision, the second byte of `µs`), so bytes that arrive later cannot
// change it.
bool DatasetReader::Complete(int tok_end) const {
  constexpr int kLookahead = 4;
  return src_eof_ || tok_end + kLookahead <= static_cast<int>(pending_.size());
}

// Wait records the token at pending_[at], which the scan could not yet take
// as complete. The scan resumes at its start: the spaces before it are
// decided, and lexing them again would cost as much as the token.
void DatasetReader::Wait(int at) {
  waiting_ = true;
  wait_at_ = at;
  wait_from_ = static_cast<int>(pending_.size());
  scan_pos_ = at;
}

// Resume reports whether a scan may lex on: no token is waiting, the source
// has ended, or the bytes read since the waiting token was last lexed could
// end it.
bool DatasetReader::Resume() {
  if (waiting_ && !src_eof_ && !MayEndToken()) {
    wait_from_ = static_cast<int>(pending_.size());
    return false;
  }
  waiting_ = false;
  return true;
}

namespace {

// kShortWait is the length under which a waiting token is lexed again
// whatever bytes arrive. What may still change a token this short is a byte
// or two of the lexer's lookahead (`""` opening `"""`, `b` opening `b"`, `/`
// opening a comment, `-in` reading `-inf`, a multi-byte rune cut in half, no
// token at all yet), and lexing a few bytes again costs nothing.
constexpr int kShortWait = 8;

bool IsDigitByte(char c) {
  return c >= '0' && c <= '9';
}

// As the lexer's IsIdentPart.
bool IsIdentByte(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || IsDigitByte(c) || c == '.';
}

// The bytes that end a timestamp, as the lexer's LexTimestamp has them.
bool EndsRun(char c) {
  return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == ',' || c == ']' || c == '}' ||
         c == ')' || c == '#';
}

}  // namespace

// MayEndToken reports whether the bytes read since the waiting token was
// last lexed could end it: a closing quote for a string (one not escaped by
// an odd run of backslashes) or a newline, which a simple string cannot
// hold; `"""` for a triple-quoted one; a quote or newline for a bytes
// literal, or enough characters that the lexer refuses it
// (max_bytes_literal_length); a newline for a line comment; `*/` for a block
// comment; a byte that ends a run, or a comment opener, for a timestamp; and
// for any other token a byte that cannot continue an identifier, number or
// duration. Each of those does end the token, so a token is lexed again only
// once it is short or decided, and the scan stays linear. It decides only
// when the lexer looks again, never what the token is. Ported from
// protowire-go's mayEndToken.
bool DatasetReader::MayEndToken() const {
  const std::string_view buf = pending_;
  const int at = wait_at_;
  const int from = wait_from_;
  if (from - at < kShortWait) return true;
  const std::string_view tok = buf.substr(at);
  const std::string_view fresh = buf.substr(from);
  if (tok.substr(0, 3) == "\"\"\"") {
    return buf.substr(std::max(from - 2, at + 3)).find("\"\"\"") != std::string_view::npos;
  }
  if (tok[0] == '"') {
    for (size_t i = 0; i < fresh.size(); ++i) {
      if (fresh[i] == '\n') return true;  // a simple string cannot hold one
      if (fresh[i] != '"') continue;
      int backslashes = 0;
      for (int j = from + static_cast<int>(i) - 1; j > at && buf[j] == '\\'; --j) ++backslashes;
      if (backslashes % 2 == 0) return true;
    }
    return false;
  }
  if (tok.substr(0, 2) == "b\"") {
    // The lexer checks the cap before each character it reads past the
    // opening quote, the last one included.
    const int chars = static_cast<int>(buf.size()) - 1 - (at + 2);
    const int limit = MaxBytesLiteral();
    return fresh.find_first_of("\"\n") != std::string_view::npos ||
           (limit > 0 && chars / 4 * 3 > limit);
  }
  if (tok[0] == '#' || tok.substr(0, 2) == "//") return fresh.find('\n') != std::string_view::npos;
  if (tok.substr(0, 2) == "/*") {
    return buf.substr(std::max(from - 1, at + 2)).find("*/") != std::string_view::npos;
  }
  if (IsDigitByte(tok[0]) && IsDigitByte(tok[1]) && IsDigitByte(tok[2]) && IsDigitByte(tok[3]) &&
      tok[4] == '-') {
    // A timestamp runs to a byte that ends a run, or to `//` or `/*`, whose
    // '/' may be the last byte checked before.
    for (size_t i = std::max(from - 1, at); i < buf.size(); ++i) {
      if (EndsRun(buf[i])) return true;
      if (buf[i] == '/' && i + 1 < buf.size() && (buf[i + 1] == '/' || buf[i + 1] == '*')) {
        return true;
      }
    }
    return false;
  }
  for (const char c : fresh) {
    if (!(IsIdentByte(c) || c == '+' || c == '-' || static_cast<unsigned char>(c) >= 0x80)) {
      return true;
    }
  }
  return false;
}

// ScanHeader finds the end of the first `@dataset TYPE ( cols )` header with
// the lexer Parse uses: the offset just past the `)` that closes the column
// list, the first `)` after an @dataset token. An @proto body is raw
// protobuf source, which Parse skips by matching its braces, so the scan
// does too. Returns kNeedMore when the buffer ends first, and kBad at a
// token the lexer rejects or an @proto body that can never close. Like
// ScanRow it resumes where the last read left it, so a header costs time
// linear in its size however many reads it arrives in (#168).
DatasetReader::RowScan DatasetReader::ScanHeader(int* end) {
  if (!hdr_in_body_ && !Resume()) return RowScan::kNeedMore;
  for (;;) {
    if (hdr_in_body_) {
      BraceMatch::State st = BraceMatch::State::kOpen;
      const int i = hdr_body_->Scan(pending_, scan_pos_, &st);
      scanned_ += static_cast<size_t>(i - scan_pos_);
      scan_pos_ = i;
      if (st == BraceMatch::State::kOpen) return RowScan::kNeedMore;
      if (st == BraceMatch::State::kFailed) return RowScan::kBad;
      hdr_in_body_ = false;
      ++scan_pos_;  // the closing `}`
    }
    Lexer lx(std::string_view(pending_).substr(scan_pos_), Position{1, 1, scan_pos_});
    lx.SetMaxBytesLiteral(MaxBytesLiteral());
    for (;;) {
      const int from = lx.CurrentPos().offset;
      const Token tok = lx.Next();
      const int tok_end = lx.CurrentPos().offset;
      scanned_ += static_cast<size_t>(tok_end - from);
      if (tok.kind == TokenKind::kEOF && src_eof_) return RowScan::kNeedMore;
      if (tok.kind == TokenKind::kEOF || !Complete(tok_end)) {
        Wait(tok.pos.offset);
        return RowScan::kNeedMore;
      }
      if (tok.kind == TokenKind::kIllegal) return RowScan::kBad;
      scan_pos_ = tok_end;
      if (tok.kind == TokenKind::kNewline || tok.kind == TokenKind::kComment) {
        // Parse skips these between @proto, its name and `{`.
        continue;
      }
      if (hdr_in_dataset_) {
        if (tok.kind == TokenKind::kRParen) {
          *end = tok_end;
          return RowScan::kRow;
        }
        continue;
      }
      if (tok.kind == TokenKind::kAtDataset) {
        hdr_in_dataset_ = true;
        hdr_proto_ = 0;
      } else if (tok.kind == TokenKind::kAtProto) {
        hdr_proto_ = 1;
      } else if (tok.kind == TokenKind::kIdent && hdr_proto_ == 1) {
        hdr_proto_ = 2;
      } else if (tok.kind == TokenKind::kLBrace && hdr_proto_ > 0) {
        hdr_proto_ = 0;
        hdr_in_body_ = true;
        hdr_body_ = std::make_unique<BraceMatch>();
        break;  // the body is matched as bytes, not lexed
      } else {
        hdr_proto_ = 0;
      }
    }
  }
}

Status DatasetReader::ReadHeader() {
  // The header is parsed under the caller's limits, as the rows are and as
  // protowire-go parses both (#160).
  ParseOptions popts;
  popts.max_nesting_depth = opts_.max_nesting_depth;
  popts.max_bytes_literal_length = opts_.max_bytes_literal_length;
  for (;;) {
    int header_end = 0;
    const RowScan scan = ScanHeader(&header_end);
    if (scan == RowScan::kBad) {
      // Every token before the one the scan stopped at is complete, so
      // Parse fails at or before it.
      auto doc = Parse(pending_, popts);
      if (!doc.ok()) return doc.status();
      // Unreachable: Parse reads what the scan read.
      return Status::Error("pxf: DatasetReader: the @dataset header neither ends nor fails");
    }
    if (scan == RowScan::kRow) {
      std::string_view header(pending_.data(), static_cast<size_t>(header_end));
      auto doc = Parse(header, popts);
      if (!doc.ok()) return doc.status();
      if (doc->datasets.empty()) {
        return Status::Error("pxf: no @dataset directive in stream");
      }
      type_ = std::move(doc->datasets[0].type);
      columns_ = std::move(doc->datasets[0].columns);
      directives_ = std::move(doc->directives);
      Consume(static_cast<size_t>(header_end));
      return Status::OK();
    }
    if (src_eof_) {
      // The input ended without a complete header. A malformed document (a
      // header cut short, a literal that never closes) reports what Parse
      // reports for it; only a well-formed one has no dataset.
      auto doc = Parse(pending_, popts);
      if (!doc.ok()) return doc.status();
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

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/parser.h"

#include <utility>

#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
#include "protowire/detail/rfc3339.h"
#include "protowire/pxf/lexer.h"
#include "protowire/pxf/schema.h"

namespace protowire::pxf {

namespace {

class Parser {
 public:
  explicit Parser(std::string_view input) : lex_(input) { Advance(); }

  StatusOr<Document> ParseDocument();

 private:
  void Advance();
  std::vector<Comment> FlushComments();

  StatusOr<EntryPtr> ParseEntry(bool allow_map_entry);
  StatusOr<ValuePtr> ParseValue();
  StatusOr<ValuePtr> ParseList();
  StatusOr<ValuePtr> ParseBlockVal();
  StatusOr<std::vector<EntryPtr>> ParseBody();
  StatusOr<Directive> ParseDirective(int* end_offset);
  StatusOr<DatasetDirective> ParseTableDirective(int* end_offset);
  StatusOr<DatasetRow> ParseTableRow(int expected, int* end_offset);
  StatusOr<ProtoDirective> ParseProtoDirective(int* end_offset);
  StatusOr<std::optional<ValuePtr>> ParseRowCell();
  TokenKind PeekKind();

  Lexer lex_;
  Token current_;
  std::vector<Comment> comments_;
};

// FindMatchingBrace returns the offset of the `}` that matches the `{`
// at openOffset, mirroring lexer string/comment handling so braces
// inside literals don't confuse the brace count. Returns -1 on
// unterminated input. Mirrors protowire-go's parser.findMatchingBrace.
int FindMatchingBrace(std::string_view input, int open_offset);
bool ContainsDot(std::string_view s);

void Parser::Advance() {
  for (;;) {
    current_ = lex_.Next();
    if (current_.kind == TokenKind::kNewline) continue;
    if (current_.kind == TokenKind::kComment) {
      comments_.push_back(Comment{current_.pos, std::string(current_.value)});
      continue;
    }
    return;
  }
}

std::vector<Comment> Parser::FlushComments() {
  std::vector<Comment> out;
  out.swap(comments_);
  return out;
}

StatusOr<Document> Parser::ParseDocument() {
  Document doc;
  doc.leading_comments = FlushComments();

  // Top-of-document directives. @type, @<name>, and @dataset may interleave
  // in any order; @type populates type_url, @<name> appends to directives,
  // @dataset appends to tables. body_offset tracks the byte immediately
  // after the last directive's last token so consumers (e.g. chameleon)
  // can hash from there; it stays 0 when no directives are present.
  bool saw_type = false;
  Position first_table_pos{};
  bool has_table = false;
  for (;;) {
    if (current_.kind == TokenKind::kAtType) {
      if (has_table) {
        return Status::Error(current_.pos.line,
                             current_.pos.column,
                             "@dataset directive cannot coexist with @type; the @dataset header "
                             "declares the document's type (draft §3.4.4)");
      }
      saw_type = true;
      Advance();
      if (current_.kind != TokenKind::kIdent) {
        return Status::Error(
            current_.pos.line, current_.pos.column, "expected type name after @type");
      }
      doc.type_url = std::string(current_.value);
      doc.body_offset = current_.pos.offset + static_cast<int>(current_.value.size());
      Advance();
    } else if (current_.kind == TokenKind::kAtDirective) {
      int end = 0;
      auto d = ParseDirective(&end);
      if (!d.ok()) return d.status();
      doc.directives.push_back(std::move(d).consume());
      doc.body_offset = end;
    } else if (current_.kind == TokenKind::kAtDataset) {
      if (saw_type) {
        return Status::Error(current_.pos.line,
                             current_.pos.column,
                             "@dataset directive cannot coexist with @type; the @dataset header "
                             "declares the document's type (draft §3.4.4)");
      }
      int end = 0;
      auto tbl = ParseTableDirective(&end);
      if (!tbl.ok()) return tbl.status();
      if (!has_table) {
        first_table_pos = tbl->pos;
        has_table = true;
      }
      doc.datasets.push_back(std::move(tbl).consume());
      doc.body_offset = end;
    } else if (current_.kind == TokenKind::kAtProto) {
      int end = 0;
      auto pd = ParseProtoDirective(&end);
      if (!pd.ok()) return pd.status();
      doc.protos.push_back(std::move(pd).consume());
      doc.body_offset = end;
    } else {
      break;
    }
  }

  // Standalone constraint (draft §3.4.4): a document containing any
  // @dataset directive MUST NOT also carry top-level field entries; the
  // @dataset header IS the document's type declaration.
  if (has_table && current_.kind != TokenKind::kEOF) {
    return Status::Error(
        first_table_pos.line,
        first_table_pos.column,
        "@dataset directive cannot coexist with top-level field entries; the document's "
        "payload is the @dataset rows (draft §3.4.4)");
  }

  while (current_.kind != TokenKind::kEOF) {
    // Top-level: only field_entry is allowed. The document represents a
    // proto message, never a map<K,V>; map_entry (`:` form) is reserved
    // for the inside of a '{ ... }' block. See docs/grammar.ebnf -> document.
    auto e = ParseEntry(/*allow_map_entry=*/false);
    if (!e.ok()) return e.status();
    doc.entries.push_back(std::move(e).consume());
  }
  return doc;
}

StatusOr<EntryPtr> Parser::ParseEntry(bool allow_map_entry) {
  auto leading = FlushComments();
  Position pos = current_.pos;
  // Accept the @type directive inside a block as an Assignment whose key is
  // the literal "@type". Decoders that care (e.g. Any sugar) recognise it.
  if (current_.kind == TokenKind::kAtType) {
    Advance();
    if (current_.kind != TokenKind::kEquals) {
      return Status::Error(current_.pos.line, current_.pos.column, "expected '=' after @type");
    }
    Advance();
    auto v = ParseValue();
    if (!v.ok()) return v.status();
    auto a = std::make_unique<Assignment>();
    a->pos = pos;
    a->key = "@type";
    a->value = std::move(v).consume();
    a->leading_comments = std::move(leading);
    return EntryPtr(std::move(a));
  }
  if (current_.kind != TokenKind::kIdent && current_.kind != TokenKind::kString &&
      current_.kind != TokenKind::kInt) {
    return Status::Error(pos.line,
                         pos.column,
                         std::string("expected identifier, string, or integer, got ") +
                             TokenKindName(current_.kind));
  }
  TokenKind key_kind = current_.kind;
  std::string key(current_.value);
  Advance();

  switch (current_.kind) {
    case TokenKind::kEquals: {
      // `=` denotes a field assignment on a proto message; the key must
      // be an identifier. Map-style keys (string / integer) are only
      // valid with `:`.
      if (key_kind != TokenKind::kIdent) {
        return Status::Error(
            pos.line,
            pos.column,
            std::string("field assignment with '=' requires an identifier key, got ") +
                TokenKindName(key_kind) + " (\"" + key + "\"); use ':' for map entries");
      }
      Advance();
      auto v = ParseValue();
      if (!v.ok()) return v.status();
      auto a = std::make_unique<Assignment>();
      a->pos = pos;
      a->key = std::move(key);
      a->value = std::move(v).consume();
      a->leading_comments = std::move(leading);
      return EntryPtr(std::move(a));
    }
    case TokenKind::kColon: {
      // Map entry. Only allowed inside a '{ ... }' block, never at
      // document top level.
      if (!allow_map_entry) {
        return Status::Error(pos.line,
                             pos.column,
                             "map entry (':' form) is only allowed inside a '{ … }' block; "
                             "use '=' for top-level field assignments");
      }
      Advance();
      auto v = ParseValue();
      if (!v.ok()) return v.status();
      auto m = std::make_unique<MapEntry>();
      m->pos = pos;
      m->key = std::move(key);
      m->value = std::move(v).consume();
      m->leading_comments = std::move(leading);
      return EntryPtr(std::move(m));
    }
    case TokenKind::kLBrace: {
      // `{ ... }` denotes a submessage field; same identifier-only rule
      // as `=` applies.
      if (key_kind != TokenKind::kIdent) {
        return Status::Error(pos.line,
                             pos.column,
                             std::string("submessage block requires an identifier key, got ") +
                                 TokenKindName(key_kind) + " (\"" + key + "\")");
      }
      Advance();
      auto entries = ParseBody();
      if (!entries.ok()) return entries.status();
      auto b = std::make_unique<Block>();
      b->pos = pos;
      b->name = std::move(key);
      b->entries = std::move(entries).consume();
      b->leading_comments = std::move(leading);
      return EntryPtr(std::move(b));
    }
    default:
      return Status::Error(current_.pos.line,
                           current_.pos.column,
                           std::string("expected '=', ':', or '{' after \"") + key + "\", got " +
                               TokenKindName(current_.kind));
  }
}

StatusOr<ValuePtr> Parser::ParseValue() {
  Position pos = current_.pos;
  switch (current_.kind) {
    case TokenKind::kString: {
      auto v = std::make_unique<StringVal>();
      v->pos = pos;
      v->value = std::string(current_.value);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kInt: {
      auto v = std::make_unique<IntVal>();
      v->pos = pos;
      v->raw = std::string(current_.value);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kFloat: {
      auto v = std::make_unique<FloatVal>();
      v->pos = pos;
      v->raw = std::string(current_.value);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kBool: {
      auto v = std::make_unique<BoolVal>();
      v->pos = pos;
      v->value = (current_.value == "true");
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kBytes: {
      auto v = std::make_unique<BytesVal>();
      v->pos = pos;
      auto decoded = detail::Base64DecodeStd(current_.value);
      if (decoded.has_value()) v->value = std::move(*decoded);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kTimestamp: {
      auto v = std::make_unique<TimestampVal>();
      v->pos = pos;
      v->raw = std::string(current_.value);
      auto t = detail::ParseRFC3339(current_.value);
      if (t.has_value()) {
        v->seconds = t->seconds;
        v->nanos = t->nanos;
      }
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kDuration: {
      auto v = std::make_unique<DurationVal>();
      v->pos = pos;
      v->raw = std::string(current_.value);
      auto d = detail::ParseDuration(current_.value);
      if (d.has_value()) {
        v->seconds = d->seconds;
        v->nanos = d->nanos;
      }
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kNull: {
      auto v = std::make_unique<NullVal>();
      v->pos = pos;
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kIdent: {
      auto v = std::make_unique<IdentVal>();
      v->pos = pos;
      v->name = std::string(current_.value);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kLBracket:
      return ParseList();
    case TokenKind::kLBrace:
      return ParseBlockVal();
    default:
      return Status::Error(
          pos.line, pos.column, std::string("expected value, got ") + TokenKindName(current_.kind));
  }
}

StatusOr<ValuePtr> Parser::ParseList() {
  Position pos = current_.pos;
  Advance();  // [
  auto list = std::make_unique<ListVal>();
  list->pos = pos;
  while (current_.kind != TokenKind::kRBracket && current_.kind != TokenKind::kEOF) {
    auto e = ParseValue();
    if (!e.ok()) return e.status();
    list->elements.push_back(std::move(e).consume());
    if (current_.kind == TokenKind::kComma) Advance();
  }
  if (current_.kind != TokenKind::kRBracket) {
    return Status::Error(current_.pos.line,
                         current_.pos.column,
                         std::string("expected ']', got ") + TokenKindName(current_.kind));
  }
  Advance();
  return ValuePtr(std::move(list));
}

StatusOr<ValuePtr> Parser::ParseBlockVal() {
  Position pos = current_.pos;
  Advance();  // {
  auto entries = ParseBody();
  if (!entries.ok()) return entries.status();
  auto bv = std::make_unique<BlockVal>();
  bv->pos = pos;
  bv->entries = std::move(entries).consume();
  return ValuePtr(std::move(bv));
}

StatusOr<std::vector<EntryPtr>> Parser::ParseBody() {
  std::vector<EntryPtr> out;
  while (current_.kind != TokenKind::kRBrace && current_.kind != TokenKind::kEOF) {
    // Inside a '{ ... }' block both forms are accepted; the schema layer
    // disambiguates submessage vs map<K,V>.
    auto e = ParseEntry(/*allow_map_entry=*/true);
    if (!e.ok()) return e.status();
    out.push_back(std::move(e).consume());
  }
  if (current_.kind != TokenKind::kRBrace) {
    return Status::Error(current_.pos.line,
                         current_.pos.column,
                         std::string("expected '}', got ") + TokenKindName(current_.kind));
  }
  Advance();
  return out;
}

// PeekKind returns the kind of the next significant token (skipping
// newlines and comments) without consuming it or disturbing the pending
// comment list. Used by ParseDirective to disambiguate "this IDENT is a
// directive prefix" from "this IDENT is a body field key".
//
// Implementation: we can't cheaply rewind the lexer's internal state
// (line/column update with every Advance), so we save and restore it
// around a single Advance() — same approach as protowire-go.
TokenKind Parser::PeekKind() {
  // Save lexer + parser state.
  Lexer saved_lex = lex_;
  Token saved_current = current_;
  size_t n_comments = comments_.size();
  Advance();
  TokenKind next = current_.kind;
  // Restore.
  lex_ = saved_lex;
  current_ = saved_current;
  comments_.resize(n_comments);
  return next;
}

// ParseDirective reads `@<name> *(<prefix-id>) [{ ... }]`. kAtDirective
// is current on entry. Writes the byte offset immediately past the
// directive's last token (the `}` for block form, the last prefix
// identifier for bare form, or `@<name>` if neither is present) to
// *end_offset. Mirrors protowire-go's parser.parseDirective.
StatusOr<Directive> Parser::ParseDirective(int* end_offset) {
  auto leading = FlushComments();
  Position at_pos = current_.pos;
  std::string name(current_.value);
  if (IsFutureReservedDirective(name)) {
    return Status::Error(
        at_pos.line,
        at_pos.column,
        std::string("@") + name +
            " is a spec-reserved directive name with no v1 semantics (draft §3.4.6)");
  }
  Directive d;
  d.pos = at_pos;
  d.name = name;
  d.leading_comments = std::move(leading);
  int eo = at_pos.offset + 1 + static_cast<int>(name.size());  // `@` + name
  Advance();                                                   // consume kAtDirective

  // Zero-or-more prefix identifiers. PXF is whitespace-insignificant,
  // so we can't end the prefix run at a newline. One-token lookahead
  // disambiguates: an IDENT followed by `=` or `:` is a body field
  // key, not a directive prefix.
  while (current_.kind == TokenKind::kIdent) {
    TokenKind next = PeekKind();
    if (next == TokenKind::kEquals || next == TokenKind::kColon) {
      // p.current is the first body entry's key; leave it for the body
      // parser.
      break;
    }
    d.prefixes.emplace_back(current_.value);
    eo = current_.pos.offset + static_cast<int>(current_.value.size());
    Advance();
  }

  // Back-compat: a single prefix identifier populates the legacy `type`
  // field, matching v0.72.0's single-Type shape so existing consumers
  // (e.g. chameleon's `@header T { ... }` reader) keep working.
  if (d.prefixes.size() == 1) d.type = d.prefixes[0];

  // Optional inline block. Use ParseBlockVal to validate inner content
  // (string / brace / comment well-formedness); then slice the raw
  // bytes between `{` and `}` from the input for Body.
  if (current_.kind == TokenKind::kLBrace) {
    int open = current_.pos.offset;
    auto bv = ParseBlockVal();
    if (!bv.ok()) return bv.status();
    int close = FindMatchingBrace(lex_.Input(), open);
    if (close < 0) {
      // ParseBlockVal succeeded so a matching brace must exist; this is
      // defensive belt-and-braces.
      return Status::Error(
          d.pos.line, d.pos.column, std::string("directive @") + d.name + ": unmatched '{'");
    }
    d.body = std::string(lex_.Input().substr(open + 1, close - (open + 1)));
    d.has_body = true;
    eo = close + 1;
  }
  *end_offset = eo;
  return d;
}

// ParseTableDirective reads `@dataset <type> ( col1, col2, ... ) row*`.
// kAtDataset is current on entry. Writes the byte offset immediately
// past the directive's last token to *end_offset. See draft §3.4.4.
StatusOr<DatasetDirective> Parser::ParseTableDirective(int* end_offset) {
  auto leading = FlushComments();
  DatasetDirective tbl;
  tbl.pos = current_.pos;
  tbl.leading_comments = std::move(leading);
  Advance();  // consume @dataset

  // Optional row message type. MAY be omitted when an anonymous @proto
  // directive precedes the dataset (draft §3.4.4 Anonymous binding).
  if (current_.kind == TokenKind::kIdent) {
    tbl.type = std::string(current_.value);
    Advance();
  }

  // Required: column list in `( ... )`. At least one column.
  if (current_.kind != TokenKind::kLParen) {
    return Status::Error(current_.pos.line,
                         current_.pos.column,
                         std::string("expected '(' to start @dataset column list, got ") +
                             TokenKindName(current_.kind));
  }
  Advance();  // consume (

  if (current_.kind != TokenKind::kIdent) {
    return Status::Error(current_.pos.line,
                         current_.pos.column,
                         std::string("@dataset column list must contain at least one field name, "
                                     "got ") +
                             TokenKindName(current_.kind));
  }
  for (;;) {
    if (current_.kind != TokenKind::kIdent) {
      return Status::Error(
          current_.pos.line,
          current_.pos.column,
          std::string("expected column field name, got ") + TokenKindName(current_.kind));
    }
    std::string col_name(current_.value);
    // v1: column entries are unqualified field names; dotted paths
    // reserved for a future revision.
    if (ContainsDot(col_name)) {
      return Status::Error(current_.pos.line,
                           current_.pos.column,
                           std::string("@dataset column \"") + col_name +
                               "\": dotted column paths are not supported in v1 (draft §3.4.4)");
    }
    tbl.columns.push_back(std::move(col_name));
    Advance();
    if (current_.kind == TokenKind::kComma) {
      Advance();
      continue;
    }
    if (current_.kind == TokenKind::kRParen) break;
    return Status::Error(current_.pos.line,
                         current_.pos.column,
                         std::string("expected ',' or ')' in @dataset column list, got ") +
                             TokenKindName(current_.kind));
  }
  int eo = current_.pos.offset + 1;  // past `)`
  Advance();                         // consume )

  // Zero or more rows.
  while (current_.kind == TokenKind::kLParen) {
    int row_end = 0;
    auto row = ParseTableRow(static_cast<int>(tbl.columns.size()), &row_end);
    if (!row.ok()) return row.status();
    tbl.rows.push_back(std::move(row).consume());
    eo = row_end;
  }
  *end_offset = eo;
  return tbl;
}

// ParseTableRow reads `( cell ( ',' cell )* )` with an arity check
// against `expected`. kLParen is current on entry. Writes the byte
// offset immediately past the closing `)` to *end_offset.
StatusOr<DatasetRow> Parser::ParseTableRow(int expected, int* end_offset) {
  Position pos = current_.pos;
  Advance();  // consume (

  DatasetRow row;
  row.pos = pos;
  row.cells.reserve(expected);

  auto first = ParseRowCell();
  if (!first.ok()) return first.status();
  row.cells.push_back(std::move(first).consume());

  while (current_.kind == TokenKind::kComma) {
    Advance();
    auto cell = ParseRowCell();
    if (!cell.ok()) return cell.status();
    row.cells.push_back(std::move(cell).consume());
  }

  if (current_.kind != TokenKind::kRParen) {
    return Status::Error(
        current_.pos.line,
        current_.pos.column,
        std::string("expected ',' or ')' in @dataset row, got ") + TokenKindName(current_.kind));
  }
  int eo = current_.pos.offset + 1;
  Advance();  // consume )

  if (static_cast<int>(row.cells.size()) != expected) {
    return Status::Error(pos.line,
                         pos.column,
                         std::string("@dataset row has ") + std::to_string(row.cells.size()) +
                             " cells, expected " + std::to_string(expected) + " (column count)");
  }
  *end_offset = eo;
  return row;
}

// ParseProtoDirective reads `@proto <body>` (draft §3.4.5). kAtProto
// is current on entry. Four body shapes are lexically distinguished:
//
//   - anonymous:  `@proto { <message-body> }`
//   - named:      `@proto <dotted-name> { <message-body> }`
//   - source:     `@proto """<proto-source>"""`
//   - descriptor: `@proto b"<base64-FileDescriptorSet>"`
//
// For brace-bounded shapes the body is sliced as raw bytes between
// `{` and the matching `}` (both exclusive); the contents are
// protobuf source and are NOT decoded as PXF entries.
StatusOr<ProtoDirective> Parser::ParseProtoDirective(int* end_offset) {
  auto leading = FlushComments();
  Position at_pos = current_.pos;
  ProtoDirective pd;
  pd.pos = at_pos;
  pd.leading_comments = std::move(leading);
  Advance();  // consume @proto

  auto capture_brace_body = [this, at_pos](const std::string& label,
                                            std::string* body,
                                            int* eo) -> Status {
    int open = current_.pos.offset;
    int close = FindMatchingBrace(lex_.Input(), open);
    if (close < 0) {
      return Status::Error(at_pos.line, at_pos.column, label + ": unmatched '{'");
    }
    *body = std::string(lex_.Input().substr(open + 1, close - (open + 1)));
    // Reposition the lexer past the closing `}` and prime the parser.
    lex_.RepositionTo(close + 1);
    Advance();
    *eo = close + 1;
    return Status::OK();
  };

  switch (current_.kind) {
    case TokenKind::kLBrace: {
      pd.shape = ProtoShape::kAnonymous;
      int eo = 0;
      auto st = capture_brace_body("@proto (anonymous form)", &pd.body, &eo);
      if (!st.ok()) return st;
      *end_offset = eo;
      return pd;
    }
    case TokenKind::kIdent: {
      pd.shape = ProtoShape::kNamed;
      pd.type_name = std::string(current_.value);
      Advance();
      if (current_.kind != TokenKind::kLBrace) {
        return Status::Error(
            current_.pos.line,
            current_.pos.column,
            std::string("expected '{' after @proto ") + pd.type_name + ", got " +
                TokenKindName(current_.kind));
      }
      int eo = 0;
      auto st = capture_brace_body(std::string("@proto ") + pd.type_name, &pd.body, &eo);
      if (!st.ok()) return st;
      *end_offset = eo;
      return pd;
    }
    case TokenKind::kString: {
      pd.shape = ProtoShape::kSource;
      pd.body = std::string(current_.value);
      *end_offset = current_.pos.offset + static_cast<int>(current_.value.size());
      Advance();
      return pd;
    }
    case TokenKind::kBytes: {
      pd.shape = ProtoShape::kDescriptor;
      auto decoded = detail::Base64DecodeStd(current_.value);
      if (!decoded.has_value()) {
        return Status::Error(current_.pos.line,
                             current_.pos.column,
                             "@proto descriptor body: invalid base64");
      }
      pd.body = std::string(decoded->begin(), decoded->end());
      *end_offset = current_.pos.offset + static_cast<int>(current_.value.size()) + 3;  // b" … "
      Advance();
      return pd;
    }
    default:
      return Status::Error(
          current_.pos.line,
          current_.pos.column,
          std::string("expected '{', dotted identifier, triple-quoted string, or b\"...\" after "
                      "@proto, got ") +
              TokenKindName(current_.kind));
  }
}

// ParseRowCell consumes one cell of a @dataset row. Returns nullopt for
// an empty cell (no value between two commas, or at row start/end);
// rejects list / block values per v1 cell-grammar (draft §3.4.4).
StatusOr<std::optional<ValuePtr>> Parser::ParseRowCell() {
  switch (current_.kind) {
    case TokenKind::kComma:
    case TokenKind::kRParen:
      return std::optional<ValuePtr>{std::nullopt};
    case TokenKind::kLBracket:
      return Status::Error(current_.pos.line,
                           current_.pos.column,
                           "@dataset cells cannot contain list values in v1 (draft §3.4.4)");
    case TokenKind::kLBrace:
      return Status::Error(current_.pos.line,
                           current_.pos.column,
                           "@dataset cells cannot contain block values in v1 (draft §3.4.4)");
    default:
      break;
  }
  auto v = ParseValue();
  if (!v.ok()) return v.status();
  return std::optional<ValuePtr>(std::move(v).consume());
}

int FindMatchingBrace(std::string_view input, int open_offset) {
  int depth = 1;
  int i = open_offset + 1;
  int n = static_cast<int>(input.size());
  auto skip_string = [&](int j) -> int {
    if (j + 2 < n && input[j + 1] == '"' && input[j + 2] == '"') {
      int k = j + 3;
      while (k + 2 < n) {
        if (input[k] == '"' && input[k + 1] == '"' && input[k + 2] == '"') return k + 3;
        ++k;
      }
      return -1;
    }
    int k = j + 1;
    while (k < n) {
      if (input[k] == '\\') {
        if (k + 1 >= n) return -1;
        k += 2;
        continue;
      }
      if (input[k] == '"') return k + 1;
      if (input[k] == '\n') return -1;
      ++k;
    }
    return -1;
  };
  auto skip_bytes = [&](int j) -> int {
    int k = j + 2;  // past `b"`
    while (k < n) {
      if (input[k] == '\\') {
        if (k + 1 >= n) return -1;
        k += 2;
        continue;
      }
      if (input[k] == '"') return k + 1;
      if (input[k] == '\n') return -1;
      ++k;
    }
    return -1;
  };
  auto skip_eol = [&](int j) -> int {
    while (j < n && input[j] != '\n') ++j;
    return j;
  };
  while (i < n) {
    char ch = input[i];
    if (ch == '{') {
      ++depth;
      ++i;
    } else if (ch == '}') {
      --depth;
      if (depth == 0) return i;
      ++i;
    } else if (ch == '"') {
      i = skip_string(i);
      if (i < 0) return -1;
    } else if (ch == 'b' && i + 1 < n && input[i + 1] == '"') {
      i = skip_bytes(i);
      if (i < 0) return -1;
    } else if (ch == '#') {
      i = skip_eol(i + 1);
    } else if (ch == '/' && i + 1 < n && input[i + 1] == '/') {
      i = skip_eol(i + 2);
    } else if (ch == '/' && i + 1 < n && input[i + 1] == '*') {
      int j = i + 2;
      bool closed = false;
      while (j + 1 < n) {
        if (input[j] == '*' && input[j + 1] == '/') {
          j += 2;
          closed = true;
          break;
        }
        ++j;
      }
      if (!closed) return -1;
      i = j;
    } else {
      ++i;
    }
  }
  return -1;
}

bool ContainsDot(std::string_view s) {
  for (char c : s)
    if (c == '.') return true;
  return false;
}

}  // namespace

StatusOr<Document> Parse(std::string_view input) {
  return Parser(input).ParseDocument();
}

}  // namespace protowire::pxf

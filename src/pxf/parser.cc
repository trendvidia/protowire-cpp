// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/parser.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "dataset_paths.h"
#include "parser_internal.h"
#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
#include "protowire/detail/go_quote.h"
#include "protowire/detail/rfc3339.h"
#include "protowire/limits.h"
#include "protowire/pxf/lexer.h"
#include "protowire/pxf/schema.h"

namespace protowire::pxf {

namespace {

constexpr const char* kTypeEntryMisplaced =
    "@type is allowed only as the first entry of a block bound to google.protobuf.Any "
    "(draft -01 \"Any Blocks\")";

// Clip shortens a literal for an error message: it can be thousands of digits
// long, and an error is read, not parsed.
std::string Clip(std::string_view s) {
  if (s.size() <= 24) return std::string(s);
  return std::string(s.substr(0, 24)) + "…";
}

std::string WrittenKey(const std::string& key, std::string_view prec) {
  if (prec.empty()) return key;
  return key + "p" + Clip(prec);
}

// The parser mirrors protowire-go's parser.go function for function. In
// tolerant mode, ParseTolerant's, it records an error and recovers at the
// nearest entry or block boundary. Strict mode returns the first error.
class Parser {
 public:
  Parser(std::string_view input, int max_depth, int max_bytes_literal)
      : lex_(input), max_depth_(max_depth) {
    lex_.SetMaxBytesLiteral(max_bytes_literal);
    Advance();
  }

  struct TolerantTag {};
  Parser(TolerantTag, std::string_view input, int max_depth, int max_bytes_literal)
      : lex_(input), max_depth_(max_depth), tolerant_(true) {
    lex_.SetMaxBytesLiteral(max_bytes_literal);
    lex_.SetTolerant(&errs_);
    Advance();
  }

  // Adopts a lexer already part-way through its input, with the token it
  // last produced; Release hands both back.
  Parser(Lexer&& lex, Token current, int max_depth)
      : lex_(std::move(lex)), current_(current), max_depth_(max_depth), for_decoder_(true) {}
  void Release(Lexer* lex, Token* current) {
    *lex = std::move(lex_);
    *current = current_;
  }

  StatusOr<Document> ParseDocument();
  StatusOr<DatasetDirective> ParseTableDirective(int* end_offset);
  StatusOr<DatasetRow> ParseTableRow(int expected, int* end_offset);

  std::vector<Error> TakeErrors() {
    std::stable_sort(errs_.begin(), errs_.end(), [](const Error& a, const Error& b) {
      return a.pos.offset < b.pos.offset;
    });
    return std::move(errs_);
  }
  // Records a failure that reached the top of a tolerant parse.
  void Record(const Status& s) { errs_.push_back(Error{err_pos_, s.message()}); }

 private:
  void Advance();
  std::vector<Comment> FlushComments();
  std::string TakeTrailingComment(const Position& end);

  // Err makes a failure at pos. Status keeps no byte offset, so the last one
  // is kept here for Record.
  Status Err(Position pos, std::string msg) {
    err_pos_ = pos;
    return Status::Error(pos.line, pos.column, std::move(msg));
  }
  // Soft routes a recoverable error: strict mode returns it, tolerant mode
  // records it and returns OK so the call site can recover.
  Status Soft(Position pos, std::string msg) {
    if (!tolerant_) return Err(pos, std::move(msg));
    errs_.push_back(Error{pos, std::move(msg)});
    return Status::OK();
  }

  // TokenEnd is just past the current token: the lexer sits there, since
  // Advance reads only as far as the token it returns.
  Position TokenEnd() const { return lex_.CurrentPos(); }

  // TokenErrMsg renders the current token for "expected X, got Y".
  std::string TokenErrMsg() const {
    if (current_.kind == TokenKind::kIllegal) return std::string(current_.value);
    return std::string(TokenKindName(current_.kind)) + " (" + detail::GoQuote(current_.value) + ")";
  }
  // DatasetTokenMsg is the same for a @dataset header or row. Parse names the
  // token's kind there and the decoder its text, as protowire-go does.
  std::string DatasetTokenMsg() const {
    return for_decoder_ ? TokenErrMsg() : std::string(TokenKindName(current_.kind));
  }

  // A skipped entry is nullopt: tolerant mode has moved past a token that
  // starts none.
  StatusOr<std::optional<EntryPtr>> ParseEntry(int depth, bool allow_map_entry);
  StatusOr<std::optional<EntryPtr>> ParseTypeEntry(bool first);
  StatusOr<ValuePtr> ParseValue(int depth);
  StatusOr<ValuePtr> ParseList(int depth);
  StatusOr<ValuePtr> ParseBlockVal(int depth);
  StatusOr<std::vector<EntryPtr>> ParseBody(int depth, Position* end);
  StatusOr<Directive> ParseDirective(int* end_offset);
  StatusOr<ProtoDirective> ParseProtoDirective(int* end_offset);
  StatusOr<std::optional<ValuePtr>> ParseRowCell();
  TokenKind PeekKind();
  Position SkipBalanced(TokenKind open, TokenKind close);
  bool CurrentStartsEntry();
  void SyncTopLevel();
  // A ')' is in no value, so tolerant mode reads one inside a list or block
  // as the end of the @dataset row the value is a cell of. Strict mode never
  // stops there; the ')' is its error.
  bool TolerantRowClose() const { return tolerant_ && current_.kind == TokenKind::kRParen; }

  ValuePtr BadValue(Position pos) {
    auto b = std::make_unique<BadVal>();
    b->pos = pos;
    b->end = pos;
    return ValuePtr(std::move(b));
  }
  template <class T>
  std::unique_ptr<T> Literal(Position pos) {
    auto v = std::make_unique<T>();
    v->pos = pos;
    v->end = TokenEnd();
    return v;
  }

  Lexer lex_;
  Token current_;
  std::vector<Comment> comments_;
  int max_depth_ = kMaxNestingDepth;
  bool for_decoder_ = false;
  bool tolerant_ = false;
  std::vector<Error> errs_;
  Position err_pos_;
  // Just past the last significant token consumed: where a missing value
  // would have been typed, so where a BadVal anchors.
  Position prev_end_;
};

// FindMatchingBrace returns the offset of the `}` matching the `{` at
// open_offset, or -1 if there is none. Mirrors protowire-go's braceMatch, so
// braces inside literals and comments don't count.
int FindMatchingBrace(std::string_view input, int open_offset);

void Parser::Advance() {
  prev_end_ = lex_.CurrentPos();
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

// TakeTrailingComment removes and returns the comment written after a value on
// its line. A newline ends every line, so there is at most one and it is the
// first pending. Empty when that one starts on a later line, a leading comment
// of the next entry, or before the value.
std::string Parser::TakeTrailingComment(const Position& end) {
  if (comments_.empty()) return {};
  const Comment& c = comments_.front();
  if (c.pos.line != end.line || c.pos.offset < end.offset) return {};
  std::string text = c.text;
  comments_.erase(comments_.begin());
  return text;
}

// SkipBalanced consumes the open token it stands on through its matching close
// or EOF, and returns the position just past the close. Tolerant recovery for
// what is too deep to descend into.
Position Parser::SkipBalanced(TokenKind open, TokenKind close) {
  int depth = 0;
  for (;;) {
    if (current_.kind == open) {
      ++depth;
    } else if (current_.kind == close) {
      if (--depth <= 0) {
        Position end = TokenEnd();
        Advance();
        return end;
      }
    } else if (current_.kind == TokenKind::kEOF) {
      return current_.pos;
    }
    Advance();
  }
}

// CurrentStartsEntry reports whether the current token is the key of a new
// entry, not part of what is being parsed.
bool Parser::CurrentStartsEntry() {
  switch (current_.kind) {
    case TokenKind::kIdent:
    case TokenKind::kString:
    case TokenKind::kInt:
    case TokenKind::kBool: {
      TokenKind next = PeekKind();
      return next == TokenKind::kEquals || next == TokenKind::kColon || next == TokenKind::kLBrace;
    }
    default:
      return false;
  }
}

// SyncTopLevel advances to a directive, a token that can start a body entry,
// or EOF.
void Parser::SyncTopLevel() {
  for (;;) {
    switch (current_.kind) {
      case TokenKind::kEOF:
      case TokenKind::kAtType:
      case TokenKind::kAtDirective:
      case TokenKind::kAtDataset:
      case TokenKind::kAtProto:
      case TokenKind::kIdent:
      case TokenKind::kString:
      case TokenKind::kInt:
        return;
      default:
        Advance();
    }
  }
}

StatusOr<Document> Parser::ParseDocument() {
  Document doc;
  doc.leading_comments = FlushComments();

  // Top-of-document directives. @type and @<name> may interleave in any
  // order. body_offset tracks the byte after the last directive's last token
  // so consumers (e.g. chameleon) can hash from there; it stays 0 when there
  // are no directives.
  for (bool in_directives = true; in_directives;) {
    switch (current_.kind) {
      case TokenKind::kAtType: {
        Advance();
        bool missing_name = current_.kind != TokenKind::kIdent;
        // A dangling `@type` with the body right after it: an identifier
        // followed by '=', ':' or '{' is the first entry's key.
        if (!missing_name && tolerant_ && CurrentStartsEntry()) missing_name = true;
        if (missing_name) {
          if (Status s = Soft(current_.pos,
                              std::string("expected type name after @type, got ") +
                                  TokenKindName(current_.kind));
              !s.ok()) {
            return s;
          }
          SyncTopLevel();
          continue;
        }
        doc.type_url = std::string(current_.value);
        doc.body_offset = current_.pos.offset + static_cast<int>(current_.value.size());
        Advance();
        break;
      }
      case TokenKind::kAtDirective: {
        int end = 0;
        auto d = ParseDirective(&end);
        if (!d.ok()) {
          if (!tolerant_) return d.status();
          Record(d.status());
          SyncTopLevel();
          continue;
        }
        doc.directives.push_back(std::move(d).consume());
        doc.body_offset = end;
        break;
      }
      case TokenKind::kAtDataset: {
        int end = 0;
        auto tbl = ParseTableDirective(&end);
        if (!tbl.ok()) {
          if (!tolerant_) return tbl.status();
          Record(tbl.status());
          SyncTopLevel();
          continue;
        }
        doc.datasets.push_back(std::move(tbl).consume());
        doc.body_offset = end;
        break;
      }
      case TokenKind::kAtProto: {
        int end = 0;
        auto pd = ParseProtoDirective(&end);
        if (!pd.ok()) {
          if (!tolerant_) return pd.status();
          Record(pd.status());
          SyncTopLevel();
          continue;
        }
        doc.protos.push_back(std::move(pd).consume());
        doc.body_offset = end;
        break;
      }
      default:
        in_directives = false;
    }
  }

  // Standalone constraint (draft §3.4.4): a document with any @dataset MUST
  // NOT also carry @type or top-level field entries; the @dataset header IS
  // the document's type declaration.
  if (!doc.datasets.empty()) {
    if (!doc.type_url.empty()) {
      if (Status s = Soft(doc.datasets[0].pos,
                          "@dataset directive cannot coexist with @type; the @dataset header "
                          "declares the document's type (draft §3.4.4)");
          !s.ok()) {
        return s;
      }
    }
    if (current_.kind != TokenKind::kEOF) {
      if (Status s = Soft(current_.pos,
                          "@dataset directive cannot coexist with top-level field entries; the "
                          "document's payload is the @dataset rows (draft §3.4.4)");
          !s.ok()) {
        return s;
      }
    }
  }

  while (current_.kind != TokenKind::kEOF) {
    // Top-level: only field_entry is allowed. The document represents a
    // proto message, never a map<K,V>; map_entry (`:` form) is reserved
    // for the inside of a '{ ... }' block. See docs/grammar.ebnf -> document.
    auto e = ParseEntry(/*depth=*/0, /*allow_map_entry=*/false);
    if (!e.ok()) return e.status();
    auto entry = std::move(e).consume();
    if (entry.has_value()) doc.entries.push_back(std::move(*entry));
  }
  return doc;
}

StatusOr<std::optional<EntryPtr>> Parser::ParseEntry(int depth, bool allow_map_entry) {
  auto leading = FlushComments();
  Position pos = current_.pos;
  // map-key = identifier / string / integer / bool (draft -01 § Entries and
  // Keys; the keyword spelling landed in protowire#284). Only the ':' tail
  // takes a bool key, like an integer key.
  if (current_.kind != TokenKind::kIdent && current_.kind != TokenKind::kString &&
      current_.kind != TokenKind::kInt && current_.kind != TokenKind::kBool) {
    std::string msg = "expected identifier, string, integer, or bool, got " + TokenErrMsg();
    if (!tolerant_) return Err(pos, std::move(msg));
    // Skip the token and let the caller's loop try the next. The leading
    // comments go back, for the next real entry.
    errs_.push_back(Error{pos, std::move(msg)});
    comments_.insert(comments_.begin(), leading.begin(), leading.end());
    Advance();
    return std::optional<EntryPtr>{};
  }
  TokenKind key_kind = current_.kind;
  std::string key(current_.value);
  std::string key_prec(current_.prec);
  Advance();

  switch (current_.kind) {
    case TokenKind::kEquals: {
      // `=` denotes a field assignment on a proto message; the key is an
      // identifier, or a string — the quoted entry name of draft -01
      // §3.13, which the grammar accepts everywhere and the schema layer
      // restricts to keyed repeated fields' blocks. An integer key is
      // only valid with `:`. Tolerant mode keeps the entry as an assignment.
      if (key_kind == TokenKind::kInt || key_kind == TokenKind::kBool) {
        if (Status s =
                Soft(pos,
                     std::string("field assignment with '=' requires an identifier or string key, "
                                 "got ") +
                         TokenKindName(key_kind) + " (" +
                         detail::GoQuote(WrittenKey(key, key_prec)) + "); use ':' for map entries");
            !s.ok()) {
          return s;
        }
      }
      Advance();
      auto v = ParseValue(depth);
      if (!v.ok()) return v.status();
      auto a = std::make_unique<Assignment>();
      a->pos = pos;
      a->value = std::move(v).consume();
      a->end = ValueSpan(a->value).second;
      a->key = std::move(key);
      a->key_quoted = (key_kind == TokenKind::kString);
      a->leading_comments = std::move(leading);
      a->trailing_comment = TakeTrailingComment(a->end);
      return std::optional<EntryPtr>(EntryPtr(std::move(a)));
    }
    case TokenKind::kColon: {
      // Map entry. Only allowed inside a '{ ... }' block, never at document
      // top level. Tolerant mode keeps the entry as a map entry.
      if (!allow_map_entry) {
        if (Status s = Soft(pos,
                            "map entry (':' form) is only allowed inside a '{ … }' block; "
                            "use '=' for top-level field assignments");
            !s.ok()) {
          return s;
        }
      }
      // An integer key takes no precision suffix (draft -01 ABNF,
      // protowire#406). MapEntry has nowhere to keep one.
      if (!key_prec.empty()) {
        if (Status s = Soft(pos,
                            "map key " + key + ": a key cannot carry a precision suffix (p" +
                                Clip(key_prec) + ")");
            !s.ok()) {
          return s;
        }
      }
      Advance();
      auto v = ParseValue(depth);
      if (!v.ok()) return v.status();
      auto m = std::make_unique<MapEntry>();
      m->pos = pos;
      m->value = std::move(v).consume();
      m->end = ValueSpan(m->value).second;
      m->key = std::move(key);
      m->key_quoted = (key_kind == TokenKind::kString);
      m->leading_comments = std::move(leading);
      m->trailing_comment = TakeTrailingComment(m->end);
      return std::optional<EntryPtr>(EntryPtr(std::move(m)));
    }
    case TokenKind::kLBrace: {
      // `{ ... }` denotes a submessage field; the same identifier-or-
      // string rule as `=` applies.
      if (key_kind == TokenKind::kInt || key_kind == TokenKind::kBool) {
        if (Status s =
                Soft(pos,
                     std::string("submessage block requires an identifier or string key, got ") +
                         TokenKindName(key_kind) + " (" +
                         detail::GoQuote(WrittenKey(key, key_prec)) + ")");
            !s.ok()) {
          return s;
        }
      }
      auto b = std::make_unique<Block>();
      b->pos = pos;
      b->name_quoted = (key_kind == TokenKind::kString);
      b->name = std::move(key);
      b->leading_comments = std::move(leading);
      if (depth + 1 > max_depth_) {
        if (Status s = Soft(current_.pos,
                            "nesting depth exceeds MaxNestingDepth=" + std::to_string(max_depth_));
            !s.ok()) {
          return s;
        }
        // Too deep to descend; skip the whole block, keep the entry.
        b->end = SkipBalanced(TokenKind::kLBrace, TokenKind::kRBrace);
        return std::optional<EntryPtr>(EntryPtr(std::move(b)));
      }
      Advance();  // consume {
      auto entries = ParseBody(depth + 1, &b->end);
      if (!entries.ok()) return entries.status();
      b->entries = std::move(entries).consume();
      return std::optional<EntryPtr>(EntryPtr(std::move(b)));
    }
    default: {
      if (Status s = Soft(current_.pos,
                          "expected '=', ':', or '{' after " + detail::GoQuote(key) + ", got " +
                              TokenKindName(current_.kind));
          !s.ok()) {
        return s;
      }
      // A dangling key, the shape completion fires on. Keep it with a BadVal
      // just past the key so tooling sees it, and leave the current token: it
      // is most likely the next entry's key.
      auto a = std::make_unique<Assignment>();
      a->pos = pos;
      a->end = prev_end_;
      a->key = std::move(key);
      a->value = BadValue(prev_end_);
      a->leading_comments = std::move(leading);
      return std::optional<EntryPtr>(EntryPtr(std::move(a)));
    }
  }
}

StatusOr<ValuePtr> Parser::ParseValue(int depth) {
  Position pos = current_.pos;

  if (tolerant_ && CurrentStartsEntry()) {
    // The current token is the next entry's key, so the value is missing
    // (`key =` dangling mid-edit). Leave it, and anchor just past the
    // separator, where the value belongs.
    errs_.push_back(Error{prev_end_, "missing value"});
    return BadValue(prev_end_);
  }

  switch (current_.kind) {
    case TokenKind::kString: {
      auto v = Literal<StringVal>(pos);
      v->value = std::string(current_.value);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kInt: {
      auto v = Literal<IntVal>(pos);
      v->raw = std::string(current_.value);
      v->prec = std::string(current_.prec);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kFloat: {
      auto v = Literal<FloatVal>(pos);
      v->raw = std::string(current_.value);
      v->prec = std::string(current_.prec);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kBool: {
      auto v = Literal<BoolVal>(pos);
      v->value = (current_.value == "true");
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kBytes: {
      // The lexer has already checked this, outside tolerant mode.
      size_t offset = 0;
      auto decoded = detail::Base64DecodeLenient(current_.value, &offset);
      if (!decoded.has_value()) {
        if (Status s = Soft(
                pos, "invalid base64: illegal base64 data at input byte " + std::to_string(offset));
            !s.ok()) {
          return s;
        }
        Advance();
        return BadValue(pos);
      }
      auto v = Literal<BytesVal>(pos);
      v->value = std::move(*decoded);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kTimestamp: {
      auto v = Literal<TimestampVal>(pos);
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
      auto v = Literal<DurationVal>(pos);
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
      auto v = Literal<NullVal>(pos);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kIdent: {
      auto v = Literal<IdentVal>(pos);
      v->name = std::string(current_.value);
      Advance();
      return ValuePtr(std::move(v));
    }
    case TokenKind::kLBracket:
      return ParseList(depth);
    case TokenKind::kLBrace:
      return ParseBlockVal(depth);
    default: {
      // The lexer's own diagnostic beats a generic "got ILLEGAL".
      std::string msg = current_.kind == TokenKind::kIllegal
                            ? std::string(current_.value)
                            : "expected value, got " + TokenErrMsg();
      if (!tolerant_) return Err(pos, std::move(msg));
      errs_.push_back(Error{pos, std::move(msg)});
      switch (current_.kind) {
        case TokenKind::kEOF:
        case TokenKind::kRBrace:
        case TokenKind::kRBracket:
        case TokenKind::kRParen:
          // Closers are the enclosing construct's. A ')' closes the
          // @dataset row around an unclosed cell.
          break;
        default:
          Advance();
      }
      return BadValue(pos);
    }
  }
}

StatusOr<ValuePtr> Parser::ParseList(int depth) {
  Position pos = current_.pos;
  auto list = std::make_unique<ListVal>();
  list->pos = pos;
  if (depth + 1 > max_depth_) {
    if (Status s = Soft(current_.pos,
                        "nesting depth exceeds MaxNestingDepth=" + std::to_string(max_depth_));
        !s.ok()) {
      return s;
    }
    list->end = SkipBalanced(TokenKind::kLBracket, TokenKind::kRBracket);
    return ValuePtr(std::move(list));
  }
  Advance();  // [
  while (current_.kind != TokenKind::kRBracket && current_.kind != TokenKind::kEOF &&
         !TolerantRowClose()) {
    const int before = current_.pos.offset;
    auto e = ParseValue(depth + 1);
    if (!e.ok()) return e.status();
    auto elem = std::move(e).consume();
    // ParseValue recovered without consuming anything: a stray '}' closing
    // an enclosing block, or the next entry's key. Leave the list so the
    // enclosing construct can resynchronize.
    if (tolerant_ && current_.pos.offset == before &&
        std::holds_alternative<std::unique_ptr<BadVal>>(elem)) {
      break;
    }
    list->elements.push_back(std::move(elem));
    if (current_.kind == TokenKind::kComma) Advance();
  }
  if (current_.kind != TokenKind::kRBracket) {
    // Tolerant mode closes the list here, keeping the elements so far.
    if (Status s =
            Soft(current_.pos, std::string("expected ']', got ") + TokenKindName(current_.kind));
        !s.ok()) {
      return s;
    }
    list->end = current_.pos;
    return ValuePtr(std::move(list));
  }
  list->end = TokenEnd();
  Advance();
  return ValuePtr(std::move(list));
}

StatusOr<ValuePtr> Parser::ParseBlockVal(int depth) {
  Position pos = current_.pos;
  auto bv = std::make_unique<BlockVal>();
  bv->pos = pos;
  if (depth + 1 > max_depth_) {
    if (Status s = Soft(current_.pos,
                        "nesting depth exceeds MaxNestingDepth=" + std::to_string(max_depth_));
        !s.ok()) {
      return s;
    }
    bv->end = SkipBalanced(TokenKind::kLBrace, TokenKind::kRBrace);
    return ValuePtr(std::move(bv));
  }
  Advance();  // {
  auto entries = ParseBody(depth + 1, &bv->end);
  if (!entries.ok()) return entries.status();
  bv->entries = std::move(entries).consume();
  return ValuePtr(std::move(bv));
}

// ParseBody is entered just inside a `{` and leaves past its `}`.
StatusOr<std::vector<EntryPtr>> Parser::ParseBody(int depth, Position* end) {
  std::vector<EntryPtr> out;
  while (current_.kind != TokenKind::kRBrace && current_.kind != TokenKind::kEOF &&
         !TolerantRowClose()) {
    if (current_.kind == TokenKind::kAtType) {
      auto e = ParseTypeEntry(out.empty());
      if (!e.ok()) return e.status();
      auto entry = std::move(e).consume();
      if (entry.has_value()) out.push_back(std::move(*entry));
      continue;
    }
    // Inside a '{ ... }' block both forms are accepted; the schema layer
    // disambiguates submessage vs map<K,V>.
    auto e = ParseEntry(depth, /*allow_map_entry=*/true);
    if (!e.ok()) return e.status();
    auto entry = std::move(e).consume();
    if (entry.has_value()) out.push_back(std::move(*entry));
  }
  if (current_.kind != TokenKind::kRBrace) {
    if (!tolerant_) {
      return Err(current_.pos, std::string("expected '}', got ") + TokenKindName(current_.kind));
    }
    // Unclosed: close at EOF, or at the ')' of the @dataset row this is a
    // cell of, with the entries so far.
    errs_.push_back(Error{current_.pos,
                          current_.kind == TokenKind::kRParen
                              ? "unclosed block: expected '}' before ')'"
                              : "unclosed block: expected '}' before end of input"});
    *end = current_.pos;
    return out;
  }
  *end = TokenEnd();
  Advance();
  return out;
}

// ParseTypeEntry parses `@type = "<type-url>"`, the type entry of an Any block
// (draft -01 "Any Blocks"). first reports whether it opens its block, the only
// place it may stand. Tolerant mode records a misplaced or malformed one and
// skips what is left of it.
StatusOr<std::optional<EntryPtr>> Parser::ParseTypeEntry(bool first) {
  auto leading = FlushComments();
  Position pos = current_.pos;
  const auto fail = [&](Position at, std::string msg) -> StatusOr<std::optional<EntryPtr>> {
    if (!tolerant_) return Err(at, std::move(msg));
    errs_.push_back(Error{at, std::move(msg)});
    comments_.insert(comments_.begin(), leading.begin(), leading.end());
    for (TokenKind k : {TokenKind::kAtType, TokenKind::kEquals, TokenKind::kString}) {
      if (current_.kind == k) Advance();
    }
    return std::optional<EntryPtr>{};
  };
  if (!first) return fail(pos, kTypeEntryMisplaced);
  Advance();  // consume @type
  if (current_.kind != TokenKind::kEquals) {
    return fail(current_.pos, "expected '=' after @type, got " + TokenErrMsg());
  }
  Advance();
  if (current_.kind != TokenKind::kString) {
    return fail(current_.pos, "expected a string type URL after @type =, got " + TokenErrMsg());
  }
  auto t = std::make_unique<TypeEntry>();
  t->pos = pos;
  t->url = std::string(current_.value);
  t->end = TokenEnd();
  t->leading_comments = std::move(leading);
  Advance();
  t->trailing_comment = TakeTrailingComment(t->end);
  return std::optional<EntryPtr>(EntryPtr(std::move(t)));
}

// PeekKind returns the kind of the next significant token without consuming
// it or disturbing the pending comments. What a tolerant lexer reports during
// the peek is dropped, since it reports it again.
TokenKind Parser::PeekKind() {
  const Lexer::Mark mark = lex_.GetMark();
  const Token saved_current = current_;
  const Position saved_prev_end = prev_end_;
  const size_t n_comments = comments_.size();
  const size_t n_errs = errs_.size();
  Advance();
  const TokenKind next = current_.kind;
  lex_.Rewind(mark);
  current_ = saved_current;
  prev_end_ = saved_prev_end;
  comments_.resize(n_comments);
  errs_.resize(n_errs);
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
    // Tolerant mode records it and parses on with the open grammar, so the
    // rest of the document still gets an AST.
    if (Status s = Soft(at_pos,
                        std::string("@") + name +
                            " is a spec-reserved directive name with no v1 semantics (draft "
                            "§3.4.6)");
        !s.ok()) {
      return s;
    }
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
    auto bv = ParseBlockVal(0);
    if (!bv.ok()) return bv.status();
    int close = FindMatchingBrace(lex_.Input(), open);
    if (close < 0) {
      // Reachable only in tolerant mode, where the block closed at EOF.
      return Err(d.pos, std::string("directive @") + d.name + ": unmatched '{'");
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
    return Err(current_.pos,
               std::string("expected '(' to start @dataset column list, got ") + DatasetTokenMsg());
  }
  Advance();  // consume (

  if (current_.kind != TokenKind::kIdent) {
    return Err(current_.pos,
               std::string("@dataset column list must contain at least one field name, got ") +
                   DatasetTokenMsg());
  }
  std::vector<Position> column_pos;
  for (;;) {
    if (current_.kind != TokenKind::kIdent) {
      return Err(current_.pos, std::string("expected column field name, got ") + DatasetTokenMsg());
    }
    tbl.columns.emplace_back(current_.value);
    column_pos.push_back(current_.pos);
    Advance();
    if (current_.kind == TokenKind::kComma) {
      Advance();
      continue;
    }
    if (current_.kind == TokenKind::kRParen) break;
    return Err(
        current_.pos,
        std::string("expected ',' or ')' in @dataset column list, got ") + DatasetTokenMsg());
  }
  // The rules that need no schema are checked here, at the offending
  // column; the ones that need the row type wait for a row to bind.
  size_t bad_column = 0;
  if (std::string msg = ColumnPathError(tbl.columns, &bad_column); !msg.empty()) {
    return Err(column_pos[bad_column], std::move(msg));
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
    return Err(current_.pos,
               std::string("expected ',' or ')' in @dataset row, got ") + DatasetTokenMsg());
  }
  int eo = current_.pos.offset + 1;
  Advance();  // consume )

  if (static_cast<int>(row.cells.size()) != expected) {
    return Err(pos,
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

  auto capture_brace_body = [this](const std::string& label, std::string* body, int* eo) -> Status {
    int open = current_.pos.offset;
    int close = FindMatchingBrace(lex_.Input(), open);
    if (close < 0) return Err(current_.pos, label + ": unmatched '{'");
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
        return Err(current_.pos,
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
      *end_offset = TokenEnd().offset;
      Advance();
      return pd;
    }
    case TokenKind::kBytes: {
      pd.shape = ProtoShape::kDescriptor;
      size_t offset = 0;
      auto decoded = detail::Base64DecodeProto(current_.value, &offset);
      if (!decoded.has_value()) {
        return Err(current_.pos,
                   "@proto descriptor body: invalid base64: illegal base64 data at input byte " +
                       std::to_string(offset));
      }
      pd.body = std::string(decoded->begin(), decoded->end());
      *end_offset = TokenEnd().offset;
      Advance();
      return pd;
    }
    default:
      return Err(current_.pos,
                 std::string("expected '{', dotted identifier, triple-quoted string, or b\"...\" "
                             "after @proto, got ") +
                     TokenKindName(current_.kind));
  }
}

// ParseRowCell consumes one cell of a @dataset row. Returns nullopt for
// an empty cell (no value between two commas, or at row start/end). A cell
// holds any value: `c = <value>` in a body means the same (draft §3.4.4).
StatusOr<std::optional<ValuePtr>> Parser::ParseRowCell() {
  if (current_.kind == TokenKind::kComma || current_.kind == TokenKind::kRParen) {
    return std::optional<ValuePtr>{std::nullopt};
  }
  // A block cell's entries take pending comments as their own. Comments
  // before the cell belong to no entry of it, so they are set aside.
  std::vector<Comment> pending;
  pending.swap(comments_);
  auto v = ParseValue(0);
  comments_.insert(comments_.begin(), pending.begin(), pending.end());
  if (!v.ok()) return v.status();
  return std::optional<ValuePtr>(std::move(v).consume());
}

int FindMatchingBrace(std::string_view input, int open_offset) {
  enum class Mode : uint8_t { kCode, kString, kTripleString, kBytes, kLineComment, kBlockComment };
  Mode mode = Mode::kCode;
  int depth = 1;
  bool esc = false;
  int run = 0;  // quotes just seen in a triple string, or 1 after a '*'
  const int n = static_cast<int>(input.size());
  for (int i = open_offset + 1; i < n; ++i) {
    const char ch = input[i];
    switch (mode) {
      case Mode::kString:
        if (esc) {
          esc = false;
        } else if (ch == '\\') {
          esc = true;
        } else if (ch == '"') {
          mode = Mode::kCode;
        } else if (ch == '\n') {
          return -1;
        }
        continue;
      case Mode::kTripleString:
        if (ch != '"') {
          run = 0;
        } else if (++run == 3) {
          mode = Mode::kCode;
          run = 0;
        }
        continue;
      case Mode::kBytes:
        if (ch == '"') {
          mode = Mode::kCode;
        } else if (ch == '\n') {
          return -1;
        }
        continue;
      case Mode::kLineComment:
        if (ch == '\n') mode = Mode::kCode;
        continue;
      case Mode::kBlockComment:
        if (ch == '/' && run == 1) {
          mode = Mode::kCode;
          run = 0;
        } else {
          run = ch == '*' ? 1 : 0;
        }
        continue;
      case Mode::kCode:
        break;
    }
    switch (ch) {
      case '{':
        ++depth;
        break;
      case '}':
        if (--depth == 0) return i;
        break;
      case '"':
        if (i + 2 >= n) return -1;
        if (input[i + 1] == '"' && input[i + 2] == '"') {
          mode = Mode::kTripleString;
          i += 2;
        } else {
          mode = Mode::kString;
        }
        break;
      case '#':
        mode = Mode::kLineComment;
        break;
      case 'b':
      case '/': {
        if (i + 1 >= n) return -1;
        const char next = input[i + 1];
        if (ch == 'b' && next == '"') {
          mode = Mode::kBytes;
          ++i;
        } else if (ch == '/' && next == '/') {
          mode = Mode::kLineComment;
          ++i;
        } else if (ch == '/' && next == '*') {
          mode = Mode::kBlockComment;
          ++i;
        }
        break;
      }
      default:
        break;
    }
  }
  return -1;
}

}  // namespace

StatusOr<DatasetDirective> ParseDatasetAt(Lexer* lex, Token* current, int max_depth) {
  Parser p(std::move(*lex), *current, max_depth);
  int end = 0;
  auto tbl = p.ParseTableDirective(&end);
  p.Release(lex, current);
  return tbl;
}

StatusOr<DatasetRow> ParseDatasetRow(std::string_view row,
                                     int expected,
                                     int max_depth,
                                     int max_bytes_literal) {
  Parser p(row, max_depth, max_bytes_literal);
  int end = 0;
  return p.ParseTableRow(expected, &end);
}

StatusOr<Document> Parse(std::string_view input, ParseOptions opts) {
  const int max_size = opts.max_message_size > 0 ? opts.max_message_size : kMaxMessageSize;
  if (input.size() > static_cast<size_t>(max_size)) {
    return Status::Error(1,
                         1,
                         "input of " + std::to_string(input.size()) +
                             " bytes exceeds MaxMessageSize=" + std::to_string(max_size));
  }
  Parser p(
      input,
      opts.max_nesting_depth > 0 ? opts.max_nesting_depth : kMaxNestingDepth,
      opts.max_bytes_literal_length > 0 ? opts.max_bytes_literal_length : kMaxBytesLiteralLength);
  return p.ParseDocument();
}

TolerantResult ParseTolerant(std::string_view input, ParseOptions opts) {
  TolerantResult out;
  const int max_size = opts.max_message_size > 0 ? opts.max_message_size : kMaxMessageSize;
  if (input.size() > static_cast<size_t>(max_size)) {
    out.errors.push_back(Error{Position{},
                               "input of " + std::to_string(input.size()) +
                                   " bytes exceeds MaxMessageSize=" + std::to_string(max_size)});
    return out;
  }
  Parser p(
      Parser::TolerantTag{},
      input,
      opts.max_nesting_depth > 0 ? opts.max_nesting_depth : kMaxNestingDepth,
      opts.max_bytes_literal_length > 0 ? opts.max_bytes_literal_length : kMaxBytesLiteralLength);
  auto doc = p.ParseDocument();
  if (doc.ok()) {
    out.document = std::move(doc).consume();
  } else {
    p.Record(doc.status());  // Not reachable; ParseDocument recovers from all.
  }
  // Recorded in parse order, which is source order except when a lexer error
  // surfaces during lookahead.
  out.errors = p.TakeErrors();
  return out;
}

}  // namespace protowire::pxf

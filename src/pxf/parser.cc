// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/parser.h"

#include <utility>

#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
#include "protowire/detail/rfc3339.h"
#include "protowire/pxf/lexer.h"

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

  Lexer lex_;
  Token current_;
  std::vector<Comment> comments_;
};

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

  if (current_.kind == TokenKind::kAtType) {
    Advance();
    if (current_.kind != TokenKind::kIdent) {
      return Status::Error(
          current_.pos.line, current_.pos.column, "expected type name after @type");
    }
    doc.type_url = std::string(current_.value);
    Advance();
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

}  // namespace

StatusOr<Document> Parse(std::string_view input) {
  return Parser(input).ParseDocument();
}

}  // namespace protowire::pxf

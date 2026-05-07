// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/lexer.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

using protowire::pxf::Lexer;
using protowire::pxf::Token;
using protowire::pxf::TokenKind;

TEST(Lexer, BasicAssignment) {
  std::string_view src = R"(name = "Alice"
count = 42
)";
  Lexer lex(src);
  Token t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kIdent);
  EXPECT_EQ(t.value, "name");

  EXPECT_EQ(lex.Next().kind, TokenKind::kEquals);
  t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kString);
  EXPECT_EQ(t.value, "Alice");
  EXPECT_EQ(lex.Next().kind, TokenKind::kNewline);
  EXPECT_EQ(lex.Next().kind, TokenKind::kIdent);
  EXPECT_EQ(lex.Next().kind, TokenKind::kEquals);
  t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kInt);
  EXPECT_EQ(t.value, "42");
}

TEST(Lexer, Punctuation) {
  Lexer lex("{ } [ ] : , =");
  for (auto k : {TokenKind::kLBrace,
                 TokenKind::kRBrace,
                 TokenKind::kLBracket,
                 TokenKind::kRBracket,
                 TokenKind::kColon,
                 TokenKind::kComma,
                 TokenKind::kEquals}) {
    EXPECT_EQ(lex.Next().kind, k);
  }
}

TEST(Lexer, Comments) {
  Lexer lex("# hash\n// slash\n/* block */");
  Token t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kComment);
  lex.Next();  // newline
  EXPECT_EQ(lex.Next().kind, TokenKind::kComment);
  lex.Next();  // newline
  EXPECT_EQ(lex.Next().kind, TokenKind::kComment);
}

TEST(Lexer, BoolNullEnumIdent) {
  Lexer lex("true false null STATUS_ACTIVE");
  EXPECT_EQ(lex.Next().kind, TokenKind::kBool);
  EXPECT_EQ(lex.Next().kind, TokenKind::kBool);
  EXPECT_EQ(lex.Next().kind, TokenKind::kNull);
  Token t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kIdent);
  EXPECT_EQ(t.value, "STATUS_ACTIVE");
}

TEST(Lexer, Numbers) {
  Lexer lex("123 -456 3.14 1.5e10");
  Token t1 = lex.Next();
  EXPECT_EQ(t1.kind, TokenKind::kInt);
  EXPECT_EQ(t1.value, "123");
  Token t2 = lex.Next();
  EXPECT_EQ(t2.kind, TokenKind::kInt);
  EXPECT_EQ(t2.value, "-456");
  Token t3 = lex.Next();
  EXPECT_EQ(t3.kind, TokenKind::kFloat);
  EXPECT_EQ(t3.value, "3.14");
  Token t4 = lex.Next();
  EXPECT_EQ(t4.kind, TokenKind::kFloat);
  EXPECT_EQ(t4.value, "1.5e10");
}

TEST(Lexer, Timestamp) {
  Lexer lex("2024-01-15T10:30:00Z");
  Token t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kTimestamp);
  EXPECT_EQ(t.value, "2024-01-15T10:30:00Z");
}

TEST(Lexer, Duration) {
  Lexer lex("30s 1h30m");
  Token t1 = lex.Next();
  EXPECT_EQ(t1.kind, TokenKind::kDuration);
  EXPECT_EQ(t1.value, "30s");
  Token t2 = lex.Next();
  EXPECT_EQ(t2.kind, TokenKind::kDuration);
  EXPECT_EQ(t2.value, "1h30m");
}

TEST(Lexer, BytesLiteral) {
  Lexer lex(R"(b"SGVsbG8=")");
  Token t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kBytes);
  EXPECT_EQ(t.value, "SGVsbG8=");
}

TEST(Lexer, AtType) {
  Lexer lex("@type pkg.Foo");
  Token t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kAtType);
}

TEST(Lexer, EscapesInString) {
  Lexer lex(R"("hello\nworld\t!")");
  Token t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kString);
  EXPECT_EQ(t.value, "hello\nworld\t!");
}

TEST(Lexer, TripleQuotedDedent) {
  std::string_view src = "\"\"\"\n  line1\n  line2\n  \"\"\"";
  Lexer lex(src);
  Token t = lex.Next();
  EXPECT_EQ(t.kind, TokenKind::kString);
  EXPECT_EQ(t.value, "line1\nline2");
}

}  // namespace

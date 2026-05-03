// Tests for the full Go-aligned PXF string-escape set:
//   \" \\ \' \?  \a \b \f \n \r \t \v
//   \xHH    (2 hex digits)
//   \nnn    (3 octal digits, leading 0-3)
//   \uHHHH  (4 hex digits, valid Unicode scalar)
//   \UHHHHHHHH (8 hex digits, valid Unicode scalar)
// Mirrors protowire-go/encoding/pxf/lexer_test.go.

#include "protowire/pxf/lexer.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using protowire::pxf::Lexer;
using protowire::pxf::Token;
using protowire::pxf::TokenKind;

// LexOne lexes a single STRING token from a `"..."` literal and returns its
// value. Returns std::nullopt if the lexer produces an ILLEGAL token.
std::optional<std::string> LexOne(std::string_view literal) {
  Lexer lex(literal);
  Token t = lex.Next();
  if (t.kind != TokenKind::kString) return std::nullopt;
  return std::string(t.value);
}

TEST(LexEscapes, BasicSet) {
  EXPECT_EQ(LexOne(R"("hello")"), "hello");
  EXPECT_EQ(LexOne(R"("a\"b")"), "a\"b");
  EXPECT_EQ(LexOne(R"("a\\b")"), "a\\b");
  EXPECT_EQ(LexOne(R"("a\nb")"), "a\nb");
  EXPECT_EQ(LexOne(R"("a\tb")"), "a\tb");
  EXPECT_EQ(LexOne(R"("a\rb")"), "a\rb");
}

TEST(LexEscapes, ExtendedSimpleEscapes) {
  EXPECT_EQ(LexOne(R"("\a")"), std::string("\x07"));
  EXPECT_EQ(LexOne(R"("\b")"), std::string("\x08"));
  EXPECT_EQ(LexOne(R"("\f")"), std::string("\x0C"));
  EXPECT_EQ(LexOne(R"("\v")"), std::string("\x0B"));
  EXPECT_EQ(LexOne(R"("\'")"), "'");
  EXPECT_EQ(LexOne(R"("\?")"), "?");
  EXPECT_EQ(LexOne(R"("\a\b\f\n\r\t\v")"),
            std::string("\x07\x08\x0C\n\r\t\x0B"));
}

TEST(LexEscapes, HexAndOctal) {
  EXPECT_EQ(LexOne(R"("\x41")"), "A");
  EXPECT_EQ(LexOne(R"("\xFF")"), std::string("\xFF"));
  EXPECT_EQ(LexOne(R"("\x00")"), std::string("\x00", 1));
  // Two adjacent hex escapes encode a 2-byte UTF-8 sequence.
  EXPECT_EQ(LexOne(R"("\xc3\xa9")"), "é");

  EXPECT_EQ(LexOne(R"("\101")"), "A");
  EXPECT_EQ(LexOne(R"("\377")"), std::string("\xFF"));
  EXPECT_EQ(LexOne(R"("\000")"), std::string("\x00", 1));
}

TEST(LexEscapes, UnicodeEscapes) {
  // BMP — 2-byte UTF-8.
  EXPECT_EQ(LexOne(R"("é")"), "é");
  // BMP — 3-byte UTF-8.
  EXPECT_EQ(LexOne(R"("中")"), "中");
  // Supplementary — 4-byte UTF-8.
  EXPECT_EQ(LexOne(R"("\U0001F600")"), "😀");
  // \U with BMP-range value.
  EXPECT_EQ(LexOne(R"("\U0000004A")"), "J");
  // Mixed.
  EXPECT_EQ(LexOne(R"("aéb")"), "aéb");
}

TEST(LexEscapes, LiteralUTF8) {
  // Literal multi-byte UTF-8 between quotes round-trips byte-for-byte.
  EXPECT_EQ(LexOne(R"("café")"), "café");
  EXPECT_EQ(LexOne(R"("日本語")"), "日本語");
  EXPECT_EQ(LexOne(R"("😀")"), "😀");
}

TEST(LexEscapes, RejectInvalid) {
  // Unknown escape — must error, not silently passthrough.
  EXPECT_FALSE(LexOne(R"("\z")").has_value());
  // Truncated \u.
  EXPECT_FALSE(LexOne(R"("\u12")").has_value());
  // Non-hex in \u.
  EXPECT_FALSE(LexOne(R"("\u12gh")").has_value());
  // High surrogate.
  EXPECT_FALSE(LexOne(R"("\uD800")").has_value());
  // Low surrogate.
  EXPECT_FALSE(LexOne(R"("\uDFFF")").has_value());
  // Out-of-range \U.
  EXPECT_FALSE(LexOne(R"("\U00110000")").has_value());
  // Truncated \U.
  EXPECT_FALSE(LexOne(R"("\U0001F60")").has_value());
  // Truncated \x.
  EXPECT_FALSE(LexOne(R"("\x")").has_value());
  EXPECT_FALSE(LexOne(R"("\x4")").has_value());
  // Non-hex \x.
  EXPECT_FALSE(LexOne(R"("\xZZ")").has_value());
  // Truncated octal.
  EXPECT_FALSE(LexOne(R"("\10")").has_value());
  // Non-octal in octal escape.
  EXPECT_FALSE(LexOne(R"("\18a")").has_value());
}

}  // namespace

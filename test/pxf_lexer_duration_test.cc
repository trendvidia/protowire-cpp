// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Tokenisation of duration literals against draft-01 §3.3:
//
//   duration-segment = 1*DIGIT [ "." 1*DIGIT ] time-unit
//   time-unit        = "ns" / "us" / micro-us / "ms" / "s" / "m" / "h"
//   micro-us         = %xC2.B5 %x73    ; UTF-8 of "µs"
//
// Before #20 the lexer decided FLOAT on seeing "." before it looked for a
// unit, so "1.5ms" came out as FLOAT "1.5" + IDENT "ms", and it never
// admitted the two-byte "µ", so "2µs" was INT "2" + ILLEGAL. Both are what
// Marshal writes for any Duration that is not a whole multiple of its
// largest unit, so this port could not read its own output. The table is
// the one protowire-go pins in lexer_duration_test.go (protowire-go#75).

#include "protowire/pxf/lexer.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using protowire::pxf::Lexer;
using protowire::pxf::Token;
using protowire::pxf::TokenKind;

struct Tok {
  TokenKind kind;
  std::string value;
};

std::vector<Tok> LexAll(std::string_view input) {
  Lexer lex(input);
  std::vector<Tok> out;
  for (int i = 0; i < 64; ++i) {
    Token t = lex.Next();
    if (t.kind == TokenKind::kEOF) return out;
    out.push_back(Tok{t.kind, std::string(t.value)});
  }
  ADD_FAILURE() << "lexer did not reach EOF within 64 tokens on " << input;
  return out;
}

TEST(LexerDuration, FractionalAndMicro) {
  const TokenKind D = TokenKind::kDuration;
  const TokenKind F = TokenKind::kFloat;
  const TokenKind I = TokenKind::kInt;
  const TokenKind ID = TokenKind::kIdent;
  const TokenKind X = TokenKind::kIllegal;
  struct Case {
    std::string input;
    std::vector<Tok> want;
  };
  const std::vector<Case> cases = {
      // §3.10 examples.
      {"30s", {{D, "30s"}}},
      {"1h30m", {{D, "1h30m"}}},
      {"500ms", {{D, "500ms"}}},
      {"1.5h", {{D, "1.5h"}}},
      {"2µs", {{D, "2µs"}}},
      {"2us", {{D, "2us"}}},

      // What FormatDuration emits for measured values.
      {"1.234567ms", {{D, "1.234567ms"}}},
      {"1.5ms", {{D, "1.5ms"}}},
      {"312.5µs", {{D, "312.5µs"}}},
      {"1.234µs", {{D, "1.234µs"}}},
      {"1h30m0.5s", {{D, "1h30m0.5s"}}},
      {"-1.5s", {{D, "-1.5s"}}},
      {"-312.5µs", {{D, "-312.5µs"}}},
      {"0s", {{D, "0s"}}},

      // Every unit, fractional.
      {"1.5ns", {{D, "1.5ns"}}},
      {"1.5us", {{D, "1.5us"}}},
      {"1.5µs", {{D, "1.5µs"}}},
      {"1.5s", {{D, "1.5s"}}},
      {"1.5m", {{D, "1.5m"}}},

      // A fraction in any segment, not only the first.
      {"1h30.5m", {{D, "1h30.5m"}}},
      {"1.5h30.5m1.5s", {{D, "1.5h30.5m1.5s"}}},

      // Unchanged forms.
      {"1h30m500ms", {{D, "1h30m500ms"}}},
      {"1ms234us567ns", {{D, "1ms234us567ns"}}},
      {"250ms", {{D, "250ms"}}},

      // Still a float when no unit follows...
      {"1.5", {{F, "1.5"}}},
      {"1.5e3", {{F, "1.5e3"}}},
      {"1.5E-3", {{F, "1.5E-3"}}},
      {"-1.5", {{F, "-1.5"}}},
      {"1.", {{F, "1."}}},
      {"1.e3", {{F, "1.e3"}}},
      // ...an exponent is not a unit...
      {"1.5e3ms", {{F, "1.5e3"}, {ID, "ms"}}},
      {"1e3s", {{F, "1e3"}, {ID, "s"}}},
      // ...a fraction with no digits after the "." is not a
      // duration-segment...
      {"1.ms", {{F, "1."}, {ID, "ms"}}},
      // ...and a non-unit letter is an identifier following the number.
      {"1.5x", {{F, "1.5"}, {ID, "x"}}},
      {"1.5 x", {{F, "1.5"}, {ID, "x"}}},
      {"5x", {{I, "5"}, {ID, "x"}}},

      // A unit letter that starts a longer word is consumed into the
      // duration attempt and rejected there.
      {"1.5min", {{X, "invalid duration: 1.5min"}}},
      {"5min", {{X, "invalid duration: 5min"}}},

      // Only U+00B5 MICRO SIGN (C2 B5) is micro-us. U+03BC GREEK SMALL
      // LETTER MU (CE BC) is not in the grammar even though ParseDuration
      // would accept it, and must not sneak in via the lexer. This port's
      // ILLEGAL path reports one byte at a time.
      {"2μs", {{I, "2"}, {X, "\xCE"}, {X, "\xBC"}, {ID, "s"}}},
      // A bare micro sign with no "s" is not a unit either.
      {"2µ", {{X, "invalid duration: 2µ"}}},
      {"2µm", {{X, "invalid duration: 2µm"}}},

      // The token ends where the value ends.
      {"1.5ms,", {{D, "1.5ms"}, {TokenKind::kComma, ","}}},
      {"1.5ms]", {{D, "1.5ms"}, {TokenKind::kRBracket, "]"}}},
      {"1.5ms}", {{D, "1.5ms"}, {TokenKind::kRBrace, "}"}}},
      {"1.5ms#c", {{D, "1.5ms"}, {TokenKind::kComment, "#c"}}},
      {"1.5ms\n", {{D, "1.5ms"}, {TokenKind::kNewline, ""}}},
      {"2µs 3", {{D, "2µs"}, {I, "3"}}},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.input);
    auto got = LexAll(c.input);
    ASSERT_EQ(got.size(), c.want.size());
    for (size_t i = 0; i < c.want.size(); ++i) {
      EXPECT_EQ(got[i].kind, c.want[i].kind) << "token " << i;
      EXPECT_EQ(got[i].value, c.want[i].value) << "token " << i;
    }
  }
}

}  // namespace

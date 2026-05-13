// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace protowire::pxf {

enum class TokenKind : uint8_t {
  kEOF = 0,
  kIllegal,
  kNewline,
  kComment,

  kIdent,
  kString,
  kInt,
  kFloat,
  kBool,
  kNull,
  kBytes,
  kTimestamp,
  kDuration,

  kLBrace,
  kRBrace,
  kLBracket,
  kRBracket,
  kLParen,  // ( — used by @dataset column list and row tuples
  kRParen,  // )
  kEquals,
  kColon,
  kComma,

  kAtType,
  kAtDirective,  // @<ident> for any non-reserved name; Token.value carries the bare name (no '@')
  kAtDataset,    // @dataset — row-oriented bulk-data directive (draft §3.4.4)
  kAtProto,      // @proto — embedded protobuf schema directive (draft §3.4.5)
};

const char* TokenKindName(TokenKind k);

struct Position {
  int line = 1;
  int column = 1;
  // Byte offset into the lexer's input. Used by directive Body
  // extraction to slice the raw bytes between '{' and '}'; line/column
  // remain the primary user-facing identifier. Zero is the start of
  // input.
  int offset = 0;
  std::string ToString() const { return std::to_string(line) + ":" + std::to_string(column); }
};

// A Token's `value` is owned by the lexer's input buffer for non-allocated
// tokens (numbers, idents, durations, timestamps) and points into it via
// string_view. STRING / BYTES tokens that required escape decoding own
// their string in the lexer's small storage list.
struct Token {
  TokenKind kind = TokenKind::kEOF;
  std::string_view value;
  Position pos;
};

}  // namespace protowire::pxf

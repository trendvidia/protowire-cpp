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
  kEquals,
  kColon,
  kComma,

  kAtType,
};

const char* TokenKindName(TokenKind k);

struct Position {
  int line = 1;
  int column = 1;
  std::string ToString() const {
    return std::to_string(line) + ":" + std::to_string(column);
  }
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

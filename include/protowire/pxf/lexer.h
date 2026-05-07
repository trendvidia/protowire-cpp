// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <deque>
#include <string>
#include <string_view>

#include "protowire/pxf/token.h"

namespace protowire::pxf {

class Lexer {
 public:
  // The input must outlive the lexer; tokens reference into it.
  explicit Lexer(std::string_view input) : input_(input) {}

  Token Next();

  Position CurrentPos() const { return Position{line_, column_}; }

 private:
  uint8_t Peek(size_t offset = 0) const {
    size_t i = pos_ + offset;
    if (i >= input_.size()) return 0;
    return static_cast<uint8_t>(input_[i]);
  }
  uint8_t Advance();
  void SkipSpaces();

  Token LexLineComment(Position pos);
  Token LexBlockComment(Position pos);
  Token LexString(Position pos);
  Token LexTripleString(Position pos);
  Token LexBytes(Position pos);
  Token LexDirective(Position pos);
  Token LexNumber(Position pos);
  Token LexFloat(Position pos, size_t start);
  Token LexTimestamp(Position pos, size_t start);
  Token LexDuration(Position pos, size_t start);
  Token LexIdent(Position pos);

  // Owned storage for tokens whose .value cannot view the input directly
  // (escape-decoded strings, dedented triple-quoted strings).
  std::string_view Store(std::string s);

  std::string_view input_;
  size_t pos_ = 0;
  int line_ = 1;
  int column_ = 1;
  std::deque<std::string> owned_;  // pointer-stable storage
};

}  // namespace protowire::pxf

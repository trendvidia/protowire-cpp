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

  Position CurrentPos() const { return Position{line_, column_, static_cast<int>(pos_)}; }

  // Raw input view — used by parseDirective to slice the body bytes
  // between '{' and '}' once the matching brace has been located.
  std::string_view Input() const { return input_; }

  // Reposition the lexer to byte offset `target`, recomputing line/col
  // by scanning forward from the current position. Used by parseProto-
  // Directive to skip past an @proto brace-body whose interior is
  // protobuf source (not PXF) without lexing through it.
  void RepositionTo(int target) {
    if (target < static_cast<int>(pos_)) {
      pos_ = 0;
      line_ = 1;
      column_ = 1;
    }
    while (static_cast<int>(pos_) < target && pos_ < input_.size()) {
      uint8_t ch = static_cast<uint8_t>(input_[pos_++]);
      if (ch == '\n') {
        ++line_;
        column_ = 1;
      } else {
        ++column_;
      }
    }
  }

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

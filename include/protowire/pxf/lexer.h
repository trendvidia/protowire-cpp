// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "protowire/limits.h"
#include "protowire/pxf/token.h"

namespace protowire::pxf {

class Lexer {
 public:
  // The input must outlive the lexer; tokens reference into it.
  explicit Lexer(std::string_view input) : input_(input) {}

  // A lexer over a piece of a larger document that starts at `start` in it,
  // as a dataset row does in its stream. Positions count from `start`: the
  // first byte is at start.line and start.column, and offsets add
  // start.offset. Input(), Mark and RepositionTo still index the piece
  // itself.
  Lexer(std::string_view input, Position start)
      : input_(input), line_(start.line), column_(start.column), start_(start) {}

  Token Next();

  // Bounds the decoded length of a b"…" literal (HARDENING.md
  // MaxBytesLiteralLength), judged from the literal's length while it is
  // scanned so it is neither scanned to its end nor decoded before it is
  // refused. The parser and decoder set it from their per-call limits.
  void SetMaxBytesLiteral(int n) { max_bytes_literal_ = n; }

  // Turns on the recovery ParseTolerant needs. An unterminated string ends at
  // the newline or EOF; an unterminated triple-quoted string, block comment
  // or bytes literal ends at EOF, a bytes literal at the newline; a bad escape
  // is kept literally; base64 is left to the parser. Each recovery appends to
  // *errors, which must outlive the lexer.
  void SetTolerant(std::vector<Error>* errors) { errors_ = errors; }

  // A place to come back to, for a parser's lookahead. Unlike a copy of the
  // lexer, going back leaves the strings of tokens already produced alone.
  struct Mark {
    size_t pos;
    int line;
    int column;
  };
  Mark GetMark() const { return Mark{pos_, line_, column_}; }
  void Rewind(const Mark& m) {
    pos_ = m.pos;
    line_ = m.line;
    column_ = m.column;
  }

  Position CurrentPos() const {
    return Position{line_, column_, start_.offset + static_cast<int>(pos_)};
  }

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
      line_ = start_.line;
      column_ = start_.column;
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
  bool Tolerant() const { return errors_ != nullptr; }
  void Report(Position pos, std::string msg) { errors_->push_back(Error{pos, std::move(msg)}); }
  bool ReadHexByte(uint8_t* out);
  bool ReadHexRune(int n, uint32_t* out);
  bool ReadOctRest(uint8_t first, uint8_t* out);

  Token LexLineComment(Position pos);
  Token LexBlockComment(Position pos);
  Token LexString(Position pos);
  Token LexTripleString(Position pos);
  Token LexBytes(Position pos);
  Token LexDirective(Position pos);
  Token LexNumber(Position pos);
  std::string_view LexPrecision();
  Token LexFloat(Position pos, size_t start);
  Token LexTimestamp(Position pos, size_t start);
  Token LexDuration(Position pos, size_t start);
  bool AtDurationUnit() const;
  Token LexIdent(Position pos);

  // Owned storage for tokens whose .value cannot view the input directly
  // (escape-decoded strings, dedented triple-quoted strings).
  std::string_view Store(std::string s);

  std::string_view input_;
  size_t pos_ = 0;
  int line_ = 1;
  int column_ = 1;
  Position start_{1, 1, 0};  // where input_[0] is in the document
  int max_bytes_literal_ = kMaxBytesLiteralLength;
  std::deque<std::string> owned_;  // pointer-stable storage
  std::vector<Error>* errors_ = nullptr;
};

}  // namespace protowire::pxf

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/lexer.h"

#include <cctype>
#include <string>
#include <vector>

#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
#include "protowire/detail/rfc3339.h"

namespace protowire::pxf {

namespace {

bool IsDigit(uint8_t c) {
  return c >= '0' && c <= '9';
}
bool IsIdentStart(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IsIdentPart(uint8_t c) {
  return IsIdentStart(c) || IsDigit(c) || c == '.';
}
bool IsDurationUnit(uint8_t c) {
  return c == 'h' || c == 'm' || c == 's' || c == 'n' || c == 'u';
}
bool IsLowerAlpha(uint8_t c) {
  return c >= 'a' && c <= 'z';
}

// HexVal decodes one hex digit; returns {value, ok}.
std::pair<int, bool> HexVal(uint8_t c) {
  if (c >= '0' && c <= '9') return {c - '0', true};
  if (c >= 'a' && c <= 'f') return {c - 'a' + 10, true};
  if (c >= 'A' && c <= 'F') return {c - 'A' + 10, true};
  return {0, false};
}

// OctVal decodes one octal digit; returns {value, ok}.
std::pair<int, bool> OctVal(uint8_t c) {
  if (c >= '0' && c <= '7') return {c - '0', true};
  return {0, false};
}

// EncodeRune writes the UTF-8 encoding of a Unicode scalar value into out.
// Caller must ensure the rune is valid (not a surrogate, <= 0x10FFFF).
void EncodeRune(uint32_t r, std::string& out) {
  if (r <= 0x7F) {
    out.push_back(static_cast<char>(r));
  } else if (r <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (r >> 6)));
    out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
  } else if (r <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (r >> 12)));
    out.push_back(static_cast<char>(0x80 | ((r >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (r >> 18)));
    out.push_back(static_cast<char>(0x80 | ((r >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((r >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
  }
}

// IsValidRune mirrors Go's utf8.ValidRune: rune must be in [0, 0x10FFFF]
// and not a UTF-16 surrogate half.
bool IsValidRune(uint32_t r) {
  return r <= 0x10FFFF && (r < 0xD800 || r > 0xDFFF);
}

// Strip leading newline and indent of closing triple-quote.
std::string Dedent(std::string_view raw) {
  if (!raw.empty() && raw[0] == '\n') raw.remove_prefix(1);
  if (raw.empty()) return std::string();
  // Split into lines.
  std::vector<std::string_view> lines;
  size_t start = 0;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\n') {
      lines.push_back(raw.substr(start, i - start));
      start = i + 1;
    }
  }
  lines.push_back(raw.substr(start));
  // If the last line is whitespace-only, use it as the indent and remove it.
  std::string_view indent;
  if (!lines.empty()) {
    std::string_view last = lines.back();
    bool ws_only = true;
    for (char c : last) {
      if (c != ' ' && c != '\t') {
        ws_only = false;
        break;
      }
    }
    if (ws_only) {
      indent = last;
      lines.pop_back();
    }
  }
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string_view line = lines[i];
    if (!indent.empty() && line.size() >= indent.size() &&
        line.substr(0, indent.size()) == indent) {
      line.remove_prefix(indent.size());
    }
    out.append(line);
    if (i + 1 < lines.size()) out.push_back('\n');
  }
  return out;
}

}  // namespace

const char* TokenKindName(TokenKind k) {
  switch (k) {
    case TokenKind::kEOF:
      return "EOF";
    case TokenKind::kIllegal:
      return "ILLEGAL";
    case TokenKind::kNewline:
      return "newline";
    case TokenKind::kComment:
      return "comment";
    case TokenKind::kIdent:
      return "identifier";
    case TokenKind::kString:
      return "string";
    case TokenKind::kInt:
      return "integer";
    case TokenKind::kFloat:
      return "float";
    case TokenKind::kBool:
      return "bool";
    case TokenKind::kNull:
      return "null";
    case TokenKind::kBytes:
      return "bytes";
    case TokenKind::kTimestamp:
      return "timestamp";
    case TokenKind::kDuration:
      return "duration";
    case TokenKind::kLBrace:
      return "{";
    case TokenKind::kRBrace:
      return "}";
    case TokenKind::kLBracket:
      return "[";
    case TokenKind::kRBracket:
      return "]";
    case TokenKind::kEquals:
      return "=";
    case TokenKind::kColon:
      return ":";
    case TokenKind::kComma:
      return ",";
    case TokenKind::kLParen:
      return "(";
    case TokenKind::kRParen:
      return ")";
    case TokenKind::kAtType:
      return "@type";
    case TokenKind::kAtDataset:
      return "@dataset";
    case TokenKind::kAtProto:
      return "@proto";
    case TokenKind::kAtDirective:
      return "@<directive>";
  }
  return "?";
}

uint8_t Lexer::Advance() {
  if (pos_ >= input_.size()) return 0;
  uint8_t c = static_cast<uint8_t>(input_[pos_++]);
  if (c == '\n') {
    ++line_;
    column_ = 1;
  } else {
    ++column_;
  }
  return c;
}

void Lexer::SkipSpaces() {
  while (pos_ < input_.size()) {
    uint8_t c = static_cast<uint8_t>(input_[pos_]);
    if (c == ' ' || c == '\t' || c == '\r') {
      Advance();
    } else {
      break;
    }
  }
}

std::string_view Lexer::Store(std::string s) {
  owned_.push_back(std::move(s));
  return owned_.back();
}

Token Lexer::Next() {
  SkipSpaces();
  if (pos_ >= input_.size()) {
    return Token{TokenKind::kEOF, {}, CurrentPos()};
  }
  Position pos = CurrentPos();
  uint8_t c = Peek();

  if (c == '\n') {
    Advance();
    return Token{TokenKind::kNewline, {}, pos};
  }
  if (c == '#') return LexLineComment(pos);
  if (c == '/' && Peek(1) == '/') return LexLineComment(pos);
  if (c == '/' && Peek(1) == '*') return LexBlockComment(pos);
  if (c == '"') {
    if (Peek(1) == '"' && Peek(2) == '"') return LexTripleString(pos);
    return LexString(pos);
  }
  if (c == 'b' && Peek(1) == '"') return LexBytes(pos);
  switch (c) {
    case '{':
      Advance();
      return Token{TokenKind::kLBrace, "{", pos};
    case '}':
      Advance();
      return Token{TokenKind::kRBrace, "}", pos};
    case '[':
      Advance();
      return Token{TokenKind::kLBracket, "[", pos};
    case ']':
      Advance();
      return Token{TokenKind::kRBracket, "]", pos};
    case '(':
      Advance();
      return Token{TokenKind::kLParen, "(", pos};
    case ')':
      Advance();
      return Token{TokenKind::kRParen, ")", pos};
    case '=':
      Advance();
      return Token{TokenKind::kEquals, "=", pos};
    case ':':
      Advance();
      return Token{TokenKind::kColon, ":", pos};
    case ',':
      Advance();
      return Token{TokenKind::kComma, ",", pos};
    case '@':
      return LexDirective(pos);
  }
  if (c == '-' || IsDigit(c)) return LexNumber(pos);
  if (IsIdentStart(c)) return LexIdent(pos);
  Advance();
  return Token{TokenKind::kIllegal, Store(std::string(1, static_cast<char>(c))), pos};
}

Token Lexer::LexLineComment(Position pos) {
  size_t start = pos_;
  while (pos_ < input_.size() && input_[pos_] != '\n') Advance();
  return Token{TokenKind::kComment, input_.substr(start, pos_ - start), pos};
}

Token Lexer::LexBlockComment(Position pos) {
  size_t start = pos_;
  Advance();  // /
  Advance();  // *
  while (pos_ + 1 < input_.size()) {
    if (input_[pos_] == '*' && input_[pos_ + 1] == '/') {
      Advance();
      Advance();
      return Token{TokenKind::kComment, input_.substr(start, pos_ - start), pos};
    }
    Advance();
  }
  return Token{TokenKind::kIllegal, "unterminated block comment", pos};
}

Token Lexer::LexString(Position pos) {
  Advance();  // opening "
  std::string sb;
  bool needs_storage = false;
  size_t verbatim_start = pos_;
  while (pos_ < input_.size()) {
    uint8_t c = static_cast<uint8_t>(input_[pos_]);
    if (c == '"') {
      if (!needs_storage) {
        std::string_view v = input_.substr(verbatim_start, pos_ - verbatim_start);
        Advance();
        return Token{TokenKind::kString, v, pos};
      }
      Advance();
      return Token{TokenKind::kString, Store(std::move(sb)), pos};
    }
    if (c == '\\') {
      if (!needs_storage) {
        sb.assign(input_.data() + verbatim_start, pos_ - verbatim_start);
        needs_storage = true;
      }
      Advance();
      if (pos_ >= input_.size()) {
        return Token{TokenKind::kIllegal, "unterminated escape sequence", pos};
      }
      uint8_t esc = Advance();
      switch (esc) {
        case '"':
          sb.push_back('"');
          break;
        case '\\':
          sb.push_back('\\');
          break;
        case '\'':
          sb.push_back('\'');
          break;
        case '?':
          sb.push_back('?');
          break;
        case 'a':
          sb.push_back('\x07');
          break;
        case 'b':
          sb.push_back('\x08');
          break;
        case 'f':
          sb.push_back('\x0C');
          break;
        case 'n':
          sb.push_back('\n');
          break;
        case 'r':
          sb.push_back('\r');
          break;
        case 't':
          sb.push_back('\t');
          break;
        case 'v':
          sb.push_back('\x0B');
          break;
        case 'x': {
          // Exactly 2 hex digits → 1 byte.
          if (pos_ + 1 >= input_.size()) {
            return Token{TokenKind::kIllegal, R"(invalid \x escape: expected 2 hex digits)", pos};
          }
          auto hi = HexVal(static_cast<uint8_t>(input_[pos_]));
          auto lo = HexVal(static_cast<uint8_t>(input_[pos_ + 1]));
          if (!hi.second || !lo.second) {
            return Token{TokenKind::kIllegal, R"(invalid \x escape: expected 2 hex digits)", pos};
          }
          Advance();
          Advance();
          sb.push_back(static_cast<char>((hi.first << 4) | lo.first));
          break;
        }
        case '0':
        case '1':
        case '2':
        case '3': {
          // \nnn — exactly 3 octal digits, leading 0-3 keeps result <= 0xFF.
          if (pos_ + 1 >= input_.size()) {
            return Token{
                TokenKind::kIllegal, R"(invalid octal escape: expected 3 octal digits)", pos};
          }
          auto d1 = OctVal(static_cast<uint8_t>(input_[pos_]));
          auto d2 = OctVal(static_cast<uint8_t>(input_[pos_ + 1]));
          if (!d1.second || !d2.second) {
            return Token{
                TokenKind::kIllegal, R"(invalid octal escape: expected 3 octal digits)", pos};
          }
          Advance();
          Advance();
          int v = ((esc - '0') << 6) | (d1.first << 3) | d2.first;
          sb.push_back(static_cast<char>(v));
          break;
        }
        case 'u': {
          // \uHHHH — exactly 4 hex digits → rune.
          if (pos_ + 4 > input_.size()) {
            return Token{TokenKind::kIllegal,
                         R"(invalid \u escape: expected 4 hex digits forming a valid codepoint)",
                         pos};
          }
          uint32_t r = 0;
          for (int i = 0; i < 4; ++i) {
            auto h = HexVal(static_cast<uint8_t>(input_[pos_]));
            if (!h.second) {
              return Token{TokenKind::kIllegal,
                           R"(invalid \u escape: expected 4 hex digits forming a valid codepoint)",
                           pos};
            }
            r = (r << 4) | static_cast<uint32_t>(h.first);
            Advance();
          }
          if (!IsValidRune(r)) {
            return Token{TokenKind::kIllegal,
                         R"(invalid \u escape: expected 4 hex digits forming a valid codepoint)",
                         pos};
          }
          EncodeRune(r, sb);
          break;
        }
        case 'U': {
          // \UHHHHHHHH — exactly 8 hex digits → rune.
          if (pos_ + 8 > input_.size()) {
            return Token{TokenKind::kIllegal,
                         R"(invalid \U escape: expected 8 hex digits forming a valid codepoint)",
                         pos};
          }
          uint32_t r = 0;
          for (int i = 0; i < 8; ++i) {
            auto h = HexVal(static_cast<uint8_t>(input_[pos_]));
            if (!h.second) {
              return Token{TokenKind::kIllegal,
                           R"(invalid \U escape: expected 8 hex digits forming a valid codepoint)",
                           pos};
            }
            r = (r << 4) | static_cast<uint32_t>(h.first);
            Advance();
          }
          if (!IsValidRune(r)) {
            return Token{TokenKind::kIllegal,
                         R"(invalid \U escape: expected 8 hex digits forming a valid codepoint)",
                         pos};
          }
          EncodeRune(r, sb);
          break;
        }
        default: {
          std::string msg = R"(unknown escape sequence \)";
          msg.push_back(static_cast<char>(esc));
          return Token{TokenKind::kIllegal, Store(std::move(msg)), pos};
        }
      }
      continue;
    }
    if (needs_storage) sb.push_back(static_cast<char>(c));
    Advance();
  }
  return Token{TokenKind::kIllegal, "unterminated string", pos};
}

Token Lexer::LexTripleString(Position pos) {
  Advance();
  Advance();
  Advance();
  size_t start = pos_;
  while (pos_ + 2 < input_.size()) {
    if (input_[pos_] == '"' && input_[pos_ + 1] == '"' && input_[pos_ + 2] == '"') {
      std::string raw(input_.substr(start, pos_ - start));
      Advance();
      Advance();
      Advance();
      return Token{TokenKind::kString, Store(Dedent(raw)), pos};
    }
    Advance();
  }
  return Token{TokenKind::kIllegal, "unterminated triple-quoted string", pos};
}

Token Lexer::LexBytes(Position pos) {
  Advance();  // 'b'
  if (pos_ >= input_.size() || input_[pos_] != '"') {
    return Token{TokenKind::kIllegal, R"(expected '"' after b)", pos};
  }
  Advance();  // opening "
  size_t start = pos_;
  while (pos_ < input_.size()) {
    char c = input_[pos_];
    if (c == '"') {
      std::string_view raw = input_.substr(start, pos_ - start);
      Advance();  // closing "
      if (!detail::Base64DecodeStd(raw).has_value()) {
        return Token{TokenKind::kIllegal, "invalid base64 in bytes literal", pos};
      }
      return Token{TokenKind::kBytes, raw, pos};
    }
    if (c == '\n') {
      return Token{TokenKind::kIllegal, "unterminated bytes literal", pos};
    }
    Advance();
  }
  return Token{TokenKind::kIllegal, "unterminated bytes literal", pos};
}

Token Lexer::LexDirective(Position pos) {
  Advance();  // @
  size_t start = pos_;
  while (pos_ < input_.size() && IsIdentPart(static_cast<uint8_t>(input_[pos_]))) {
    Advance();
  }
  std::string_view name = input_.substr(start, pos_ - start);
  if (name.empty()) return Token{TokenKind::kIllegal, "@", pos};
  if (name == "type") return Token{TokenKind::kAtType, "@type", pos};
  if (name == "dataset") return Token{TokenKind::kAtDataset, "@dataset", pos};
  if (name == "proto") return Token{TokenKind::kAtProto, "@proto", pos};
  // kAtDirective's Token.value carries the bare name (no `@`); the
  // parser uses this directly as Directive.name.
  return Token{TokenKind::kAtDirective, name, pos};
}

Token Lexer::LexNumber(Position pos) {
  size_t start = pos_;
  bool neg = false;
  if (Peek() == '-') {
    neg = true;
    Advance();
    if (pos_ >= input_.size() || !IsDigit(Peek())) {
      return Token{TokenKind::kIllegal, "-", pos};
    }
  }

  size_t digit_start = pos_;
  while (pos_ < input_.size() && IsDigit(Peek())) Advance();
  size_t digit_count = pos_ - digit_start;

  if (!neg && digit_count == 4 && pos_ < input_.size() && Peek() == '-') {
    return LexTimestamp(pos, start);
  }
  if (pos_ < input_.size() && (Peek() == '.' || Peek() == 'e' || Peek() == 'E')) {
    return LexFloat(pos, start);
  }
  if (pos_ < input_.size() && IsDurationUnit(Peek())) {
    return LexDuration(pos, start);
  }
  return Token{TokenKind::kInt, input_.substr(start, pos_ - start), pos};
}

Token Lexer::LexFloat(Position pos, size_t start) {
  if (Peek() == '.') {
    Advance();
    while (pos_ < input_.size() && IsDigit(Peek())) Advance();
  }
  if (pos_ < input_.size() && (Peek() == 'e' || Peek() == 'E')) {
    Advance();
    if (pos_ < input_.size() && (Peek() == '+' || Peek() == '-')) Advance();
    while (pos_ < input_.size() && IsDigit(Peek())) Advance();
  }
  return Token{TokenKind::kFloat, input_.substr(start, pos_ - start), pos};
}

Token Lexer::LexTimestamp(Position pos, size_t start) {
  while (pos_ < input_.size()) {
    uint8_t c = Peek();
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == ',' || c == ']' || c == '}' ||
        c == '#') {
      break;
    }
    if (c == '/' && (Peek(1) == '/' || Peek(1) == '*')) break;
    Advance();
  }
  std::string_view raw = input_.substr(start, pos_ - start);
  if (!detail::ParseRFC3339(raw).has_value()) {
    return Token{TokenKind::kIllegal, Store("invalid timestamp: " + std::string(raw)), pos};
  }
  return Token{TokenKind::kTimestamp, raw, pos};
}

Token Lexer::LexDuration(Position pos, size_t start) {
  while (pos_ < input_.size() && (IsDigit(Peek()) || IsLowerAlpha(Peek()))) {
    Advance();
  }
  std::string_view raw = input_.substr(start, pos_ - start);
  if (!detail::ParseDuration(raw).has_value()) {
    return Token{TokenKind::kIllegal, Store("invalid duration: " + std::string(raw)), pos};
  }
  return Token{TokenKind::kDuration, raw, pos};
}

Token Lexer::LexIdent(Position pos) {
  size_t start = pos_;
  while (pos_ < input_.size() && IsIdentPart(static_cast<uint8_t>(input_[pos_]))) {
    Advance();
  }
  std::string_view v = input_.substr(start, pos_ - start);
  if (v == "true" || v == "false") return Token{TokenKind::kBool, v, pos};
  if (v == "null") return Token{TokenKind::kNull, v, pos};
  return Token{TokenKind::kIdent, v, pos};
}

}  // namespace protowire::pxf

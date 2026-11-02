// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/lexer.h"

#include <cctype>
#include <string>
#include <vector>

#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
#include "protowire/detail/go_quote.h"
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

// IsBlank reports whether s is empty after Go's strings.TrimSpace: only
// runes with the Unicode White_Space property.
bool IsBlank(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    if (c < 0x80) {
      if (c != ' ' && (c < '\t' || c > '\r')) return false;
      ++i;
      continue;
    }
    const auto byte = [&](size_t k) -> uint32_t {
      return k < s.size() ? static_cast<uint8_t>(s[k]) : 0;
    };
    if (c == 0xC2 && (byte(i + 1) == 0x85 || byte(i + 1) == 0xA0)) {
      i += 2;  // U+0085, U+00A0
      continue;
    }
    if (i + 2 >= s.size()) return false;
    const uint32_t r = (uint32_t{c} << 16) | (byte(i + 1) << 8) | byte(i + 2);
    const bool space = r == 0xE19A80 ||                     // U+1680
                       (r >= 0xE28080 && r <= 0xE2808A) ||  // U+2000..U+200A
                       r == 0xE280A8 || r == 0xE280A9 ||    // U+2028, U+2029
                       r == 0xE280AF || r == 0xE2819F ||    // U+202F, U+205F
                       r == 0xE38080;                       // U+3000
    if (!space) return false;
    i += 3;
  }
  return true;
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
    bool ws_only = IsBlank(last);
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
      return "@directive";
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
  if (c == '-' || c == '+' || IsDigit(c)) return LexNumber(pos);
  if (IsIdentStart(c)) return LexIdent(pos);
  // A whole rune, so that a multi-byte character gives one diagnostic that
  // names what the author typed.
  size_t width = 1;
  std::string msg = detail::GoIllegalRune(input_.substr(pos_), &width);
  for (size_t i = 0; i < width; ++i) Advance();
  return Token{TokenKind::kIllegal, Store(std::move(msg)), pos};
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
  if (Tolerant()) {
    while (pos_ < input_.size()) Advance();
    Report(pos, "unterminated block comment");
    return Token{TokenKind::kComment, input_.substr(start, pos_ - start), pos};
  }
  return Token{TokenKind::kIllegal, "unterminated block comment", pos};
}

bool Lexer::ReadHexByte(uint8_t* out) {
  if (pos_ + 1 >= input_.size()) return false;
  auto hi = HexVal(static_cast<uint8_t>(input_[pos_]));
  auto lo = HexVal(static_cast<uint8_t>(input_[pos_ + 1]));
  if (!hi.second || !lo.second) return false;
  Advance();
  Advance();
  *out = static_cast<uint8_t>((hi.first << 4) | lo.first);
  return true;
}

// ReadHexRune reads exactly n hex digits. The digits before a bad one stay
// consumed, which is what tolerant recovery resumes after.
bool Lexer::ReadHexRune(int n, uint32_t* out) {
  if (pos_ + n > input_.size()) return false;
  uint32_t r = 0;
  for (int i = 0; i < n; ++i) {
    auto h = HexVal(static_cast<uint8_t>(input_[pos_]));
    if (!h.second) return false;
    r = (r << 4) | static_cast<uint32_t>(h.first);
    Advance();
  }
  *out = r;
  return true;
}

// ReadOctRest reads the two octal digits after the first. A leading 0-3
// keeps the value within a byte.
bool Lexer::ReadOctRest(uint8_t first, uint8_t* out) {
  if (pos_ + 1 >= input_.size()) return false;
  auto d1 = OctVal(static_cast<uint8_t>(input_[pos_]));
  auto d2 = OctVal(static_cast<uint8_t>(input_[pos_ + 1]));
  if (!d1.second || !d2.second) return false;
  Advance();
  Advance();
  *out = static_cast<uint8_t>(((first - '0') << 6) | (d1.first << 3) | d2.first);
  return true;
}

Token Lexer::LexString(Position pos) {
  Advance();  // opening "
  std::string sb;
  bool needs_storage = false;
  size_t verbatim_start = pos_;
  const auto finish = [&](size_t end) {
    if (!needs_storage) {
      return Token{TokenKind::kString, input_.substr(verbatim_start, end - verbatim_start), pos};
    }
    return Token{TokenKind::kString, Store(std::move(sb)), pos};
  };
  // A bad escape: tolerant mode reports it and keeps its text, kept, in the
  // string; otherwise it is the token.
  Position esc_pos;
  bool bad = false;
  Token illegal;
  const auto bad_escape = [&](std::string msg, std::string_view kept) {
    if (Tolerant()) {
      Report(esc_pos, std::move(msg));
      sb.append(kept);
    } else {
      bad = true;
      illegal = Token{TokenKind::kIllegal, Store(std::move(msg)), pos};
    }
  };
  while (pos_ < input_.size()) {
    uint8_t c = static_cast<uint8_t>(input_[pos_]);
    if (c == '"') {
      Token t = finish(pos_);
      Advance();
      return t;
    }
    if (c == '\\') {
      if (!needs_storage) {
        sb.assign(input_.data() + verbatim_start, pos_ - verbatim_start);
        needs_storage = true;
      }
      Advance();
      if (pos_ >= input_.size()) {
        if (Tolerant()) {
          Report(pos, "unterminated escape sequence");
          return finish(pos_);
        }
        return Token{TokenKind::kIllegal, "unterminated escape sequence", pos};
      }
      esc_pos = CurrentPos();
      uint8_t esc = Advance();
      switch (esc) {
        case '"':
        case '\\':
        case '\'':
        case '?':
          sb.push_back(static_cast<char>(esc));
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
          uint8_t b = 0;
          if (ReadHexByte(&b)) {
            sb.push_back(static_cast<char>(b));
          } else {
            bad_escape(R"(invalid \x escape: expected 2 hex digits)", R"(\x)");
          }
          break;
        }
        case '0':
        case '1':
        case '2':
        case '3': {
          uint8_t b = 0;
          if (ReadOctRest(esc, &b)) {
            sb.push_back(static_cast<char>(b));
          } else {
            const char kept[2] = {'\\', static_cast<char>(esc)};
            bad_escape(R"(invalid octal escape: expected 3 octal digits)",
                       std::string_view(kept, 2));
          }
          break;
        }
        case 'u': {
          uint32_t r = 0;
          if (ReadHexRune(4, &r) && IsValidRune(r)) {
            EncodeRune(r, sb);
          } else {
            bad_escape(R"(invalid \u escape: expected 4 hex digits forming a valid codepoint)",
                       R"(\u)");
          }
          break;
        }
        case 'U': {
          uint32_t r = 0;
          if (ReadHexRune(8, &r) && IsValidRune(r)) {
            EncodeRune(r, sb);
          } else {
            bad_escape(R"(invalid \U escape: expected 8 hex digits forming a valid codepoint)",
                       R"(\U)");
          }
          break;
        }
        default: {
          std::string msg = R"(unknown escape sequence \)";
          EncodeRune(esc, msg);  // Go's string(byte) reads the byte as a rune.
          const char kept[2] = {'\\', static_cast<char>(esc)};
          bad_escape(std::move(msg), std::string_view(kept, 2));
          break;
        }
      }
      if (bad) return illegal;
      continue;
    }
    if (c == '\n') {
      if (Tolerant()) {
        Report(pos, "unterminated string: ended at newline");
        return finish(pos_);
      }
      return Token{
          TokenKind::kIllegal,
          R"(newline in string literal: write it as \n, or use a """triple-quoted""" string)",
          pos};
    }
    if (needs_storage) sb.push_back(static_cast<char>(c));
    Advance();
  }
  if (Tolerant()) {
    Report(pos, "unterminated string");
    return finish(pos_);
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
  if (Tolerant()) {
    while (pos_ < input_.size()) Advance();
    Report(pos, "unterminated triple-quoted string");
    return Token{TokenKind::kString, Store(Dedent(input_.substr(start, pos_ - start))), pos};
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
    if (max_bytes_literal_ > 0 &&
        (pos_ - start) / 4 * 3 > static_cast<size_t>(max_bytes_literal_)) {
      // Decoded base64 is three bytes per four characters; judged from
      // the length so the literal is neither scanned to its end nor
      // decoded before it is refused.
      return Token{TokenKind::kIllegal,
                   Store("bytes literal decodes to more than MaxBytesLiteralLength=" +
                         std::to_string(max_bytes_literal_) + " bytes"),
                   pos};
    }
    if (c == '"') {
      std::string_view raw = input_.substr(start, pos_ - start);
      Advance();  // closing "
      // Tolerant mode leaves the check to the parser, which turns a failure
      // into a positioned error and a BadVal.
      size_t offset = 0;
      if (!Tolerant() && !detail::Base64DecodeLenient(raw, &offset).has_value()) {
        return Token{TokenKind::kIllegal, "invalid base64 in bytes literal", pos};
      }
      return Token{TokenKind::kBytes, raw, pos};
    }
    if (c == '\n') {
      if (Tolerant()) {
        Report(pos, "unterminated bytes literal: ended at newline");
        return Token{TokenKind::kBytes, input_.substr(start, pos_ - start), pos};
      }
      return Token{TokenKind::kIllegal, "unterminated bytes literal", pos};
    }
    Advance();
  }
  if (Tolerant()) {
    Report(pos, "unterminated bytes literal");
    return Token{TokenKind::kBytes, input_.substr(start, pos_ - start), pos};
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
  if (name.empty()) {
    return Token{TokenKind::kIllegal, R"("@" must be followed by a directive name)", pos};
  }
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
  if (const char sign = Peek(); sign == '-' || sign == '+') {
    Advance();
    // Spec §3.8: "+inf" and "-inf" are the only signed identifier literals.
    // A '+' sign is valid for nothing else.
    if (input_.substr(pos_, 3) == "inf" &&
        (pos_ + 3 >= input_.size() || !IsIdentPart(static_cast<uint8_t>(input_[pos_ + 3])))) {
      for (int i = 0; i < 3; ++i) Advance();
      return Token{TokenKind::kFloat, input_.substr(start, pos_ - start), pos};
    }
    if (sign == '+') {
      return Token{TokenKind::kIllegal, R"("+" is valid only in "+inf")", pos};
    }
    if (pos_ >= input_.size() || !IsDigit(Peek())) {
      return Token{TokenKind::kIllegal, R"("-" must be followed by a digit or "inf")", pos};
    }
    neg = true;
  }

  size_t digit_start = pos_;
  while (pos_ < input_.size() && IsDigit(Peek())) Advance();
  size_t digit_count = pos_ - digit_start;

  if (!neg && digit_count == 4 && pos_ < input_.size() && Peek() == '-') {
    return LexTimestamp(pos, start);
  }
  // Fraction: '.' followed by at least one digit. Floats and durations
  // both admit one (draft §3.3: duration-segment = 1*DIGIT [ "." 1*DIGIT ]
  // time-unit), so it is consumed here and the two are told apart by
  // what follows it. A '.' with no digit after it is not a
  // duration-segment; the float branch below keeps it as it always has.
  bool frac = false;
  if (Peek() == '.' && IsDigit(Peek(1))) {
    frac = true;
    Advance();  // .
    while (pos_ < input_.size() && IsDigit(Peek())) Advance();
  }
  // Duration: magnitude followed by a time unit (§3.10). Checked before
  // the float branch so "1.5ms" is one DURATION token rather than FLOAT
  // "1.5" followed by IDENT "ms" — which is what Marshal writes for any
  // Duration that is not a whole multiple of its largest unit (#20;
  // protowire-go#75).
  if (AtDurationUnit()) {
    return LexDuration(pos, start);
  }
  // Float: fraction, or 'e'/'E' exponent, or a bare trailing '.'.
  if (frac || (pos_ < input_.size() && (Peek() == '.' || Peek() == 'e' || Peek() == 'E'))) {
    return LexFloat(pos, start);
  }
  std::string_view value = input_.substr(start, pos_ - start);
  return Token{TokenKind::kInt, value, pos, LexPrecision()};
}

// LexPrecision consumes a number's precision suffix, 'p' and one or more
// digits, and returns the digits; a 'p' with no digit after it is not one.
std::string_view Lexer::LexPrecision() {
  if (Peek() != 'p' || !IsDigit(Peek(1))) return {};
  Advance();  // p
  size_t start = pos_;
  while (pos_ < input_.size() && IsDigit(Peek())) Advance();
  return input_.substr(start, pos_ - start);
}

// AtDurationUnit reports whether the bytes at pos_ start a time-unit of
// draft §3.3: one of the ASCII unit letters, or the two-byte UTF-8 micro
// sign U+00B5 (C2 B5) of "µs". U+03BC GREEK SMALL LETTER MU (CE BC) is not
// in the grammar and is deliberately not a unit here.
bool Lexer::AtDurationUnit() const {
  if (pos_ >= input_.size()) return false;
  uint8_t c = Peek();
  if (IsDurationUnit(c)) return true;
  return c == 0xC2 && Peek(1) == 0xB5;
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
  std::string_view value = input_.substr(start, pos_ - start);
  return Token{TokenKind::kFloat, value, pos, LexPrecision()};
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
  // A duration literal is digits, lower-case unit letters, a fraction
  // ('.' followed by a digit) inside any segment, and the two-byte micro
  // sign. The whole literal is then validated by ParseDuration, which
  // rejects unit misspellings ("5min") and stray micro signs ("2µ").
  for (;;) {
    if (pos_ >= input_.size()) break;
    uint8_t c = Peek();
    if (IsDigit(c) || IsLowerAlpha(c)) {
      Advance();
    } else if (c == '.' && IsDigit(Peek(1))) {
      Advance();
    } else if (c == 0xC2 && Peek(1) == 0xB5) {
      Advance();
      Advance();
    } else {
      break;
    }
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

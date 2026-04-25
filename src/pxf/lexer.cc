#include "protowire/pxf/lexer.h"

#include <cctype>
#include <string>
#include <vector>

#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
#include "protowire/detail/rfc3339.h"

namespace protowire::pxf {

namespace {

bool IsDigit(uint8_t c) { return c >= '0' && c <= '9'; }
bool IsIdentStart(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IsIdentPart(uint8_t c) {
  return IsIdentStart(c) || IsDigit(c) || c == '.';
}
bool IsDurationUnit(uint8_t c) {
  return c == 'h' || c == 'm' || c == 's' || c == 'n' || c == 'u';
}
bool IsLowerAlpha(uint8_t c) { return c >= 'a' && c <= 'z'; }

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
    case TokenKind::kEOF: return "EOF";
    case TokenKind::kIllegal: return "ILLEGAL";
    case TokenKind::kNewline: return "newline";
    case TokenKind::kComment: return "comment";
    case TokenKind::kIdent: return "identifier";
    case TokenKind::kString: return "string";
    case TokenKind::kInt: return "integer";
    case TokenKind::kFloat: return "float";
    case TokenKind::kBool: return "bool";
    case TokenKind::kNull: return "null";
    case TokenKind::kBytes: return "bytes";
    case TokenKind::kTimestamp: return "timestamp";
    case TokenKind::kDuration: return "duration";
    case TokenKind::kLBrace: return "{";
    case TokenKind::kRBrace: return "}";
    case TokenKind::kLBracket: return "[";
    case TokenKind::kRBracket: return "]";
    case TokenKind::kEquals: return "=";
    case TokenKind::kColon: return ":";
    case TokenKind::kComma: return ",";
    case TokenKind::kAtType: return "@type";
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
    case '{': Advance(); return Token{TokenKind::kLBrace, "{", pos};
    case '}': Advance(); return Token{TokenKind::kRBrace, "}", pos};
    case '[': Advance(); return Token{TokenKind::kLBracket, "[", pos};
    case ']': Advance(); return Token{TokenKind::kRBracket, "]", pos};
    case '=': Advance(); return Token{TokenKind::kEquals, "=", pos};
    case ':': Advance(); return Token{TokenKind::kColon, ":", pos};
    case ',': Advance(); return Token{TokenKind::kComma, ",", pos};
    case '@': return LexDirective(pos);
  }
  if (c == '-' || IsDigit(c)) return LexNumber(pos);
  if (IsIdentStart(c)) return LexIdent(pos);
  Advance();
  return Token{TokenKind::kIllegal, Store(std::string(1, static_cast<char>(c))),
               pos};
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
      return Token{TokenKind::kComment, input_.substr(start, pos_ - start),
                   pos};
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
        case '"': sb.push_back('"'); break;
        case '\\': sb.push_back('\\'); break;
        case 'n': sb.push_back('\n'); break;
        case 't': sb.push_back('\t'); break;
        case 'r': sb.push_back('\r'); break;
        default:
          sb.push_back('\\');
          sb.push_back(static_cast<char>(esc));
          break;
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
    if (input_[pos_] == '"' && input_[pos_ + 1] == '"' &&
        input_[pos_ + 2] == '"') {
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
  Token tok = LexString(pos);
  if (tok.kind != TokenKind::kString) return tok;
  // Validate base64.
  if (!detail::Base64DecodeStd(tok.value).has_value()) {
    return Token{TokenKind::kIllegal, "invalid base64 in bytes literal", pos};
  }
  tok.kind = TokenKind::kBytes;
  return tok;
}

Token Lexer::LexDirective(Position pos) {
  Advance();  // @
  size_t start = pos_;
  while (pos_ < input_.size() &&
         IsIdentPart(static_cast<uint8_t>(input_[pos_]))) {
    Advance();
  }
  std::string_view name = input_.substr(start, pos_ - start);
  if (name == "type") return Token{TokenKind::kAtType, "@type", pos};
  return Token{TokenKind::kIllegal, Store("@" + std::string(name)), pos};
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
  if (pos_ < input_.size() &&
      (Peek() == '.' || Peek() == 'e' || Peek() == 'E')) {
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
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == ',' ||
        c == ']' || c == '}' || c == '#') {
      break;
    }
    if (c == '/' && (Peek(1) == '/' || Peek(1) == '*')) break;
    Advance();
  }
  std::string_view raw = input_.substr(start, pos_ - start);
  if (!detail::ParseRFC3339(raw).has_value()) {
    return Token{TokenKind::kIllegal,
                 Store("invalid timestamp: " + std::string(raw)), pos};
  }
  return Token{TokenKind::kTimestamp, raw, pos};
}

Token Lexer::LexDuration(Position pos, size_t start) {
  while (pos_ < input_.size() &&
         (IsDigit(Peek()) || IsLowerAlpha(Peek()))) {
    Advance();
  }
  std::string_view raw = input_.substr(start, pos_ - start);
  if (!detail::ParseDuration(raw).has_value()) {
    return Token{TokenKind::kIllegal,
                 Store("invalid duration: " + std::string(raw)), pos};
  }
  return Token{TokenKind::kDuration, raw, pos};
}

Token Lexer::LexIdent(Position pos) {
  size_t start = pos_;
  while (pos_ < input_.size() &&
         IsIdentPart(static_cast<uint8_t>(input_[pos_]))) {
    Advance();
  }
  std::string_view v = input_.substr(start, pos_ - start);
  if (v == "true" || v == "false") return Token{TokenKind::kBool, v, pos};
  if (v == "null") return Token{TokenKind::kNull, v, pos};
  return Token{TokenKind::kIdent, v, pos};
}

}  // namespace protowire::pxf

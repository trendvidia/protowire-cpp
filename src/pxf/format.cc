// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// AST → PXF text formatter, comment-preserving. C++ port of
// protowire-go/encoding/pxf/format.go. Run as a sibling to Parse():
//
//   auto doc = pxf::Parse(input).value();
//   std::string text = pxf::FormatDocument(doc);
//
// String values are quoted using the same escape set the lexer accepts:
// `\"`, `\\`, `\n`, `\r`, `\t`, plus `\xHH` for control bytes < 0x20.
// Multi-byte UTF-8 above 0x20 is passed through literally.

#include "protowire/pxf/format.h"

#include <string>
#include <string_view>
#include <variant>

#include "protowire/detail/base64.h"
#include "protowire/pxf/lexer.h"

namespace protowire::pxf {

bool IsIdentifierSafe(std::string_view s) {
  if (s.empty() || s == "true" || s == "false" || s == "null") return false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (i != 0 && c >= '0' && c <= '9');
    if (!ok) return false;
  }
  return true;
}

namespace {

void WriteQuotedString(std::string_view s, std::string& out) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\x";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

// LexesAsBareMapKey reports whether s reads back as exactly one bare
// map-key token — an identifier, an integer or a bool (map-key =
// identifier / string / integer / bool) — spelled as s. null is not one.
bool LexesAsBareMapKey(std::string_view s) {
  Lexer lex(s);
  Token t = lex.Next();
  switch (t.kind) {
    case TokenKind::kIdent:
    case TokenKind::kInt:
    case TokenKind::kBool:
      return t.value == s && lex.Next().kind == TokenKind::kEOF;
    default:
      return false;
  }
}

// MapKeyQuoted decides the spelling of a map key on the way out. A key
// the document wrote quoted keeps its quotes unless it is identifier-safe;
// a key the document wrote bare stays bare, because the bare spellings
// true, 0 and 123 denote a bool or an integer key and quoting them would
// change what they denote (draft -01 § Entries and Keys, "Canonical
// spelling of map keys"; protowire#306). A MapEntry built in code carries
// no document spelling: it is written bare when its key lexes as one bare
// map-key token and quoted otherwise, so "" or "my key" never produce a
// document that does not parse.
bool MapKeyQuoted(const MapEntry& m) {
  if (m.key_quoted) return !IsIdentifierSafe(m.key);
  return !LexesAsBareMapKey(m.key);
}

class Formatter {
 public:
  explicit Formatter(std::string& out) : out_(out) {}

  void WriteIndent(int level) {
    for (int i = 0; i < level; ++i) out_ += "  ";
  }

  void WriteComments(const std::vector<Comment>& comments, int level) {
    for (const auto& c : comments) {
      WriteIndent(level);
      out_ += c.text;
      out_.push_back('\n');
    }
  }

  void FormatEntries(const std::vector<EntryPtr>& entries, int level) {
    for (const auto& entry : entries) {
      std::visit([&](const auto& ptr) { FormatEntry(*ptr, level); }, entry);
    }
  }

  void FormatEntry(const Assignment& a, int level) {
    WriteComments(a.leading_comments, level);
    WriteIndent(level);
    out_ += a.key;
    out_ += " = ";
    FormatValue(a.value, level);
    if (!a.trailing_comment.empty()) {
      out_.push_back(' ');
      out_ += a.trailing_comment;
    }
    out_.push_back('\n');
  }

  void FormatEntry(const MapEntry& m, int level) {
    WriteComments(m.leading_comments, level);
    WriteIndent(level);
    if (MapKeyQuoted(m)) {
      WriteQuotedString(m.key, out_);
    } else {
      out_ += m.key;
    }
    out_ += ": ";
    FormatValue(m.value, level);
    if (!m.trailing_comment.empty()) {
      out_.push_back(' ');
      out_ += m.trailing_comment;
    }
    out_.push_back('\n');
  }

  void FormatEntry(const Block& b, int level) {
    WriteComments(b.leading_comments, level);
    WriteIndent(level);
    out_ += b.name;
    out_ += " {\n";
    FormatEntries(b.entries, level + 1);
    WriteIndent(level);
    out_ += "}\n";
  }

  void FormatValue(const ValuePtr& val, int level) {
    std::visit([&](const auto& ptr) { FormatValueImpl(*ptr, level); }, val);
  }

 private:
  void FormatValueImpl(const StringVal& v, int /*level*/) { WriteQuotedString(v.value, out_); }
  void FormatValueImpl(const IntVal& v, int /*level*/) { out_ += v.raw; }
  void FormatValueImpl(const FloatVal& v, int /*level*/) { out_ += v.raw; }
  void FormatValueImpl(const BoolVal& v, int /*level*/) { out_ += v.value ? "true" : "false"; }
  void FormatValueImpl(const BytesVal& v, int /*level*/) {
    std::string_view raw(reinterpret_cast<const char*>(v.value.data()), v.value.size());
    out_ += "b\"";
    out_ += detail::Base64EncodeStd(raw);
    out_.push_back('"');
  }
  void FormatValueImpl(const NullVal& /*v*/, int /*level*/) { out_ += "null"; }
  void FormatValueImpl(const IdentVal& v, int /*level*/) { out_ += v.name; }
  void FormatValueImpl(const TimestampVal& v, int /*level*/) { out_ += v.raw; }
  void FormatValueImpl(const DurationVal& v, int /*level*/) { out_ += v.raw; }
  void FormatValueImpl(const ListVal& v, int level) {
    out_ += "[\n";
    for (size_t i = 0; i < v.elements.size(); ++i) {
      WriteIndent(level + 1);
      FormatValue(v.elements[i], level + 1);
      if (i + 1 < v.elements.size()) out_.push_back(',');
      out_.push_back('\n');
    }
    WriteIndent(level);
    out_.push_back(']');
  }
  void FormatValueImpl(const BlockVal& v, int level) {
    out_ += "{\n";
    FormatEntries(v.entries, level + 1);
    WriteIndent(level);
    out_.push_back('}');
  }

  std::string& out_;
};

}  // namespace

std::string FormatDocument(const Document& doc) {
  std::string out;
  if (!doc.type_url.empty()) {
    out += "@type ";
    out += doc.type_url;
    out += "\n\n";
  }
  Formatter f(out);
  f.WriteComments(doc.leading_comments, 0);
  f.FormatEntries(doc.entries, 0);
  return out;
}

}  // namespace protowire::pxf

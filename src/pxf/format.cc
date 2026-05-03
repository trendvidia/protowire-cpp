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

namespace protowire::pxf {

namespace {

void WriteQuotedString(std::string_view s, std::string& out) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
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

bool IsIdentChar(char c, bool first) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') return true;
  if (!first && c >= '0' && c <= '9') return true;
  return false;
}

// NeedsQuoting returns true for keys that aren't a plain identifier, matching
// Go's needsQuoting(). Empty string also requires quoting.
bool NeedsQuoting(std::string_view s) {
  if (s.empty()) return true;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!IsIdentChar(s[i], i == 0)) return true;
  }
  return false;
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
    if (NeedsQuoting(m.key)) {
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
  void FormatValueImpl(const StringVal& v, int /*level*/) {
    WriteQuotedString(v.value, out_);
  }
  void FormatValueImpl(const IntVal& v, int /*level*/) { out_ += v.raw; }
  void FormatValueImpl(const FloatVal& v, int /*level*/) { out_ += v.raw; }
  void FormatValueImpl(const BoolVal& v, int /*level*/) {
    out_ += v.value ? "true" : "false";
  }
  void FormatValueImpl(const BytesVal& v, int /*level*/) {
    std::string_view raw(reinterpret_cast<const char*>(v.value.data()),
                         v.value.size());
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

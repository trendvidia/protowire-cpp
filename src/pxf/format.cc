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
#include "protowire/detail/go_quote.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "format_internal.h"
#include "protowire/detail/base64.h"
#include "protowire/pxf/keyed.h"
#include "protowire/pxf/lexer.h"

namespace protowire::pxf {

bool IsIdentifierSafe(std::string_view s) {
  // One identifier-safe rule for the document: the test keyed entry
  // names use (protowire#313). The alphabet used to stop at
  // [A-Za-z0-9_], so "a.b" was written quoted where the identifier
  // production — ident-part admits '.' — says bare.
  return IsIdentifierSafeEntryName(s);
}

namespace {

void WriteQuotedString(std::string_view s, std::string& out) {
  out += detail::GoQuote(s);
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

class Formatter {
 public:
  explicit Formatter(std::string& out, std::string_view indent = "  ")
      : out_(out), indent_(indent) {}

  void WriteIndent(int level) {
    for (int i = 0; i < level; ++i) out_ += indent_;
  }

  void WriteComments(const std::vector<Comment>& comments, int level) {
    for (const auto& c : comments) {
      WriteIndent(level);
      out_ += c.text;
      out_.push_back('\n');
    }
  }

  void FormatEntries(const std::vector<EntryPtr>& entries, int level) {
    for (const auto& entry : entries) FormatOne(entry, level);
  }

  // A null entry or value is Go's nil interface: the type switch matches
  // nothing and writes nothing.
  void FormatOne(const EntryPtr& entry, int level) {
    std::visit(
        [&](const auto& ptr) {
          if (ptr) FormatEntry(*ptr, level);
        },
        entry);
  }

  void FormatEntry(const Assignment& a, int level) {
    WriteComments(a.leading_comments, level);
    WriteIndent(level);
    if (a.key_quoted) {
      WriteQuotedString(a.key, out_);
    } else {
      out_ += a.key;
    }
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

  void FormatEntry(const TypeEntry& e, int level) {
    WriteComments(e.leading_comments, level);
    WriteIndent(level);
    out_ += "@type = ";
    WriteQuotedString(e.url, out_);
    if (!e.trailing_comment.empty()) {
      out_.push_back(' ');
      out_ += e.trailing_comment;
    }
    out_.push_back('\n');
  }

  void FormatEntry(const Block& b, int level) {
    WriteComments(b.leading_comments, level);
    WriteIndent(level);
    if (b.name_quoted) {
      WriteQuotedString(b.name, out_);
    } else {
      out_ += b.name;
    }
    out_ += " {\n";
    FormatEntries(b.entries, level + 1);
    WriteIndent(level);
    out_ += "}\n";
  }

  void FormatValue(const ValuePtr& val, int level) {
    std::visit(
        [&](const auto& ptr) {
          if (ptr) FormatValueImpl(*ptr, level);
        },
        val);
  }

 private:
  void FormatNumber(const std::string& raw, const std::string& prec) {
    out_ += raw;
    if (!prec.empty()) {
      out_.push_back('p');
      out_ += prec;
    }
  }
  void FormatValueImpl(const StringVal& v, int /*level*/) { WriteQuotedString(v.value, out_); }
  void FormatValueImpl(const IntVal& v, int /*level*/) { FormatNumber(v.raw, v.prec); }
  void FormatValueImpl(const FloatVal& v, int /*level*/) { FormatNumber(v.raw, v.prec); }
  void FormatValueImpl(const BoolVal& v, int /*level*/) { out_ += v.value ? "true" : "false"; }
  void FormatValueImpl(const BytesVal& v, int /*level*/) {
    std::string_view raw(reinterpret_cast<const char*>(v.value.data()), v.value.size());
    out_ += "b\"";
    out_ += detail::Base64EncodeStd(raw);
    out_.push_back('"');
  }
  void FormatValueImpl(const NullVal& /*v*/, int /*level*/) { out_ += "null"; }
  // A placeholder has no source text. See BadVal.
  void FormatValueImpl(const BadVal& /*v*/, int /*level*/) {}
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
  std::string_view indent_;
};

}  // namespace

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

void AppendValueIndented(std::string* out, const ValuePtr& value, std::string_view indent) {
  Formatter f(*out, indent);
  f.FormatValue(value, 0);
}

void AppendEntryIndented(std::string* out, const EntryPtr& entry, std::string_view indent) {
  Formatter f(*out, indent);
  f.FormatOne(entry, 0);
}

void AppendValue(std::string* out, const ValuePtr& value) {
  Formatter f(*out);
  f.FormatValue(value, 0);
}

std::string FormatValue(const ValuePtr& value) {
  std::string out;
  AppendValue(&out, value);
  return out;
}

namespace {

// TopDirective is one of a Document's directives other than @type.
using TopDirective = std::variant<const Directive*, const DatasetDirective*, const ProtoDirective*>;

// TopDirectives returns doc's directives other than @type, which the
// Document keeps in three lists, merged back into source order: an
// anonymous @proto binds the typed directive after it, so the order is
// part of the meaning.
std::vector<TopDirective> TopDirectives(const Document& doc) {
  std::vector<std::pair<int, TopDirective>> all;
  all.reserve(doc.directives.size() + doc.datasets.size() + doc.protos.size());
  for (const auto& d : doc.directives) all.emplace_back(d.pos.offset, &d);
  for (const auto& d : doc.datasets) all.emplace_back(d.pos.offset, &d);
  for (const auto& d : doc.protos) all.emplace_back(d.pos.offset, &d);
  std::stable_sort(
      all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<TopDirective> out;
  out.reserve(all.size());
  for (const auto& a : all) out.push_back(a.second);
  return out;
}

// FormatNamedDirective writes `@<name> prefix* [{body}]`, the body as it
// was written: it is raw text, which the directive's consumer decodes.
void FormatNamedDirective(const Directive& d, std::string& out) {
  out.push_back('@');
  out += d.name;
  for (const auto& p : d.prefixes) {
    out.push_back(' ');
    out += p;
  }
  if (d.has_body) {
    out += " {";
    out += d.body;
    out.push_back('}');
  }
  out.push_back('\n');
}

// FormatDataset writes the header, then one row per line. An absent cell
// is left empty; null is written.
void FormatDataset(const DatasetDirective& d, Formatter& f, std::string& out) {
  out += "@dataset";
  if (!d.type.empty()) {
    out.push_back(' ');
    out += d.type;
  }
  out += " (";
  for (size_t i = 0; i < d.columns.size(); ++i) {
    if (i > 0) out += ", ";
    out += d.columns[i];
  }
  out += ")\n";
  for (const auto& row : d.rows) {
    out.push_back('(');
    for (size_t i = 0; i < row.cells.size(); ++i) {
      if (i > 0) out += ", ";
      const auto& cell = row.cells[i];
      if (cell.has_value()) f.FormatValue(*cell, 0);
    }
    out += ")\n";
  }
}

// FormatProto writes a @proto directive in the shape it was read in.
void FormatProto(const ProtoDirective& d, std::string& out) {
  out += "@proto";
  switch (d.shape) {
    case ProtoShape::kAnonymous:
      out += " {";
      out += d.body;
      out.push_back('}');
      break;
    case ProtoShape::kNamed:
      out.push_back(' ');
      out += d.type_name;
      out += " {";
      out += d.body;
      out.push_back('}');
      break;
    case ProtoShape::kSource:
      out += R"( """)";
      out += d.body;
      out += R"(""")";
      break;
    case ProtoShape::kDescriptor:
      out += " b\"";
      out += detail::Base64EncodeStd(d.body);
      out.push_back('"');
      break;
  }
  out.push_back('\n');
}

}  // namespace

// FormatDocument keeps every directive, with its dataset rows, in document
// order (trendvidia/protowire#558, #130; protowire-go#262). @type is written
// first: it binds no anonymous @proto, so moving it changes nothing. The
// other directives follow in source order, each after its comments: a named
// directive's body and a @proto body as written, and a dataset as its header
// and then one row per line, an absent cell left empty. A document whose only
// directive is @type formats as it always has.
std::string FormatDocument(const Document& doc) {
  std::string out;
  Formatter f(out);
  const std::vector<TopDirective> others = TopDirectives(doc);
  if (others.empty()) {
    if (!doc.type_url.empty()) {
      out += "@type ";
      out += doc.type_url;
      out += "\n\n";
    }
    f.WriteComments(doc.leading_comments, 0);
    f.FormatEntries(doc.entries, 0);
    return out;
  }

  if (!doc.type_url.empty()) {
    out += "@type ";
    out += doc.type_url;
    out.push_back('\n');
  }
  f.WriteComments(doc.leading_comments, 0);
  for (const auto& top : others) {
    std::visit(
        [&](const auto* d) {
          using T = std::decay_t<decltype(*d)>;
          f.WriteComments(d->leading_comments, 0);
          if constexpr (std::is_same_v<T, Directive>) {
            FormatNamedDirective(*d, out);
          } else if constexpr (std::is_same_v<T, DatasetDirective>) {
            FormatDataset(*d, f, out);
          } else {
            FormatProto(*d, out);
          }
        },
        top);
  }
  if (!doc.entries.empty()) {
    out.push_back('\n');
    f.FormatEntries(doc.entries, 0);
  }
  return out;
}

}  // namespace protowire::pxf

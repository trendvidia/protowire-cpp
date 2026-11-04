// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// SBE XML schema → .proto with SBE annotations. Line for line protowire-go's
// xmltoproto.go, whose output this matches byte for byte.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "protowire/detail/go_quote.h"
#include "protowire/sbe/xml.h"
#include "xml_names.h"

namespace protowire::sbe {

namespace {

using xml_names::CamelToScreamingSnake;
using xml_names::CamelToSnake;
using xml_names::SingularPascal;

struct Maps {
  std::map<std::string, XMLType> types;
  std::map<std::string, const XMLComposite*> composites;
  std::map<std::string, const XMLEnum*> enums;
};

// IsUTF8Compatible reports whether every string in a char array of the named
// encoding is valid UTF-8: none named (FIX SBE's default is US-ASCII), a
// US-ASCII name, or a UTF-8 name. Names are IANA charset names and their
// registered aliases, compared ignoring case and punctuation.
bool IsUTF8Compatible(std::string_view encoding) {
  constexpr std::string_view kSpace = " \t\n\v\f\r";
  const size_t b = encoding.find_first_not_of(kSpace);
  encoding = b == std::string_view::npos
                 ? std::string_view()
                 : encoding.substr(b, encoding.find_last_not_of(kSpace) - b + 1);
  std::string name;
  for (size_t i = 0; i < encoding.size(); ++i) {
    const char c = encoding[i];
    if (c == '-' || c == '_' || c == '.' || c == ':' || c == ' ') continue;
    // unicode.ToLower maps two non-ASCII letters into the names below: the
    // Kelvin sign (U+212A) to k and capital I with dot (U+0130) to i.
    if (encoding.substr(i, 3) == "\xE2\x84\xAA") {
      name += 'k';
      i += 2;
      continue;
    }
    if (encoding.substr(i, 2) == "\xC4\xB0") {
      name += 'i';
      i += 1;
      continue;
    }
    name += xml_names::ToLower(c);
  }
  static constexpr std::string_view kCompatible[] = {"",
                                                     // US-ASCII and its IANA aliases.
                                                     "usascii",
                                                     "ascii",
                                                     "ansix341968",
                                                     "ansix341986",
                                                     "iso646us",
                                                     "isoir6",
                                                     "iso646irv1991",
                                                     "us",
                                                     "ibm367",
                                                     "cp367",
                                                     "csascii",
                                                     // UTF-8.
                                                     "utf8",
                                                     "csutf8"};
  for (std::string_view c : kCompatible) {
    if (name == c) return true;
  }
  return false;
}

// ResolvePrimitive maps an SBE primitive type other than char to a proto type
// and its field options.
std::pair<std::string, std::string> ResolvePrimitive(const std::string& primitive) {
  if (primitive == "int8") return {"int32", "(sbe.encoding) = \"int8\""};
  if (primitive == "int16") return {"int32", "(sbe.encoding) = \"int16\""};
  if (primitive == "int32") return {"int32", ""};
  if (primitive == "int64") return {"int64", ""};
  if (primitive == "uint8") return {"uint32", "(sbe.encoding) = \"uint8\""};
  if (primitive == "uint16") return {"uint32", "(sbe.encoding) = \"uint16\""};
  if (primitive == "uint32") return {"uint32", ""};
  if (primitive == "uint64") return {"uint64", ""};
  if (primitive == "float") return {"float", ""};
  if (primitive == "double") return {"double", ""};
  return {primitive, ""};
}

struct Resolved {
  std::string proto_type;
  std::string opts;
  std::string note;  // a comment for the line above the field, when not empty
};

// ResolveType maps an SBE type to a proto type. It refuses an array that no
// proto field can hold, rather than importing it as one element and
// misplacing every later field in the block.
StatusOr<Resolved> ResolveType(const XMLType& t) {
  if (t.primitive_type == "char") {
    const uint32_t length = t.length == 0 ? 1 : t.length;
    const std::string opts = "(sbe.length) = " + std::to_string(length);
    if (!IsUTF8Compatible(t.character_encoding)) {
      // A proto string holds UTF-8 only, and a decoder refuses a string that
      // is not, so text in another encoding is carried as bytes rather than
      // failing at run time (protowire#494).
      return Resolved{"bytes",
                      opts,
                      "SBE characterEncoding " + detail::GoQuote(t.character_encoding) +
                          " is not UTF-8 compatible, so this field is bytes."};
    }
    return Resolved{"string", opts, ""};
  }
  if (t.length > 1) {
    if (t.primitive_type == "uint8" || t.primitive_type == "int8") {
      // A fixed-length array of octets is raw bytes, the same N bytes on the
      // wire as a bytes field with (sbe.length) = N.
      return Resolved{"bytes", "(sbe.length) = " + std::to_string(t.length), ""};
    }
    return Status::Error("type " + detail::GoQuote(t.name) + " is an array of " +
                         std::to_string(t.length) + " " + t.primitive_type +
                         ", which no proto field represents");
  }
  auto [type, opts] = ResolvePrimitive(t.primitive_type);
  return Resolved{type, opts, ""};
}

void WriteNote(std::string* b, const std::string& indent, const std::string& note) {
  if (!note.empty()) *b += indent + "// " + note + "\n";
}

void WriteLine(std::string* b,
               const std::string& indent,
               const std::string& type,
               const std::string& name,
               uint32_t number,
               const std::string& opts) {
  *b += indent + type + " " + name + " = " + std::to_string(number);
  if (!opts.empty()) *b += " [" + opts + "]";
  *b += ";\n";
}

void WriteEnum(std::string* b, const XMLEnum& e) {
  *b += "enum " + e.name + " {\n";
  const std::string prefix = CamelToScreamingSnake(e.name);
  for (const XMLValidValue& v : e.valid_values) {
    *b += "  " + prefix + "_" + CamelToScreamingSnake(v.name) + " = " + v.value + ";\n";
  }
  *b += "}\n\n";
}

Status WriteComposite(std::string* b, const XMLComposite& c) {
  *b += "message " + c.name + " {\n";
  uint32_t field_num = 1;
  for (const XMLType& t : c.types) {
    auto r = ResolveType(t);
    if (!r.ok()) {
      return Status::Error("sbe: composite " + detail::GoQuote(c.name) + ": " +
                           r.status().message());
    }
    WriteNote(b, "  ", r->note);
    WriteLine(b, "  ", r->proto_type, CamelToSnake(t.name), field_num, r->opts);
    ++field_num;
  }
  for (const XMLRef& ref : c.refs) {
    WriteLine(b, "  ", ref.type, CamelToSnake(ref.name), field_num, "");
    ++field_num;
  }
  *b += "}\n\n";
  return Status::OK();
}

Status WriteField(std::string* b, const XMLField& f, const Maps& m, const std::string& indent) {
  const std::string name = CamelToSnake(f.name);
  if (m.enums.count(f.type) > 0 || m.composites.count(f.type) > 0) {
    WriteLine(b, indent, f.type, name, f.id, "");
    return Status::OK();
  }
  if (auto it = m.types.find(f.type); it != m.types.end()) {
    auto r = ResolveType(it->second);
    if (!r.ok())
      return Status::Error("field " + detail::GoQuote(f.name) + ": " + r.status().message());
    WriteNote(b, indent, r->note);
    WriteLine(b, indent, r->proto_type, name, f.id, r->opts);
    return Status::OK();
  }
  // Unknown type: passed through as it is.
  WriteLine(b, indent, f.type, name, f.id, "");
  return Status::OK();
}

Status WriteGroup(std::string* b, const XMLGroup& g, const Maps& m, const std::string& indent) {
  const std::string msg_name = SingularPascal(g.name);
  *b += indent + "message " + msg_name + " {\n";
  for (const XMLField& f : g.fields) {
    if (Status st = WriteField(b, f, m, indent + "  "); !st.ok()) {
      return Status::Error("group " + detail::GoQuote(g.name) + ": " + st.message());
    }
  }
  *b += indent + "}\n";
  *b += indent + "repeated " + msg_name + " " + CamelToSnake(g.name) + " = " +
        std::to_string(g.id) + ";\n";
  return Status::OK();
}

Status WriteMessage(std::string* b, const XMLMessage& msg, const Maps& m) {
  *b += "message " + msg.name + " {\n";
  *b += "  option (sbe.template_id) = " + std::to_string(msg.id) + ";\n";
  for (const XMLField& f : msg.fields) {
    if (Status st = WriteField(b, f, m, "  "); !st.ok()) {
      return Status::Error("sbe: message " + detail::GoQuote(msg.name) + ": " + st.message());
    }
  }
  for (const XMLGroup& g : msg.groups) {
    if (Status st = WriteGroup(b, g, m, "  "); !st.ok()) {
      return Status::Error("sbe: message " + detail::GoQuote(msg.name) + ": " + st.message());
    }
  }
  *b += "}\n\n";
  return Status::OK();
}

}  // namespace

StatusOr<std::string> XMLToProto(std::string_view xml) {
  auto parsed = ParseXMLSchema(xml);
  if (!parsed.ok()) return parsed.status();
  const XMLSchema& schema = *parsed;

  Maps m;
  // The built-in SBE primitives, then the schema's own types, which may
  // replace one of them.
  for (const char* name : {"int8",
                           "int16",
                           "int32",
                           "int64",
                           "uint8",
                           "uint16",
                           "uint32",
                           "uint64",
                           "float",
                           "double",
                           "char"}) {
    XMLType t;
    t.name = name;
    t.primitive_type = name;
    m.types[name] = t;
  }
  for (const XMLType& t : schema.types.types) m.types[t.name] = t;
  for (const XMLComposite& c : schema.types.composites) m.composites[c.name] = &c;
  for (const XMLEnum& e : schema.types.enums) m.enums[e.name] = &e;

  std::string b = "syntax = \"proto3\";\n\n";
  if (!schema.package.empty()) b += "package " + schema.package + ";\n\n";
  b += "import \"sbe/annotations.proto\";\n\n";
  b += "option (sbe.schema_id) = " + std::to_string(schema.id) + ";\n";
  b += "option (sbe.version) = " + std::to_string(schema.version) + ";\n\n";

  for (const XMLEnum& e : schema.types.enums) WriteEnum(&b, e);
  // Composites become messages, except SBE's own framing types.
  for (const XMLComposite& c : schema.types.composites) {
    if (c.name == "messageHeader" || c.name == "groupSizeEncoding") continue;
    if (Status st = WriteComposite(&b, c); !st.ok()) return st;
  }
  for (const XMLMessage& msg : schema.messages) {
    if (Status st = WriteMessage(&b, msg, m); !st.ok()) return st;
  }
  return b;
}

}  // namespace protowire::sbe

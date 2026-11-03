// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// SBE XML schema parsing, as protowire-go's xmlschema.go does it with
// encoding/xml: the root must be messageSchema, elements and attributes match
// by local name, anything unknown is skipped, and a uint32 attribute is read
// by strconv.ParseUint after trimming space, empty meaning zero.

#include <cstdint>
#include <string>
#include <string_view>

#include <tinyxml2.h>

#include "protowire/detail/go_strconv.h"
#include "protowire/sbe/xml.h"

namespace protowire::sbe {

namespace {

namespace tx = tinyxml2;

// LocalName drops a namespace prefix, as encoding/xml matches by local name.
std::string_view LocalName(const char* name) {
  std::string_view s = name == nullptr ? "" : name;
  const size_t colon = s.find(':');
  return colon == std::string_view::npos ? s : s.substr(colon + 1);
}

// Attr is the value of the last attribute whose local name is name, or empty.
std::string Attr(const tx::XMLElement* e, std::string_view name) {
  std::string value;
  for (const tx::XMLAttribute* a = e->FirstAttribute(); a != nullptr; a = a->Next()) {
    if (LocalName(a->Name()) == name) value = a->Value();
  }
  return value;
}

std::string_view TrimSpace(std::string_view s) {
  constexpr std::string_view kSpace = " \t\n\v\f\r";
  const size_t b = s.find_first_not_of(kSpace);
  if (b == std::string_view::npos) return {};
  return s.substr(b, s.find_last_not_of(kSpace) - b + 1);
}

Status Uint32Attr(const tx::XMLElement* e, std::string_view name, uint32_t* out) {
  const std::string raw = Attr(e, name);
  if (raw.empty()) {
    *out = 0;
    return Status::OK();
  }
  uint64_t v = 0;
  std::string why;
  if (!detail::GoParseUint(TrimSpace(raw), 32, &v, &why)) return Status::Error(why);
  *out = static_cast<uint32_t>(v);
  return Status::OK();
}

// CharData is the element's own text, CDATA included, as `xml:",chardata"`
// collects it.
std::string CharData(const tx::XMLElement* e) {
  std::string out;
  for (const tx::XMLNode* n = e->FirstChild(); n != nullptr; n = n->NextSibling()) {
    if (const tx::XMLText* t = n->ToText(); t != nullptr) out += t->Value();
  }
  return out;
}

template <class F>
Status EachChild(const tx::XMLElement* e, std::string_view name, F&& f) {
  for (const tx::XMLElement* c = e->FirstChildElement(); c != nullptr;
       c = c->NextSiblingElement()) {
    if (LocalName(c->Name()) != name) continue;
    if (Status st = f(c); !st.ok()) return st;
  }
  return Status::OK();
}

Status ParseType(const tx::XMLElement* e, XMLType* t) {
  t->name = Attr(e, "name");
  t->primitive_type = Attr(e, "primitiveType");
  if (Status st = Uint32Attr(e, "length", &t->length); !st.ok()) return st;
  t->description = Attr(e, "description");
  t->character_encoding = Attr(e, "characterEncoding");
  return Status::OK();
}

Status ParseField(const tx::XMLElement* e, XMLField* f) {
  f->name = Attr(e, "name");
  if (Status st = Uint32Attr(e, "id", &f->id); !st.ok()) return st;
  f->type = Attr(e, "type");
  return Status::OK();
}

Status ParseTypes(const tx::XMLElement* e, XMLTypes* types) {
  if (Status st = EachChild(
          e,
          "type",
          [&](const tx::XMLElement* c) { return ParseType(c, &types->types.emplace_back()); });
      !st.ok()) {
    return st;
  }
  if (Status st = EachChild(e,
                            "composite",
                            [&](const tx::XMLElement* c) {
                              XMLComposite& comp = types->composites.emplace_back();
                              comp.name = Attr(c, "name");
                              comp.description = Attr(c, "description");
                              if (Status s = EachChild(c,
                                                       "type",
                                                       [&](const tx::XMLElement* t) {
                                                         return ParseType(
                                                             t, &comp.types.emplace_back());
                                                       });
                                  !s.ok()) {
                                return s;
                              }
                              return EachChild(c, "ref", [&](const tx::XMLElement* r) {
                                comp.refs.push_back({Attr(r, "name"), Attr(r, "type")});
                                return Status::OK();
                              });
                            });
      !st.ok()) {
    return st;
  }
  return EachChild(e, "enum", [&](const tx::XMLElement* c) {
    XMLEnum& en = types->enums.emplace_back();
    en.name = Attr(c, "name");
    en.encoding_type = Attr(c, "encodingType");
    en.description = Attr(c, "description");
    return EachChild(c, "validValue", [&](const tx::XMLElement* v) {
      en.valid_values.push_back({Attr(v, "name"), CharData(v)});
      return Status::OK();
    });
  });
}

Status ParseMessage(const tx::XMLElement* e, XMLMessage* m) {
  m->name = Attr(e, "name");
  if (Status st = Uint32Attr(e, "id", &m->id); !st.ok()) return st;
  m->description = Attr(e, "description");
  if (Status st = EachChild(
          e,
          "field",
          [&](const tx::XMLElement* c) { return ParseField(c, &m->fields.emplace_back()); });
      !st.ok()) {
    return st;
  }
  return EachChild(e, "group", [&](const tx::XMLElement* c) {
    XMLGroup& g = m->groups.emplace_back();
    g.name = Attr(c, "name");
    if (Status st = Uint32Attr(c, "id", &g.id); !st.ok()) return st;
    return EachChild(c, "field", [&](const tx::XMLElement* f) {
      return ParseField(f, &g.fields.emplace_back());
    });
  });
}

// StripSBENamespace is Go's textual replacement, applied to the whole input:
// sbe:message and sbe:messageSchema lose their prefix.
std::string StripSBENamespace(std::string_view data) {
  constexpr std::string_view kFrom = "sbe:message";
  std::string out;
  out.reserve(data.size());
  size_t i = 0;
  while (true) {
    const size_t at = data.find(kFrom, i);
    if (at == std::string_view::npos) break;
    out.append(data.substr(i, at - i));
    out += "message";
    i = at + kFrom.size();
  }
  out.append(data.substr(i));
  return out;
}

Status Wrap(const Status& st) {
  return Status::Error("sbe: parse XML schema: " + st.message());
}

}  // namespace

StatusOr<XMLSchema> ParseXMLSchema(std::string_view data) {
  const std::string input = StripSBENamespace(data);
  tx::XMLDocument doc(/*processEntities=*/true, tx::PRESERVE_WHITESPACE);
  if (doc.Parse(input.data(), input.size()) != tx::XML_SUCCESS) {
    // encoding/xml reports input with no element as EOF.
    if (doc.ErrorID() == tx::XML_ERROR_EMPTY_DOCUMENT) return Wrap(Status::Error("EOF"));
    return Wrap(Status::Error(doc.ErrorStr()));
  }
  const tx::XMLElement* root = doc.RootElement();
  if (root == nullptr) return Wrap(Status::Error("EOF"));
  if (LocalName(root->Name()) != "messageSchema") {
    return Wrap(Status::Error("expected element type <messageSchema> but have <" +
                              std::string(LocalName(root->Name())) + ">"));
  }
  XMLSchema schema;
  schema.package = Attr(root, "package");
  if (Status st = Uint32Attr(root, "id", &schema.id); !st.ok()) return Wrap(st);
  if (Status st = Uint32Attr(root, "version", &schema.version); !st.ok()) return Wrap(st);
  schema.byte_order = Attr(root, "byteOrder");
  schema.description = Attr(root, "description");
  // Every <types> adds to the one XMLTypes, as encoding/xml fills a struct
  // field once per matching element.
  if (Status st = EachChild(
          root, "types", [&](const tx::XMLElement* c) { return ParseTypes(c, &schema.types); });
      !st.ok()) {
    return Wrap(st);
  }
  if (Status st = EachChild(root,
                            "message",
                            [&](const tx::XMLElement* c) {
                              return ParseMessage(c, &schema.messages.emplace_back());
                            });
      !st.ok()) {
    return Wrap(st);
  }
  return schema;
}

}  // namespace protowire::sbe

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// .proto with SBE annotations → SBE XML schema. Line for line protowire-go's
// prototoxml.go, whose output this matches byte for byte.

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/unknown_field_set.h>

#include "protowire/sbe/annotations.h"
#include "protowire/sbe/xml.h"
#include "xml_names.h"

namespace protowire::sbe {

namespace {

namespace gpb = google::protobuf;
using xml_names::SnakeToCamel;
using xml_names::StripEnumPrefix;

// (sbe.schema_id)'s number before protowire v1.12.0. Nothing declares it any
// more, so it survives only in an options message's unknown fields.
constexpr int kRetiredSchemaID = 50100;

// StaleFileError is the diagnosis for a file whose (sbe.schema_id) sits at the
// retired number, as protowire-go's staleFileError; empty when it does not.
std::string StaleFileError(const gpb::FileDescriptor* fd) {
  // An extension the pool declares at that number is a known option, as Go's
  // compiler stores it, even where libprotobuf keeps it as unknown bytes.
  // The pool's own FileOptions, which is not the generated one when the file
  // was compiled with descriptor.proto in its pool.
  const gpb::Descriptor* file_options =
      fd->pool()->FindMessageTypeByName("google.protobuf.FileOptions");
  if (file_options == nullptr) file_options = gpb::FileOptions::descriptor();
  if (fd->pool()->FindExtensionByNumber(file_options, kRetiredSchemaID) != nullptr) return "";
  const gpb::Message& opts = fd->options();
  const gpb::UnknownFieldSet& unknown = opts.GetReflection()->GetUnknownFields(opts);
  for (int i = 0; i < unknown.field_count(); ++i) {
    if (unknown.field(i).number() == kRetiredSchemaID) {
      return "sbe: file " + std::string(fd->name()) +
             " carries (sbe.schema_id) at retired option number " +
             std::to_string(kRetiredSchemaID) + " (registered as " + std::to_string(kExtSchemaID) +
             " since protowire v1.12.0): the descriptor predates the registered extension block "
             "and "
             "must be recompiled against the current sbe/annotations.proto";
    }
  }
  return "";
}

// IsList is protoreflect's IsList: repeated and not a map.
bool IsList(const gpb::FieldDescriptor* f) {
  return f->is_repeated() && !f->is_map();
}

bool IsMessage(const gpb::FieldDescriptor* f) {
  return f->type() == gpb::FieldDescriptor::TYPE_MESSAGE;
}

std::vector<const gpb::FieldDescriptor*> SortedFields(const gpb::Descriptor* md) {
  // By field number; numbers are unique within a message.
  std::vector<std::pair<int, int>> order;
  order.reserve(static_cast<size_t>(md->field_count()));
  for (int i = 0; i < md->field_count(); ++i) order.emplace_back(md->field(i)->number(), i);
  std::sort(order.begin(), order.end());
  std::vector<const gpb::FieldDescriptor*> out;
  out.reserve(order.size());
  for (const auto& [number, i] : order) out.push_back(md->field(i));
  return out;
}

std::string StrTypeName(uint32_t length) {
  return "str" + std::to_string(length);
}
std::string BytesTypeName(uint32_t length) {
  return "bytes" + std::to_string(length);
}

struct Collected {
  std::set<uint32_t> str_lengths;
  std::set<uint32_t> bytes_lengths;
  std::vector<const gpb::Descriptor*> composites;
  std::set<std::string> comp_seen;
  std::vector<const gpb::EnumDescriptor*> enums;
  std::set<std::string> enum_seen;
};

void AddEnum(Collected* c, const gpb::EnumDescriptor* ed) {
  if (c->enum_seen.insert(std::string(ed->full_name())).second) c->enums.push_back(ed);
}

void CollectTypes(const gpb::Descriptor* md, Collected* c) {
  for (int i = 0; i < md->enum_type_count(); ++i) AddEnum(c, md->enum_type(i));
  for (int i = 0; i < md->field_count(); ++i) {
    const gpb::FieldDescriptor* f = md->field(i);
    if (auto length = GetFieldUint32Option(f, kExtLength)) {
      if (f->type() == gpb::FieldDescriptor::TYPE_STRING) c->str_lengths.insert(*length);
      if (f->type() == gpb::FieldDescriptor::TYPE_BYTES) c->bytes_lengths.insert(*length);
    }
    if (f->type() == gpb::FieldDescriptor::TYPE_ENUM) AddEnum(c, f->enum_type());
    if (IsMessage(f)) {
      if (IsList(f)) {
        CollectTypes(f->message_type(), c);
      } else if (c->comp_seen.insert(std::string(f->message_type()->full_name())).second) {
        c->composites.push_back(f->message_type());
        CollectTypes(f->message_type(), c);
      }
    }
  }
}

struct TypeInfo {
  std::string primitive_type;
  // What a <field> names: the primitive, or for a fixed-length array the
  // <types> entry declared for its length.
  std::string xml_type;
  uint32_t length = 0;
};

TypeInfo FieldToSBEType(const gpb::FieldDescriptor* fd) {
  if (auto enc = GetFieldStringOption(fd, kExtEncoding)) return {*enc, *enc, 0};
  // A bytes field is a uint8 array, not a char array: a char array imports
  // back as a string, and a char in a non-UTF-8 encoding as bytes only because
  // nothing carries the encoding. Both are the same N bytes on the wire.
  const auto type = fd->type();
  if (type == gpb::FieldDescriptor::TYPE_STRING || type == gpb::FieldDescriptor::TYPE_BYTES) {
    const auto length = GetFieldUint32Option(fd, kExtLength);
    if (!length || *length == 0) return {"char", "char", 0};
    if (type == gpb::FieldDescriptor::TYPE_BYTES) return {"uint8", BytesTypeName(*length), *length};
    return {"char", StrTypeName(*length), *length};
  }
  switch (type) {
    case gpb::FieldDescriptor::TYPE_BOOL:
      return {"uint8", "uint8", 0};
    case gpb::FieldDescriptor::TYPE_INT32:
    case gpb::FieldDescriptor::TYPE_SINT32:
    case gpb::FieldDescriptor::TYPE_SFIXED32:
      return {"int32", "int32", 0};
    case gpb::FieldDescriptor::TYPE_INT64:
    case gpb::FieldDescriptor::TYPE_SINT64:
    case gpb::FieldDescriptor::TYPE_SFIXED64:
      return {"int64", "int64", 0};
    case gpb::FieldDescriptor::TYPE_UINT32:
    case gpb::FieldDescriptor::TYPE_FIXED32:
      return {"uint32", "uint32", 0};
    case gpb::FieldDescriptor::TYPE_UINT64:
    case gpb::FieldDescriptor::TYPE_FIXED64:
      return {"uint64", "uint64", 0};
    case gpb::FieldDescriptor::TYPE_FLOAT:
      return {"float", "float", 0};
    case gpb::FieldDescriptor::TYPE_DOUBLE:
      return {"double", "double", 0};
    default:
      return {"uint8", "uint8", 0};
  }
}

void WriteEnum(std::string* b, const gpb::EnumDescriptor* ed) {
  const std::string name = std::string(ed->name());
  *b += "        <enum name=\"" + name + "\" encodingType=\"uint8\">\n";
  for (int i = 0; i < ed->value_count(); ++i) {
    const gpb::EnumValueDescriptor* v = ed->value(i);
    *b += "            <validValue name=\"" + StripEnumPrefix(v->name(), name) + "\">" +
          std::to_string(v->number()) + "</validValue>\n";
  }
  *b += "        </enum>\n";
}

void WriteComposite(std::string* b, const gpb::Descriptor* md) {
  *b += "        <composite name=\"" + std::string(md->name()) + "\">\n";
  for (const gpb::FieldDescriptor* f : SortedFields(md)) {
    const TypeInfo info = FieldToSBEType(f);
    *b += "            <type name=\"" + SnakeToCamel(f->name()) + "\" primitiveType=\"" +
          info.primitive_type + "\"";
    if (info.length > 0) *b += " length=\"" + std::to_string(info.length) + "\"";
    *b += "/>\n";
  }
  *b += "        </composite>\n";
}

void WriteField(std::string* b, const gpb::FieldDescriptor* fd, const std::string& indent) {
  std::string type;
  if (fd->type() == gpb::FieldDescriptor::TYPE_ENUM) {
    type = fd->enum_type()->name();
  } else if (IsMessage(fd) && !IsList(fd)) {
    type = fd->message_type()->name();
  } else {
    type = FieldToSBEType(fd).xml_type;
  }
  *b += indent + "<field name=\"" + SnakeToCamel(fd->name()) + "\" id=\"" +
        std::to_string(fd->number()) + "\" type=\"" + type + "\"/>\n";
}

void WriteGroup(std::string* b, const gpb::FieldDescriptor* fd, const std::string& indent) {
  *b += indent + "<group name=\"" + SnakeToCamel(fd->name()) + "\" id=\"" +
        std::to_string(fd->number()) + "\">\n";
  for (const gpb::FieldDescriptor* f : SortedFields(fd->message_type())) {
    WriteField(b, f, indent + "    ");
  }
  *b += indent + "</group>\n";
}

void WriteMessage(std::string* b, const gpb::Descriptor* md, uint32_t template_id) {
  *b += "    <sbe:message name=\"" + std::string(md->name()) + "\" id=\"" +
        std::to_string(template_id) + "\">\n";
  for (const gpb::FieldDescriptor* f : SortedFields(md)) {
    if (IsList(f) && IsMessage(f)) {
      WriteGroup(b, f, "        ");
    } else {
      WriteField(b, f, "        ");
    }
  }
  *b += "    </sbe:message>\n";
}

}  // namespace

StatusOr<std::string> ProtoToXML(const std::vector<const gpb::FileDescriptor*>& files) {
  if (files.empty() || files[0] == nullptr) return Status::Error("sbe: no files provided");
  const gpb::FileDescriptor* fd = files[0];

  const auto schema_id = GetFileUint32Option(fd, kExtSchemaID);
  if (!schema_id) {
    if (std::string stale = StaleFileError(fd); !stale.empty()) return Status::Error(stale);
    return Status::Error("sbe: file " + std::string(fd->name()) + " missing (sbe.schema_id)");
  }
  const uint32_t version = GetFileUint32Option(fd, kExtVersion).value_or(0);

  Collected c;
  for (int i = 0; i < fd->enum_type_count(); ++i) AddEnum(&c, fd->enum_type(i));
  for (int i = 0; i < fd->message_type_count(); ++i) {
    const gpb::Descriptor* md = fd->message_type(i);
    if (GetMessageUint32Option(md, kExtTemplateID)) {
      CollectTypes(md, &c);
    } else if (c.comp_seen.insert(std::string(md->full_name())).second) {
      // A top-level message that is not a template is a composite.
      c.composites.push_back(md);
    }
  }

  std::string b = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  b += "<sbe:messageSchema xmlns:sbe=\"http://fixprotocol.io/2016/sbe\"\n";
  b += "                   package=\"" + std::string(fd->package()) + "\"\n";
  b += "                   id=\"" + std::to_string(*schema_id) + "\"\n";
  b += "                   version=\"" + std::to_string(version) + "\"\n";
  b += "                   byteOrder=\"littleEndian\">\n";
  b += "    <types>\n";
  b += "        <composite name=\"messageHeader\">\n";
  b += "            <type name=\"blockLength\" primitiveType=\"uint16\"/>\n";
  b += "            <type name=\"templateId\" primitiveType=\"uint16\"/>\n";
  b += "            <type name=\"schemaId\" primitiveType=\"uint16\"/>\n";
  b += "            <type name=\"version\" primitiveType=\"uint16\"/>\n";
  b += "        </composite>\n";
  b += "        <composite name=\"groupSizeEncoding\">\n";
  b += "            <type name=\"blockLength\" primitiveType=\"uint16\"/>\n";
  b += "            <type name=\"numInGroup\" primitiveType=\"uint16\"/>\n";
  b += "        </composite>\n";
  // Named string and bytes types, in ascending length.
  for (uint32_t l : c.str_lengths) {
    b += "        <type name=\"" + StrTypeName(l) + "\" primitiveType=\"char\" length=\"" +
         std::to_string(l) + "\"/>\n";
  }
  for (uint32_t l : c.bytes_lengths) {
    b += "        <type name=\"" + BytesTypeName(l) + "\" primitiveType=\"uint8\" length=\"" +
         std::to_string(l) + "\"/>\n";
  }
  for (const gpb::EnumDescriptor* ed : c.enums) WriteEnum(&b, ed);
  for (const gpb::Descriptor* md : c.composites) WriteComposite(&b, md);
  b += "    </types>\n";
  for (int i = 0; i < fd->message_type_count(); ++i) {
    const gpb::Descriptor* md = fd->message_type(i);
    if (auto tid = GetMessageUint32Option(md, kExtTemplateID)) WriteMessage(&b, md, *tid);
  }
  b += "</sbe:messageSchema>\n";
  return b;
}

}  // namespace protowire::sbe

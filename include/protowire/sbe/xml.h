// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// FIX SBE XML message schemas: parsing one, converting one to a .proto file
// with SBE annotations, and the reverse. Ported from protowire-go's
// encoding/sbe (xmlschema.go, xmltoproto.go, prototoxml.go), whose output this
// matches byte for byte (scripts/go_differential.sh).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/descriptor.h>

#include "protowire/detail/status.h"

namespace protowire::sbe {

// XMLType is an SBE simple type definition.
struct XMLType {
  std::string name;
  std::string primitive_type;
  uint32_t length = 0;
  std::string description;
  // The IANA charset name a char type's bytes are in. FIX SBE makes US-ASCII
  // the default for a char array.
  std::string character_encoding;
};

// XMLRef is a reference to another type within a composite.
struct XMLRef {
  std::string name;
  std::string type;
};

// XMLComposite is an SBE composite type definition.
struct XMLComposite {
  std::string name;
  std::string description;
  std::vector<XMLType> types;
  std::vector<XMLRef> refs;
};

// XMLValidValue is one value of an SBE enum: its text exactly as written.
struct XMLValidValue {
  std::string name;
  std::string value;
};

// XMLEnum is an SBE enum type definition.
struct XMLEnum {
  std::string name;
  std::string encoding_type;
  std::string description;
  std::vector<XMLValidValue> valid_values;
};

// XMLField is a field within an SBE message or group.
struct XMLField {
  std::string name;
  uint32_t id = 0;
  std::string type;
};

// XMLGroup is an SBE repeating group.
struct XMLGroup {
  std::string name;
  uint32_t id = 0;
  std::vector<XMLField> fields;
};

// XMLMessage is an SBE message (template).
struct XMLMessage {
  std::string name;
  uint32_t id = 0;
  std::string description;
  std::vector<XMLField> fields;
  std::vector<XMLGroup> groups;
};

// XMLTypes holds the type definitions of every <types> element in a schema.
struct XMLTypes {
  std::vector<XMLType> types;
  std::vector<XMLComposite> composites;
  std::vector<XMLEnum> enums;
};

// XMLSchema is an SBE XML message schema.
struct XMLSchema {
  std::string package;
  uint32_t id = 0;
  uint32_t version = 0;
  std::string byte_order;
  std::string description;
  XMLTypes types;
  std::vector<XMLMessage> messages;
};

// ParseXMLSchema parses an SBE XML schema, with or without the sbe: prefix.
// Elements and attributes it does not know are ignored, as in protowire-go.
// The text of an error for malformed XML is tinyxml2's, not Go's encoding/xml.
StatusOr<XMLSchema> ParseXMLSchema(std::string_view data);

// XMLToProto converts an SBE XML schema to a .proto file with SBE annotations.
// It refuses an array no proto field can hold. A char type whose
// characterEncoding is not UTF-8 compatible becomes a bytes field
// (protowire#494).
StatusOr<std::string> XMLToProto(std::string_view xml);

// ProtoToXML converts the first of files, with SBE annotations, to an SBE XML
// schema. A file without (sbe.schema_id) is an error, and so is one that
// carries it at the retired option number.
StatusOr<std::string> ProtoToXML(const std::vector<const google::protobuf::FileDescriptor*>& files);

}  // namespace protowire::sbe

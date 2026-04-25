#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <google/protobuf/descriptor.h>

#include "protowire/detail/status.h"

namespace protowire::sbe {

// SBE encoding type identifiers (specification names).
inline constexpr const char* kEncInt8 = "int8";
inline constexpr const char* kEncInt16 = "int16";
inline constexpr const char* kEncInt32 = "int32";
inline constexpr const char* kEncInt64 = "int64";
inline constexpr const char* kEncUint8 = "uint8";
inline constexpr const char* kEncUint16 = "uint16";
inline constexpr const char* kEncUint32 = "uint32";
inline constexpr const char* kEncUint64 = "uint64";
inline constexpr const char* kEncFloat = "float";
inline constexpr const char* kEncDouble = "double";
inline constexpr const char* kEncChar = "char";

struct FieldTemplate {
  const google::protobuf::FieldDescriptor* fd = nullptr;
  uint16_t offset = 0;
  uint16_t size = 0;
  std::string encoding;                    // empty for composite (nested)
  std::vector<FieldTemplate> composite;    // non-empty for nested message
};

struct GroupTemplate {
  const google::protobuf::FieldDescriptor* fd = nullptr;
  uint16_t block_length = 0;
  std::vector<FieldTemplate> fields;
};

struct MessageTemplate {
  uint16_t template_id = 0;
  uint16_t schema_id = 0;
  uint16_t version = 0;
  uint16_t block_length = 0;
  std::vector<FieldTemplate> fields;
  std::vector<GroupTemplate> groups;
};

// Builds a message template from a proto descriptor. The schema_id and
// version come from the enclosing FileDescriptor's SBE options.
StatusOr<MessageTemplate> BuildTemplate(
    const google::protobuf::Descriptor* md, uint16_t schema_id,
    uint16_t version);

}  // namespace protowire::sbe

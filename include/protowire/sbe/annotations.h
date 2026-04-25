#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

namespace protowire::sbe {

// SBE extension field numbers (from proto/sbe/annotations.proto).
inline constexpr int kExtSchemaID = 50100;
inline constexpr int kExtVersion = 50101;
inline constexpr int kExtTemplateID = 50200;
inline constexpr int kExtLength = 50300;
inline constexpr int kExtEncoding = 50301;

// Reads a uint32 option from a *Options message by field number. Walks both
// the message's regular fields (for SourceTree-loaded descriptors) and its
// unknown-field bytes (for descriptor sets where the extension wasn't
// registered).
std::optional<uint32_t> GetUint32Option(
    const google::protobuf::Message& options, int field_number);

std::optional<std::string> GetStringOption(
    const google::protobuf::Message& options, int field_number);

// Convenience helpers.
std::optional<uint32_t> GetFileUint32Option(
    const google::protobuf::FileDescriptor* fd, int field_number);
std::optional<uint32_t> GetMessageUint32Option(
    const google::protobuf::Descriptor* md, int field_number);
std::optional<uint32_t> GetFieldUint32Option(
    const google::protobuf::FieldDescriptor* fd, int field_number);
std::optional<std::string> GetFieldStringOption(
    const google::protobuf::FieldDescriptor* fd, int field_number);

}  // namespace protowire::sbe

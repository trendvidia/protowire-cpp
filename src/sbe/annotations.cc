// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/sbe/annotations.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/unknown_field_set.h>

#include "protowire/detail/wire.h"

namespace protowire::sbe {

namespace pb = google::protobuf;

namespace {

// Walks reflective fields of an options message.
std::optional<uint32_t> ScanFieldsUint32(const pb::Message& opts, int field_number) {
  const pb::Reflection* r = opts.GetReflection();
  std::vector<const pb::FieldDescriptor*> fields;
  r->ListFields(opts, &fields);
  for (const pb::FieldDescriptor* fd : fields) {
    if (fd->number() == field_number) {
      switch (fd->cpp_type()) {
        case pb::FieldDescriptor::CPPTYPE_UINT32:
          return r->GetUInt32(opts, fd);
        case pb::FieldDescriptor::CPPTYPE_INT32:
          return static_cast<uint32_t>(r->GetInt32(opts, fd));
        default:
          return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> ScanFieldsString(const pb::Message& opts, int field_number) {
  const pb::Reflection* r = opts.GetReflection();
  std::vector<const pb::FieldDescriptor*> fields;
  r->ListFields(opts, &fields);
  for (const pb::FieldDescriptor* fd : fields) {
    if (fd->number() == field_number && fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_STRING) {
      std::string scratch;
      return std::string(r->GetStringReference(opts, fd, &scratch));
    }
  }
  return std::nullopt;
}

// Walks the unknown-field byte stream from the options message and returns
// the value of a varint or length-prefixed bytes field by number. When
// `as_string` is set, returns the bytes as string.
std::optional<uint32_t> ScanUnknownUint32(const pb::Message& opts, int field_number) {
  // UnknownFieldSet is the high-level API.
  const pb::UnknownFieldSet& uf = opts.GetReflection()->GetUnknownFields(opts);
  for (int i = 0; i < uf.field_count(); ++i) {
    const pb::UnknownField& u = uf.field(i);
    if (u.number() != field_number) continue;
    switch (u.type()) {
      case pb::UnknownField::TYPE_VARINT:
        return static_cast<uint32_t>(u.varint());
      case pb::UnknownField::TYPE_FIXED32:
        return u.fixed32();
      default:
        return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ScanUnknownString(const pb::Message& opts, int field_number) {
  const pb::UnknownFieldSet& uf = opts.GetReflection()->GetUnknownFields(opts);
  for (int i = 0; i < uf.field_count(); ++i) {
    const pb::UnknownField& u = uf.field(i);
    if (u.number() != field_number) continue;
    if (u.type() == pb::UnknownField::TYPE_LENGTH_DELIMITED) {
      return std::string(u.length_delimited());
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<uint32_t> GetUint32Option(const pb::Message& opts, int field_number) {
  if (auto v = ScanFieldsUint32(opts, field_number); v.has_value()) return v;
  return ScanUnknownUint32(opts, field_number);
}

std::optional<std::string> GetStringOption(const pb::Message& opts, int field_number) {
  if (auto v = ScanFieldsString(opts, field_number); v.has_value()) return v;
  return ScanUnknownString(opts, field_number);
}

std::optional<uint32_t> GetFileUint32Option(const pb::FileDescriptor* fd, int field_number) {
  return GetUint32Option(fd->options(), field_number);
}
std::optional<uint32_t> GetMessageUint32Option(const pb::Descriptor* md, int field_number) {
  return GetUint32Option(md->options(), field_number);
}
std::optional<uint32_t> GetFieldUint32Option(const pb::FieldDescriptor* fd, int field_number) {
  return GetUint32Option(fd->options(), field_number);
}
std::optional<std::string> GetFieldStringOption(const pb::FieldDescriptor* fd, int field_number) {
  return GetStringOption(fd->options(), field_number);
}

namespace {

// CarriesRetired reports whether opts, the options of a descriptor in pool,
// has field number as an unknown field the pool does not declare.
bool CarriesRetired(const google::protobuf::DescriptorPool* pool,
                    const google::protobuf::Message& opts,
                    int number) {
  // The pool's own options type, which is not the generated one when the file
  // was compiled with descriptor.proto in its pool.
  const google::protobuf::Descriptor* type = pool->FindMessageTypeByName(opts.GetTypeName());
  if (type == nullptr) type = opts.GetDescriptor();
  if (pool->FindExtensionByNumber(type, number) != nullptr) return false;
  const google::protobuf::UnknownFieldSet& unknown = opts.GetReflection()->GetUnknownFields(opts);
  for (int i = 0; i < unknown.field_count(); ++i) {
    if (unknown.field(i).number() == number) return true;
  }
  return false;
}

std::string StaleText(const std::string& what, const char* option, int retired, int registered) {
  return "sbe: " + what + " carries (sbe." + option + ") at retired option number " +
         std::to_string(retired) + " (registered as " + std::to_string(registered) +
         " since protowire v1.12.0): the descriptor predates the registered extension block and "
         "must be recompiled against the current sbe/annotations.proto";
}

}  // namespace

std::string StaleFileError(const google::protobuf::FileDescriptor* fd) {
  if (!CarriesRetired(fd->pool(), fd->options(), kRetiredSchemaID)) return "";
  return StaleText("file " + std::string(fd->name()), "schema_id", kRetiredSchemaID, kExtSchemaID);
}

std::string StaleMessageError(const google::protobuf::Descriptor* md) {
  if (!CarriesRetired(md->file()->pool(), md->options(), kRetiredTemplateID)) return "";
  return StaleText(
      "message " + std::string(md->full_name()), "template_id", kRetiredTemplateID, kExtTemplateID);
}

}  // namespace protowire::sbe

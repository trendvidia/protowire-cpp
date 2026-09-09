// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/annotations.h"

#include "pxf/annotations.pb.h"

namespace protowire::pxf {

bool IsRequired(const google::protobuf::FieldDescriptor* fd) {
  if (fd == nullptr) return false;
  return fd->options().GetExtension(::pxf::required);
}

std::optional<std::string> GetDefault(const google::protobuf::FieldDescriptor* fd) {
  if (fd == nullptr) return std::nullopt;
  const auto& opts = fd->options();
  if (!opts.HasExtension(::pxf::default_)) return std::nullopt;
  return opts.GetExtension(::pxf::default_);
}

std::optional<std::string> KeyFieldName(const google::protobuf::FieldDescriptor* fd) {
  if (fd == nullptr) return std::nullopt;
  const auto& opts = fd->options();
  if (!opts.HasExtension(::pxf::key)) return std::nullopt;
  return opts.GetExtension(::pxf::key);
}

const google::protobuf::FieldDescriptor* KeyField(const google::protobuf::FieldDescriptor* fd) {
  using google::protobuf::FieldDescriptor;
  if (fd == nullptr || !fd->is_repeated() || fd->is_map() ||
      fd->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
    return nullptr;
  }
  auto name = KeyFieldName(fd);
  if (!name.has_value() || name->empty()) return nullptr;
  const FieldDescriptor* kf = fd->message_type()->FindFieldByName(*name);
  if (kf == nullptr || kf->is_repeated() || kf->is_map() ||
      kf->type() != FieldDescriptor::TYPE_STRING) {
    return nullptr;
  }
  return kf;
}

bool IsKeyed(const google::protobuf::FieldDescriptor* fd) {
  return KeyField(fd) != nullptr;
}

}  // namespace protowire::pxf

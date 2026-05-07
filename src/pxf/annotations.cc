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

}  // namespace protowire::pxf

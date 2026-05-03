#pragma once

#include <optional>
#include <string>

#include <google/protobuf/descriptor.h>

namespace protowire::pxf {

// IsRequired returns true if the field has (pxf.required) = true.
bool IsRequired(const google::protobuf::FieldDescriptor* fd);

// GetDefault returns the (pxf.default) string if the field has one set.
// The returned string is the raw PXF literal as written in the .proto
// (e.g. "42", "true", "viewer").
std::optional<std::string> GetDefault(
    const google::protobuf::FieldDescriptor* fd);

}  // namespace protowire::pxf

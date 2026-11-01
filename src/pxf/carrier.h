// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// PXF has two spellings for `required` and `default`. The bracket form
// `[(pxf.required) = true]` / `[(pxf.default) = "…"]` lowers to extensions
// 1314 / 1315. The annotation form `@required` / `@default(v)` lowers to one
// AnnotationList at extension 1327 on the field's options (RFC-001 §8.1). A
// compiler emits the one the author wrote and MUST NOT synthesize the other,
// so a runtime reading only 1314 / 1315 binds a migrated schema with no
// enforcement and no diagnostic.
//
// This reads the carrier's wire bytes directly: its shape is small and
// frozen by STABILITY.md promise 3, and the library links no generated
// schema package.

#pragma once

#include <optional>
#include <string>

#include <google/protobuf/descriptor.h>

namespace protowire::pxf {

// FieldAnnotations is what a field says through both spellings.
struct FieldAnnotations {
  bool required = false;
  bool required_from_carrier = false;  // @required rather than (pxf.required)
  std::optional<std::string> def;      // reduced to the bracket form's literal
  bool default_from_carrier = false;   // @default rather than (pxf.default)
  // Why the field's default cannot be used: an @default that reduces to no
  // single literal, or two spellings that disagree. ValidateFile reports it
  // rather than dropping the annotation, which would be the silent
  // non-enforcement the carrier path exists to end.
  std::string default_problem;
};

FieldAnnotations ReadFieldAnnotations(const google::protobuf::FieldDescriptor* fd);

}  // namespace protowire::pxf

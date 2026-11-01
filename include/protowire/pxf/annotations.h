// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <optional>
#include <string>

#include <google/protobuf/descriptor.h>

namespace protowire::pxf {

// IsRequired returns true if the field is required, in either spelling: the
// option `[(pxf.required) = true]`, or the annotation `@required` carried at
// extension 1327. Both are honoured; neither is synthesized from the other.
bool IsRequired(const google::protobuf::FieldDescriptor* fd);

// GetDefault returns the field's default if it has one, in either spelling:
// the option `[(pxf.default) = "…"]`, or `@default(v)` carried at extension
// 1327, whose typed argument is reduced to the same literal form. The
// returned string is a raw PXF literal (e.g. "42", "true", "viewer").
std::optional<std::string> GetDefault(const google::protobuf::FieldDescriptor* fd);

// KeyFieldName returns the raw (pxf.key) annotation value if set — the
// proto field name the schema designates as the key of a keyed repeated
// field (draft -01 §3.13). The value is returned even when its placement
// is invalid; ValidateFile reports placement violations and KeyField
// resolves the annotation only when it is well-placed.
std::optional<std::string> KeyFieldName(const google::protobuf::FieldDescriptor* fd);

// KeyField returns the key field descriptor of a keyed repeated field:
// the singular string field of fd's element message that fd's (pxf.key)
// annotation names (draft -01 §3.13). It returns nullptr when fd carries
// no (pxf.key) annotation or when the annotation's placement is invalid —
// fd is not a repeated message-typed field, the named field does not
// exist, or it is not a singular string field. Use ValidateFile to
// surface invalid placements as violations.
const google::protobuf::FieldDescriptor* KeyField(const google::protobuf::FieldDescriptor* fd);

// IsKeyed reports whether fd is a keyed repeated field — a repeated
// message-typed field with a valid (pxf.key) annotation. Equivalent to
// KeyField(fd) != nullptr.
bool IsKeyed(const google::protobuf::FieldDescriptor* fd);

}  // namespace protowire::pxf

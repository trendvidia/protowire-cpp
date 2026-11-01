// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Dotted column paths of a `@dataset` header (protowire#342, draft -01
// §3.4.4). A column may be a path such as "desc.energy": a non-empty cell
// in it stands for `energy = <value>` inside the block `desc { ... }`, and
// every cell of a row that shares a path prefix lands in that one block.

#pragma once

#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>

namespace protowire::pxf {

// ColumnPathError checks the header rules that need no schema: no path has
// an empty segment, no path appears twice, and no column is a prefix of
// another ("a" beside "a.b"), since a row could then set one sub-message
// twice. It returns the message for the first column that breaks one and
// sets *index to that column, or returns "".
std::string ColumnPathError(const std::vector<std::string>& columns, size_t* index);

// CheckColumnPaths adds the rules that need the row type: each segment
// names a field, and each segment but the last names a singular message
// field whose type is not written as a literal. It returns the message for
// the first column that breaks a rule, or "".
std::string CheckColumnPaths(const google::protobuf::Descriptor* desc,
                             const std::vector<std::string>& columns);

}  // namespace protowire::pxf

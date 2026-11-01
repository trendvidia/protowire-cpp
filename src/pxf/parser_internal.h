// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Entry points for callers inside the library. The parser's let a caller
// holding its own lexer share the one `@dataset` parser.

#pragma once

#include <string_view>

#include <google/protobuf/descriptor.h>

#include "protowire/detail/status.h"
#include "protowire/pxf/ast.h"
#include "protowire/pxf/lexer.h"

namespace protowire::pxf {

// ParseDatasetAt parses the `@dataset` directive at *current, whose next
// token *lex produces, and leaves both on the token after it.
StatusOr<DatasetDirective> ParseDatasetAt(Lexer* lex, Token* current, int max_depth);

// ParseDatasetRow parses one `( cell, ... )` of expected cells.
StatusOr<DatasetRow> ParseDatasetRow(std::string_view row, int expected, int max_depth);

// CheckDescriptorSchema runs ValidateDescriptor and joins any violations
// into one error, the bind-time check Unmarshal and DatasetReader share.
Status CheckDescriptorSchema(const google::protobuf::Descriptor* desc);

}  // namespace protowire::pxf

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <string>

#include "protowire/pxf/ast.h"

namespace protowire::pxf {

// FormatDocument pretty-prints a parsed AST `Document`, preserving comments.
// Unlike Marshal (which works from a proto.Message and loses comments), this
// formats directly from the AST returned by Parse().
std::string FormatDocument(const Document& doc);

}  // namespace protowire::pxf

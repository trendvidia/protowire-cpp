// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <string_view>

#include "protowire/detail/status.h"
#include "protowire/pxf/ast.h"
#include "protowire/pxf/options.h"

namespace protowire::pxf {

// Parses PXF source into an AST Document with comments attached. Enforces
// the limits in opts (HARDENING.md § Mandatory limits): the input size,
// the nesting depth of `{` and `[` (root at 0, the same count the decoder
// keeps, so a document exactly kMaxNestingDepth deep parses and decodes
// and one deeper does neither), and the decoded length of a b"…" literal.
StatusOr<Document> Parse(std::string_view input, ParseOptions opts = {});

}  // namespace protowire::pxf

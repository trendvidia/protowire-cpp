// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Formatter and keyed-field entry points for callers inside the library. The
// Rewriter renders with the document's own indent width, which the public
// FormatValue, fixed at two spaces, does not offer.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "protowire/pxf/ast.h"

namespace protowire::pxf {

// AppendValueIndented is AppendValue with indent as the per-level step.
void AppendValueIndented(std::string* out, const ValuePtr& value, std::string_view indent);

// AppendEntryIndented formats one entry as FormatDocument would at the top
// level, trailing newline included, with indent as the per-level step.
void AppendEntryIndented(std::string* out, const EntryPtr& entry, std::string_view indent);

// MapKeyQuoted decides the spelling of a map key on the way out.
bool MapKeyQuoted(const MapEntry& m);

// ExplicitKeyOf returns the value of the single explicit `key_name = "…"`
// assignment among entries. It is false when there is none, more than one, or
// the only candidate is quoted or not a string.
bool ExplicitKeyOf(const std::vector<EntryPtr>& entries,
                   std::string_view key_name,
                   std::string* key);

}  // namespace protowire::pxf

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Keyed repeated fields (draft -01 §3.13): schema-aware canonicalization
// of a parsed Document, the AST counterpart of the keyed block form the
// decoder reads and the encoder writes.

#pragma once

#include <google/protobuf/descriptor.h>

#include "protowire/pxf/ast.h"

namespace protowire::pxf {

// CanonicalizeKeyed rewrites doc in place to the canonical keyed form of
// draft -01 §3.13, using desc as the document's message schema. Per keyed
// repeated field binding it:
//
//   - converts an eligible anonymous list binding (every element with
//     exactly one non-empty, distinct explicit key assignment) to the
//     keyed block form, removing the now-implicit key assignments;
//   - normalizes `name = { ... }` entry spellings to `name { ... }`;
//   - unquotes quoted entry names that are identifier-safe;
//   - drops redundant (agreeing) explicit key-field assignments inside
//     named entries.
//
// Bindings that are not eligible for the keyed form — duplicate keys,
// absent or empty keys — are left in the anonymous form, and entries that
// don't resolve against the schema are left untouched, so formatting an
// invalid document never destroys information. Callers typically follow
// with FormatDocument; this pair is the reference `pxf fmt` pipeline.
void CanonicalizeKeyed(Document* doc, const google::protobuf::Descriptor* desc);

// IsIdentifierSafeEntryName reports whether s can be written as an
// unquoted entry name: it matches the grammar's identifier production
// (ident-start, then ident-part bytes — dots included, unlike a map key)
// and is not one of the value keywords null / true / false.
bool IsIdentifierSafeEntryName(std::string_view s);

}  // namespace protowire::pxf

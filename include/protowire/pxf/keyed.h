// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Keyed repeated fields (draft -01 §3.13): schema-aware canonicalization
// of a parsed Document, the AST counterpart of the keyed block form the
// decoder reads and the encoder writes.

#pragma once

#include <string>
#include <string_view>
#include <vector>

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

// KeyedErrorKind classifies the keyed-repeated-field errors of draft -01 §3.13.
enum class KeyedErrorKind {
  // Two entries in one keyed block whose names are equal after unquoting.
  kDuplicateKey = 1,
  // An explicit key-field assignment in a named entry that disagrees with the
  // entry name, or a binding that would undo it.
  kKeyConflict,
  // The empty string as a key: a quoted entry name "", or an explicit
  // assignment to the key field of an element, in either surface form.
  kEmptyKey,
  // A quoted entry name anywhere but in a keyed repeated field's block.
  kQuotedNameUnkeyed,
};

const char* KeyedErrorKindName(KeyedErrorKind k);

// KeyedError is one keyed-repeated-field diagnostic.
struct KeyedError {
  Position pos;
  KeyedErrorKind kind;
  std::string field;  // the keyed field's name; empty for kQuotedNameUnkeyed
  std::string key;    // the offending key or entry name, when one applies
  std::string msg;
  std::string ToString() const { return pos.ToString() + ": " + msg; }
};

// KeyedDiagnostics walks a parsed document against desc and returns every keyed
// repeated field violation of draft -01 §3.13, in source order: duplicate entry
// names in a keyed block, empty keys, key-field assignments that disagree with
// their entry name, and quoted entry names outside a keyed field's block.
//
// It is the tolerant counterpart to the errors Unmarshal returns. Editor
// tooling runs ParseTolerant, binds a schema later, and shows these. Entries
// that don't resolve against the schema are skipped: those are ordinary schema
// errors.
std::vector<KeyedError> KeyedDiagnostics(const Document& doc,
                                         const google::protobuf::Descriptor* desc);

// IsIdentifierSafeEntryName reports whether s can be written as an
// unquoted entry name: it matches the grammar's identifier production
// (ident-start, then ident-part bytes, dots included) and is not one of
// the value keywords null / true / false. IsIdentifierSafe (format.h),
// the string map-key test, is the same rule (protowire#313).
bool IsIdentifierSafeEntryName(std::string_view s);

}  // namespace protowire::pxf

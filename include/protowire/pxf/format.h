// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <string>
#include <string_view>

#include "protowire/pxf/ast.h"

namespace protowire::pxf {

// IsIdentifierSafe reports whether s can be written bare and read back as
// the string s: it matches the grammar's identifier production (a letter
// or underscore, then letters, digits, underscores and dots) and is not
// one of the value keywords "null", "true", "false" — bare, those are a
// bool key or no key at all, and a leading digit makes an integer key.
// The formatter and the marshaller share this one test so they agree on
// every key both can produce (draft -01 § Entries and Keys;
// protowire#306), and it is the same test keyed entry names use
// (protowire#313): "a.b" is bare, ".e" and "1.5" fail ident-start and
// stay quoted.
bool IsIdentifierSafe(std::string_view s);

// FormatDocument pretty-prints a parsed AST `Document`, preserving comments.
// Unlike Marshal (which works from a proto.Message and loses comments), this
// formats directly from the AST returned by Parse().
std::string FormatDocument(const Document& doc);

}  // namespace protowire::pxf

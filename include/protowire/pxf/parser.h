// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <string_view>
#include <vector>

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

// TolerantResult is a best-effort document and every error found in it, in
// source order.
struct TolerantResult {
  Document document;
  std::vector<Error> errors;
};

// ParseTolerant parses a document that may be broken, for editor tooling that
// needs structure exactly then: completion mid-keystroke, hover on a half-typed
// entry. Where Parse stops at the first syntax error, this recovers at entry
// and block boundaries. A valid input gives the AST Parse gives and no errors.
//
//   - A missing or malformed value (`key =`, `key` alone) becomes a BadVal, so
//     the entry and its position stay in the AST.
//   - An unclosed block or list closes at EOF with what it holds. One in a
//     @dataset cell closes at the row's ')', so later rows still parse.
//   - An unterminated string ends at the newline or EOF. Unterminated
//     triple-quoted strings and block comments end at EOF, bytes literals at
//     the newline.
//   - A malformed directive is skipped to the next directive or body entry.
//
// The AST may hold BadVal nodes and is for tooling. It is not for decoding:
// use Parse or Unmarshal once the document is valid.
TolerantResult ParseTolerant(std::string_view input, ParseOptions opts = {});

}  // namespace protowire::pxf

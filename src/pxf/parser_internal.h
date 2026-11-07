// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Entry points for callers inside the library. The parser's let a caller
// holding its own lexer share the one `@dataset` parser.

#pragma once

#include <cstdint>
#include <string_view>

#include <google/protobuf/descriptor.h>

#include "protowire/detail/status.h"
#include "protowire/pxf/ast.h"
#include "protowire/pxf/lexer.h"

namespace protowire::pxf {

// ParseDatasetAt parses the `@dataset` directive at *current, whose next
// token *lex produces, and leaves both on the token after it.
StatusOr<DatasetDirective> ParseDatasetAt(Lexer* lex, Token* current, int max_depth);

// FindMatchingBrace returns the offset of the `}` matching the `{` at
// open_offset, or -1 if there is none. Mirrors protowire-go's braceMatch, so
// braces inside literals and comments don't count. When there is none
// because the body can never close, a simple string or bytes literal meeting
// a newline, *failed (if given) is set; otherwise the input ends first.
int FindMatchingBrace(std::string_view input, int open_offset, bool* failed = nullptr);

// BraceMatch matches an @proto body's braces as FindMatchingBrace does, and
// can resume: Scan reads input from i and returns where it stopped, with
// *state kClosed at the matching `}`, kFailed where the body can never
// close, or kOpen where the input ended first, to scan again from there
// once more input has arrived. Mirrors protowire-go's braceMatch.
struct BraceMatch {
  enum class State : uint8_t { kOpen, kClosed, kFailed };
  enum class Mode : uint8_t { kCode, kString, kTripleString, kBytes, kLineComment, kBlockComment };
  int Scan(std::string_view input, int i, State* state);

  int depth = 1;  // the opening `{` is read
  Mode mode = Mode::kCode;
  bool esc = false;  // in a string, after a backslash
  int run = 0;       // quotes just seen in a triple string, or 1 after a '*'
};

// The error for anything after a @dataset header that is neither a row nor
// a directive (draft §3.4.4).
inline constexpr const char* kDatasetBesideEntries =
    "@dataset directive cannot coexist with top-level field entries; the document's payload is "
    "the @dataset rows (draft §3.4.4)";

// ParseDatasetRow parses one `( cell, ... )` of expected cells, under the
// caller's nesting and bytes-literal limits. The row starts at `start` in
// its stream, and the row, its cells and its errors carry positions in the
// stream, as protowire-go's do (#161). Input that does not open with `(`
// is an entry beside the dataset, as Parse reports it. On success
// *end_offset, if given, is the stream offset just past the row's `)`;
// what follows it in `row` is not read.
StatusOr<DatasetRow> ParseDatasetRow(std::string_view row,
                                     int expected,
                                     int max_depth,
                                     int max_bytes_literal,
                                     Position start,
                                     int* end_offset = nullptr);

// CheckDescriptorSchema runs ValidateDescriptor and joins any violations
// into one error, the bind-time check Unmarshal and DatasetReader share.
Status CheckDescriptorSchema(const google::protobuf::Descriptor* desc);

// WktShapeProblem says what stops d having the shape its full name needs
// (draft -01 §wkt-shapes), or returns "" when it has it or the name needs none.
std::string WktShapeProblem(const google::protobuf::Descriptor* d);

}  // namespace protowire::pxf

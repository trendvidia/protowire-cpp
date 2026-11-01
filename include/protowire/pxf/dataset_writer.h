// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Writing `@dataset` tables (draft §3.4.4): the header and one row per
// message. Cells are rendered by the encoder Marshal uses, so a cell reads
// back exactly as the same field would in a Marshal document.
//
// A row depends only on its message and the column list, never on the rows
// before it, so appending rows to a file that already holds the header needs
// no rewrite of earlier bytes.

#pragma once

#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "protowire/detail/status.h"

namespace protowire::pxf {

// AppendDatasetHeader appends the header `@dataset <type> (<columns>)` for
// rows of type desc, and a newline, to dst.
//
// Any field of desc can be a column: a cell may hold a list or a block
// (protowire#341). A column may also be a path into a sub-message, such as
// `inner.x` (protowire#342), through singular message fields only. It returns
// an error, and leaves dst unchanged, for an empty column list and for a
// column the reader would refuse: a name that is not a field, a path with an
// empty segment, a column twice, a field beside a path inside it (`inner`
// with `inner.x`), or a path through a repeated or map field or into a type
// written as a literal. A (pxf.required) field of desc that is neither a
// column nor under one is an error too: every row would be rejected on
// reading. So is a schema that ValidateDescriptor refuses, which Unmarshal
// would refuse to read.
Status AppendDatasetHeader(std::string* dst,
                           const google::protobuf::Descriptor* desc,
                           const std::vector<std::string>& columns);

// AppendDatasetRow appends msg as one row of a `@dataset` table with these
// columns, and a newline, to dst. Use the columns given to
// AppendDatasetHeader.
//
// A set field is written as Marshal writes its value. An unset field is an
// empty cell, which reads back as absent, except where reading an empty cell
// would not give back the unset field:
//
//   - A field with (pxf.default) would take the default. A proto3 scalar
//     without presence is written as its zero value, which the default does
//     not replace. A message-typed field (a wrapper, a Timestamp, ...) is
//     written `null`, which keeps the default off and leaves the field
//     unset. A scalar with presence (`optional`, or a oneof member) stays
//     empty and reads back as its default: the draft rejects `null` on a
//     scalar field, so no cell holds it unset, and an absent field with a
//     default means the default. A oneof member stays empty when another
//     member of its oneof is set and among the columns, since that member
//     keeps the default off. A set member that is not a column is not
//     written, so the oneof is treated as having no member set.
//   - A (pxf.required) field would be rejected. A proto3 scalar is written
//     as its zero value; an unset field with presence is an error.
//
// A message-typed field named in the message's `_null` FieldMask is written
// `null`, as Marshal writes it. A scalar field named there is written as if
// the mask did not name it, since the draft rejects `null` on a scalar field
// (trendvidia/protowire#339).
//
// A dotted column is written through its parents: an unset parent makes the
// cell empty, and the leaf follows the rules above inside its parent. A
// sub-message is present on reading exactly when a cell under it is non-empty
// (protowire#342), so one that is set but has every column under it empty
// reads back unset; that loss is accepted, as for a field outside the
// columns. A row that the reader would reject for it is refused instead: a
// required sub-message whose cells are all empty, or a sub-message that reads
// back present while one of its required fields is not a column.
//
// Fields not among the columns are not written, so a table can export a
// subset of a message's fields. They read back absent, taking their
// (pxf.default) if they have one. A (pxf.required) field cannot be left out:
// the column list is refused.
//
// A list, map or message that needs a block is written as Marshal writes it,
// on one line, so a table keeps one row per line. Inside it, an unset field
// with (pxf.default) or (pxf.required) follows the rules above, since the
// reader applies both in every body of a cell: a scalar without presence is
// written as its zero value, a message-typed field `null`, an `optional` or
// oneof scalar not at all (it reads back as its default), and an unset
// required field with presence is an error.
//
// It returns an error, and leaves dst unchanged, for the column errors of
// AppendDatasetHeader and for a value Marshal refuses.
Status AppendDatasetRow(std::string* dst,
                        const google::protobuf::Message& msg,
                        const std::vector<std::string>& columns);

}  // namespace protowire::pxf

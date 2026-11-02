// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// What the @dataset writer shares with the encoder, so that a cell reads back
// exactly as the same field would in a Marshal document.

#pragma once

#include <string>
#include <unordered_set>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "protowire/detail/status.h"

namespace protowire::pxf {

// NullSet holds the paths of the fields written as null.
using NullSet = std::unordered_set<std::string>;

// Nullable is whether null can bind to fd (the decoder's CheckNullable). A
// field a null set names that cannot is written as if it were not named.
bool Nullable(const google::protobuf::FieldDescriptor* fd);

// IsLiteralMessage reports whether sub is written as one literal. A Secret
// is only without metadata, which the literal would drop.
bool IsLiteralMessage(const google::protobuf::Message& sub);

// WriteCellScalar appends singular scalar field fd of msg.
void WriteCellScalar(const google::protobuf::Message& msg,
                     const google::protobuf::FieldDescriptor* fd,
                     std::string* out);

// WriteCellLiteral appends sub, for which IsLiteralMessage holds, as its
// literal.
Status WriteCellLiteral(const google::protobuf::Message& sub, std::string* out);

// WriteValueCell appends a list, map or block value as one cell
// (protowire#341): the text Marshal writes for fd of msg, on one line.
// null_set is nullptr when no nulls are tracked.
Status WriteValueCell(const google::protobuf::Message& msg,
                      const google::protobuf::FieldDescriptor* fd,
                      const NullSet* null_set,
                      std::string* out);

// CellUnset decides how a value cell writes fd, which msg does not set; see
// its definition in dataset_writer.cc. path names fd in an error.
Status CellUnset(const google::protobuf::Message& msg,
                 const google::protobuf::FieldDescriptor* fd,
                 const std::string& path,
                 bool* write,
                 bool* null);

}  // namespace protowire::pxf

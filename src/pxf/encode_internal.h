// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// What the @dataset writer shares with the encoder, so that a cell reads back
// exactly as the same field would in a Marshal document.

#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "protowire/detail/status.h"

namespace protowire::pxf {

class TypeResolver;

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

// AnyBodyReadsBack reports whether body, the entries of an Any block at the
// given depth, decodes to exactly want, the bytes of the Any's value. Only a
// reader knows everything a body can lose, so this asks one: it uses the
// default limits and runs the post-decode pass, as UnmarshalFull does, because
// a value that pass would change is not safe for that reader.
bool AnyBodyReadsBack(std::string_view body,
                      const google::protobuf::Descriptor* desc,
                      TypeResolver* resolver,
                      int depth,
                      std::string_view want);

// EmptyCollection is the literal for a list or map field with nothing in it.
const char* EmptyCollection(const google::protobuf::FieldDescriptor* fd);

// CellUnset decides how a value cell writes fd, which msg does not set; see
// its definition in dataset_writer.cc. path names fd in an error.
Status CellUnset(const google::protobuf::Message& msg,
                 const google::protobuf::FieldDescriptor* fd,
                 const std::string& path,
                 bool* write,
                 bool* null);

}  // namespace protowire::pxf

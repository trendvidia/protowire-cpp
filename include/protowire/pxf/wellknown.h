// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

namespace protowire::pxf {

// Detection helpers for google.protobuf.* well-known types.
bool IsTimestamp(const google::protobuf::Descriptor* d);
bool IsDuration(const google::protobuf::Descriptor* d);
bool IsAny(const google::protobuf::Descriptor* d);

// Returns the inner FieldDescriptor::CppType for a wrapper message (e.g.
// google.protobuf.StringValue → CPPTYPE_STRING). Returns -1 if not a
// wrapper type.
int WrapperInnerCppType(const google::protobuf::Descriptor* d);

// Sets seconds + nanos on a Timestamp / Duration message.
void SetTimestampFields(google::protobuf::Message* msg, int64_t seconds, int32_t nanos);
void SetDurationFields(google::protobuf::Message* msg, int64_t seconds, int32_t nanos);

// Reads seconds + nanos from a Timestamp / Duration message.
void ReadTimestampFields(const google::protobuf::Message& msg, int64_t* seconds, int32_t* nanos);
void ReadDurationFields(const google::protobuf::Message& msg, int64_t* seconds, int32_t* nanos);

// pxf.BigInt / pxf.Decimal / pxf.BigFloat detection by full name.
bool IsBigInt(const google::protobuf::Descriptor* d);
bool IsDecimal(const google::protobuf::Descriptor* d);
bool IsBigFloat(const google::protobuf::Descriptor* d);

// Set/read helpers for the byte-magnitude pxf.BigInt / pxf.Decimal /
// pxf.BigFloat schemas. The string form is a bare numeric literal as it
// appears in PXF text — these helpers parse/format it.
bool SetBigIntFromString(google::protobuf::Message* msg, std::string_view literal);
bool SetDecimalFromString(google::protobuf::Message* msg, std::string_view literal);
bool SetBigFloatFromString(google::protobuf::Message* msg, std::string_view literal);

std::string ReadBigIntAsString(const google::protobuf::Message& msg);
std::string ReadDecimalAsString(const google::protobuf::Message& msg);
std::string ReadBigFloatAsString(const google::protobuf::Message& msg);

// Locates the `_null` `google.protobuf.FieldMask` field on a descriptor used
// to track null fields across protobuf binary round-trips. Returns nullptr
// if the descriptor has no field named exactly `_null` of type FieldMask.
const google::protobuf::FieldDescriptor* FindNullMaskField(const google::protobuf::Descriptor* d);

// Appends a dotted field path to the FieldMask's `paths` repeated string.
void AppendNullPath(google::protobuf::Message* root,
                    const google::protobuf::FieldDescriptor* null_mask_fd,
                    std::string_view path);

// Reads all paths from a FieldMask field as a vector.
std::vector<std::string> ReadNullPaths(const google::protobuf::Message& root,
                                       const google::protobuf::FieldDescriptor* null_mask_fd);

}  // namespace protowire::pxf

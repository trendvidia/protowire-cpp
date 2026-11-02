// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// protobuf-go's marshalMessageSlow, under Deterministic. The field order is
// its order.LegacyFieldOrder.

#include "protowire/pxf/go_wire.h"

#include <algorithm>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/unknown_field_set.h>
#include <google/protobuf/wire_format_lite.h>

namespace protowire::pxf {

namespace pb = google::protobuf;
using FieldDescriptor = pb::FieldDescriptor;
using Lite = pb::internal::WireFormatLite;

namespace {

bool InRealOneof(const FieldDescriptor* fd) {
  return fd->real_containing_oneof() != nullptr;
}

bool GoFieldLess(const FieldDescriptor* x, const FieldDescriptor* y) {
  if (x->is_extension() != y->is_extension()) return x->is_extension();
  if (InRealOneof(x) != InRealOneof(y)) return InRealOneof(y);
  if (InRealOneof(x) && x->real_containing_oneof() != y->real_containing_oneof()) {
    return x->real_containing_oneof()->index() < y->real_containing_oneof()->index();
  }
  return x->number() < y->number();
}

void WriteMessage(const pb::Message& msg, pb::io::CodedOutputStream* out);

// WriteValue writes one value of fd with no tag. idx is an element of a
// repeated field, or -1.
void WriteValue(const pb::Message& msg,
                const FieldDescriptor* fd,
                int idx,
                pb::io::CodedOutputStream* out) {
  const pb::Reflection* r = msg.GetReflection();
  const bool rep = idx >= 0;
  switch (fd->type()) {
    case FieldDescriptor::TYPE_DOUBLE:
      Lite::WriteDoubleNoTag(rep ? r->GetRepeatedDouble(msg, fd, idx) : r->GetDouble(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_FLOAT:
      Lite::WriteFloatNoTag(rep ? r->GetRepeatedFloat(msg, fd, idx) : r->GetFloat(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_INT64:
      Lite::WriteInt64NoTag(rep ? r->GetRepeatedInt64(msg, fd, idx) : r->GetInt64(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_UINT64:
      Lite::WriteUInt64NoTag(rep ? r->GetRepeatedUInt64(msg, fd, idx) : r->GetUInt64(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_INT32:
      Lite::WriteInt32NoTag(rep ? r->GetRepeatedInt32(msg, fd, idx) : r->GetInt32(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_FIXED64:
      Lite::WriteFixed64NoTag(rep ? r->GetRepeatedUInt64(msg, fd, idx) : r->GetUInt64(msg, fd),
                              out);
      break;
    case FieldDescriptor::TYPE_FIXED32:
      Lite::WriteFixed32NoTag(rep ? r->GetRepeatedUInt32(msg, fd, idx) : r->GetUInt32(msg, fd),
                              out);
      break;
    case FieldDescriptor::TYPE_BOOL:
      Lite::WriteBoolNoTag(rep ? r->GetRepeatedBool(msg, fd, idx) : r->GetBool(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_UINT32:
      Lite::WriteUInt32NoTag(rep ? r->GetRepeatedUInt32(msg, fd, idx) : r->GetUInt32(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_ENUM:
      Lite::WriteEnumNoTag(rep ? r->GetRepeatedEnumValue(msg, fd, idx) : r->GetEnumValue(msg, fd),
                           out);
      break;
    case FieldDescriptor::TYPE_SFIXED32:
      Lite::WriteSFixed32NoTag(rep ? r->GetRepeatedInt32(msg, fd, idx) : r->GetInt32(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_SFIXED64:
      Lite::WriteSFixed64NoTag(rep ? r->GetRepeatedInt64(msg, fd, idx) : r->GetInt64(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_SINT32:
      Lite::WriteSInt32NoTag(rep ? r->GetRepeatedInt32(msg, fd, idx) : r->GetInt32(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_SINT64:
      Lite::WriteSInt64NoTag(rep ? r->GetRepeatedInt64(msg, fd, idx) : r->GetInt64(msg, fd), out);
      break;
    case FieldDescriptor::TYPE_STRING:
    case FieldDescriptor::TYPE_BYTES: {
      const std::string s = rep ? r->GetRepeatedString(msg, fd, idx) : r->GetString(msg, fd);
      out->WriteVarint32(static_cast<uint32_t>(s.size()));
      out->WriteString(s);
      break;
    }
    case FieldDescriptor::TYPE_MESSAGE: {
      const std::string s =
          SerializeGoOrder(rep ? r->GetRepeatedMessage(msg, fd, idx) : r->GetMessage(msg, fd));
      out->WriteVarint32(static_cast<uint32_t>(s.size()));
      out->WriteString(s);
      break;
    }
    case FieldDescriptor::TYPE_GROUP:
      WriteMessage(rep ? r->GetRepeatedMessage(msg, fd, idx) : r->GetMessage(msg, fd), out);
      break;
  }
}

Lite::WireType WireTypeOf(const FieldDescriptor* fd) {
  return Lite::WireTypeForFieldType(static_cast<Lite::FieldType>(fd->type()));
}

void WriteTagged(const pb::Message& msg,
                 const FieldDescriptor* fd,
                 int idx,
                 pb::io::CodedOutputStream* out) {
  out->WriteTag(Lite::MakeTag(fd->number(), WireTypeOf(fd)));
  WriteValue(msg, fd, idx, out);
  if (fd->type() == FieldDescriptor::TYPE_GROUP) {
    out->WriteTag(Lite::MakeTag(fd->number(), Lite::WIRETYPE_END_GROUP));
  }
}

// KeyLess is order.GenericKeyOrder: false before true, numbers by value and
// strings by bytes.
bool KeyLess(const pb::Message& a, const pb::Message& b, const FieldDescriptor* k) {
  const pb::Reflection* r = a.GetReflection();
  switch (k->cpp_type()) {
    case FieldDescriptor::CPPTYPE_BOOL:
      return !r->GetBool(a, k) && r->GetBool(b, k);
    case FieldDescriptor::CPPTYPE_INT32:
      return r->GetInt32(a, k) < r->GetInt32(b, k);
    case FieldDescriptor::CPPTYPE_INT64:
      return r->GetInt64(a, k) < r->GetInt64(b, k);
    case FieldDescriptor::CPPTYPE_UINT32:
      return r->GetUInt32(a, k) < r->GetUInt32(b, k);
    case FieldDescriptor::CPPTYPE_UINT64:
      return r->GetUInt64(a, k) < r->GetUInt64(b, k);
    default:
      return r->GetString(a, k) < r->GetString(b, k);
  }
}

void WriteMap(const pb::Message& msg, const FieldDescriptor* fd, pb::io::CodedOutputStream* out) {
  const pb::Reflection* r = msg.GetReflection();
  const int n = r->FieldSize(msg, fd);
  const FieldDescriptor* key = fd->message_type()->map_key();
  const FieldDescriptor* value = fd->message_type()->map_value();
  std::vector<const pb::Message*> entries;
  entries.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) entries.push_back(&r->GetRepeatedMessage(msg, fd, i));
  std::sort(entries.begin(), entries.end(), [&](const pb::Message* a, const pb::Message* b) {
    return KeyLess(*a, *b, key);
  });
  for (const pb::Message* e : entries) {
    std::string body;
    {
      pb::io::StringOutputStream stream(&body);
      pb::io::CodedOutputStream coded(&stream);
      WriteTagged(*e, key, -1, &coded);
      WriteTagged(*e, value, -1, &coded);
    }
    out->WriteTag(Lite::MakeTag(fd->number(), Lite::WIRETYPE_LENGTH_DELIMITED));
    out->WriteVarint32(static_cast<uint32_t>(body.size()));
    out->WriteString(body);
  }
}

void WriteField(const pb::Message& msg, const FieldDescriptor* fd, pb::io::CodedOutputStream* out) {
  const pb::Reflection* r = msg.GetReflection();
  if (fd->is_map()) {
    WriteMap(msg, fd, out);
  } else if (!fd->is_repeated()) {
    WriteTagged(msg, fd, -1, out);
  } else if (fd->is_packed()) {
    std::string body;
    {
      pb::io::StringOutputStream stream(&body);
      pb::io::CodedOutputStream coded(&stream);
      for (int i = 0; i < r->FieldSize(msg, fd); ++i) WriteValue(msg, fd, i, &coded);
    }
    out->WriteTag(Lite::MakeTag(fd->number(), Lite::WIRETYPE_LENGTH_DELIMITED));
    out->WriteVarint32(static_cast<uint32_t>(body.size()));
    out->WriteString(body);
  } else {
    for (int i = 0; i < r->FieldSize(msg, fd); ++i) WriteTagged(msg, fd, i, out);
  }
}

void WriteMessage(const pb::Message& msg, pb::io::CodedOutputStream* out) {
  const pb::Reflection* r = msg.GetReflection();
  std::vector<const FieldDescriptor*> fields;
  r->ListFields(msg, &fields);
  std::sort(fields.begin(), fields.end(), GoFieldLess);
  for (const FieldDescriptor* fd : fields) WriteField(msg, fd, out);
  std::string unknown;
  r->GetUnknownFields(msg).SerializeToString(&unknown);
  out->WriteString(unknown);
}

}  // namespace

std::string SerializeGoOrder(const pb::Message& msg) {
  std::string out;
  {
    pb::io::StringOutputStream stream(&out);
    pb::io::CodedOutputStream coded(&stream);
    WriteMessage(msg, &coded);
  }
  return out;
}

}  // namespace protowire::pxf

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/wellknown.h"

#include <cstddef>
#include <string>
#include <string_view>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "bigfloat.h"
#include "parser_internal.h"
#include "protowire/limits.h"
#include "protowire/pb_big.h"

namespace protowire::pxf {

namespace pb = google::protobuf;

namespace {

using Type = pb::FieldDescriptor::Type;

// WktField is one field a codec reads on a well-known type.
struct WktField {
  const char* name;
  Type type;
  bool repeated = false;
  bool optional = false;  // may be undeclared; when declared it must still match
};

constexpr WktField kSeconds[] = {{"seconds", pb::FieldDescriptor::TYPE_INT64},
                                 {"nanos", pb::FieldDescriptor::TYPE_INT32}};
constexpr WktField kAny[] = {{"type_url", pb::FieldDescriptor::TYPE_STRING},
                             {"value", pb::FieldDescriptor::TYPE_BYTES}};
constexpr WktField kFieldMask[] = {{"paths", pb::FieldDescriptor::TYPE_STRING, true}};
constexpr WktField kBigInt[] = {{"abs", pb::FieldDescriptor::TYPE_BYTES},
                                {"negative", pb::FieldDescriptor::TYPE_BOOL}};
constexpr WktField kDecimal[] = {{"unscaled", pb::FieldDescriptor::TYPE_BYTES},
                                 {"scale", pb::FieldDescriptor::TYPE_INT32},
                                 {"negative", pb::FieldDescriptor::TYPE_BOOL}};
constexpr WktField kBigFloat[] = {{"mantissa", pb::FieldDescriptor::TYPE_BYTES},
                                  {"exponent", pb::FieldDescriptor::TYPE_INT32},
                                  {"prec", pb::FieldDescriptor::TYPE_UINT32},
                                  {"negative", pb::FieldDescriptor::TYPE_BOOL}};
constexpr WktField kSecret[] = {{"value", pb::FieldDescriptor::TYPE_STRING},
                                {"hint", pb::FieldDescriptor::TYPE_STRING, false, true},
                                {"fingerprint", pb::FieldDescriptor::TYPE_STRING, false, true}};
constexpr WktField kBoolValue[] = {{"value", pb::FieldDescriptor::TYPE_BOOL}};
constexpr WktField kBytesValue[] = {{"value", pb::FieldDescriptor::TYPE_BYTES}};
constexpr WktField kDoubleValue[] = {{"value", pb::FieldDescriptor::TYPE_DOUBLE}};
constexpr WktField kFloatValue[] = {{"value", pb::FieldDescriptor::TYPE_FLOAT}};
constexpr WktField kInt32Value[] = {{"value", pb::FieldDescriptor::TYPE_INT32}};
constexpr WktField kInt64Value[] = {{"value", pb::FieldDescriptor::TYPE_INT64}};
constexpr WktField kStringValue[] = {{"value", pb::FieldDescriptor::TYPE_STRING}};
constexpr WktField kUInt32Value[] = {{"value", pb::FieldDescriptor::TYPE_UINT32}};
constexpr WktField kUInt64Value[] = {{"value", pb::FieldDescriptor::TYPE_UINT64}};

// WktShape is one row of draft -01 §wkt-shapes: the fields a message must
// declare to be treated as the well-known type its full name says it is.
// Field numbers are not in it, since PXF binds fields by name.
struct WktShape {
  std::string_view name;
  const WktField* fields;
  size_t count;
};

template <size_t N>
constexpr WktShape Shape(std::string_view name, const WktField (&fields)[N]) {
  return {name, fields, N};
}

constexpr WktShape kShapes[] = {
    Shape("google.protobuf.Timestamp", kSeconds),
    Shape("google.protobuf.Duration", kSeconds),
    Shape("google.protobuf.Any", kAny),
    Shape("google.protobuf.FieldMask", kFieldMask),
    Shape("pxf.BigInt", kBigInt),
    Shape("pxf.Decimal", kDecimal),
    Shape("pxf.BigFloat", kBigFloat),
    Shape("pxf.Secret", kSecret),
};

constexpr WktShape kWrappers[] = {
    Shape("google.protobuf.BoolValue", kBoolValue),
    Shape("google.protobuf.BytesValue", kBytesValue),
    Shape("google.protobuf.DoubleValue", kDoubleValue),
    Shape("google.protobuf.FloatValue", kFloatValue),
    Shape("google.protobuf.Int32Value", kInt32Value),
    Shape("google.protobuf.Int64Value", kInt64Value),
    Shape("google.protobuf.StringValue", kStringValue),
    Shape("google.protobuf.UInt32Value", kUInt32Value),
    Shape("google.protobuf.UInt64Value", kUInt64Value),
};

bool FieldFits(const pb::FieldDescriptor* fd, const WktField& f) {
  if (fd == nullptr) return f.optional;
  return fd->type() == f.type && !fd->is_map() && fd->is_repeated() == f.repeated;
}

// FindShapeField is FindFieldByName with a first look at index i, where every
// copy of a canonical schema declares the field. This runs for each value of
// a well-known type: with only the hash lookup it cost 10% of a decode and 20%
// of an encode on a document of nothing else, and this about halves that.
const pb::FieldDescriptor* FindShapeField(const pb::Descriptor* d, size_t i, const char* name) {
  if (static_cast<int>(i) < d->field_count() && d->field(static_cast<int>(i))->name() == name) {
    return d->field(static_cast<int>(i));
  }
  return d->FindFieldByName(name);
}

bool Fits(const pb::Descriptor* d, const WktField* fields, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!FieldFits(FindShapeField(d, i, fields[i].name), fields[i])) return false;
  }
  return true;
}

// HasShape reports whether d, whose name is already known to be a
// well-known type's, can be handled as that type. One that cannot is an
// ordinary message (draft -01 §wkt-shapes): bind-time validation refuses it,
// and this is what holds on the paths that do not validate, Marshal and
// skip_validate among them.
//
// This is not memoized, unlike protowire-go. A descriptor can die with its
// pool, so a cache keyed by address could answer for a new descriptor at the
// same address, and that answer decides whether a field is dereferenced.
template <size_t N>
bool HasShape(const pb::Descriptor* d, const WktField (&fields)[N]) {
  return Fits(d, fields, N);
}

std::string ShapeProblem(const pb::Descriptor* d, const WktShape& shape) {
  for (size_t i = 0; i < shape.count; ++i) {
    const WktField& f = shape.fields[i];
    const pb::FieldDescriptor* fd = d->FindFieldByName(f.name);
    if (FieldFits(fd, f)) continue;
    const std::string field = std::string("\"") + f.name + "\"";
    if (fd == nullptr) return "it has no field " + field;
    if (fd->type() != f.type) {
      return "field " + field + " is " + std::string(fd->type_name()) + ", and must be " +
             std::string(pb::FieldDescriptor::TypeName(f.type));
    }
    return "field " + field + " has the wrong cardinality";
  }
  return "";
}

}  // namespace

std::string WktShapeProblem(const pb::Descriptor* d) {
  const std::string_view name = d->full_name();
  if (name.rfind("google.protobuf.", 0) != 0 && name.rfind("pxf.", 0) != 0) return "";
  for (const WktShape& shape : kShapes) {
    if (shape.name == name) return ShapeProblem(d, shape);
  }
  for (const WktShape& shape : kWrappers) {
    if (shape.name == name) return ShapeProblem(d, shape);
  }
  return "";
}

bool IsTimestamp(const pb::Descriptor* d) {
  return d && d->full_name() == "google.protobuf.Timestamp" && HasShape(d, kSeconds);
}

bool IsDuration(const pb::Descriptor* d) {
  return d && d->full_name() == "google.protobuf.Duration" && HasShape(d, kSeconds);
}

bool IsAny(const pb::Descriptor* d) {
  return d && d->full_name() == "google.protobuf.Any" && HasShape(d, kAny);
}

int WrapperInnerCppType(const pb::Descriptor* d) {
  if (!d) return -1;
  const std::string_view name = d->full_name();
  for (const WktShape& shape : kWrappers) {
    if (shape.name != name) continue;
    if (!Fits(d, shape.fields, shape.count)) return -1;
    return pb::FieldDescriptor::TypeToCppType(shape.fields[0].type);
  }
  return -1;
}

void SetTimestampFields(pb::Message* msg, int64_t seconds, int32_t nanos) {
  const pb::Descriptor* d = msg->GetDescriptor();
  const pb::Reflection* r = msg->GetReflection();
  r->SetInt64(msg, d->FindFieldByName("seconds"), seconds);
  r->SetInt32(msg, d->FindFieldByName("nanos"), nanos);
}

void SetDurationFields(pb::Message* msg, int64_t seconds, int32_t nanos) {
  const pb::Descriptor* d = msg->GetDescriptor();
  const pb::Reflection* r = msg->GetReflection();
  r->SetInt64(msg, d->FindFieldByName("seconds"), seconds);
  r->SetInt32(msg, d->FindFieldByName("nanos"), nanos);
}

void ReadTimestampFields(const pb::Message& msg, int64_t* seconds, int32_t* nanos) {
  const pb::Descriptor* d = msg.GetDescriptor();
  const pb::Reflection* r = msg.GetReflection();
  *seconds = r->GetInt64(msg, d->FindFieldByName("seconds"));
  *nanos = r->GetInt32(msg, d->FindFieldByName("nanos"));
}

void ReadDurationFields(const pb::Message& msg, int64_t* seconds, int32_t* nanos) {
  const pb::Descriptor* d = msg.GetDescriptor();
  const pb::Reflection* r = msg.GetReflection();
  *seconds = r->GetInt64(msg, d->FindFieldByName("seconds"));
  *nanos = r->GetInt32(msg, d->FindFieldByName("nanos"));
}

bool IsBigInt(const pb::Descriptor* d) {
  return d && d->full_name() == "pxf.BigInt" && HasShape(d, kBigInt);
}
bool IsDecimal(const pb::Descriptor* d) {
  return d && d->full_name() == "pxf.Decimal" && HasShape(d, kDecimal);
}
bool IsBigFloat(const pb::Descriptor* d) {
  return d && d->full_name() == "pxf.BigFloat" && HasShape(d, kBigFloat);
}

bool IsSecret(const pb::Descriptor* d) {
  return d && d->full_name() == "pxf.Secret" && HasShape(d, kSecret);
}

bool IsDefaultable(const pb::Descriptor* d) {
  return IsTimestamp(d) || IsDuration(d) || WrapperInnerCppType(d) != -1 || IsBigInt(d) ||
         IsDecimal(d) || IsBigFloat(d);
}

const pb::FieldDescriptor* FindNullMaskField(const pb::Descriptor* d) {
  if (!d) return nullptr;
  const pb::FieldDescriptor* fd = d->FindFieldByName("_null");
  if (!fd) return nullptr;
  if (fd->cpp_type() != pb::FieldDescriptor::CPPTYPE_MESSAGE) return nullptr;
  const pb::Descriptor* mask = fd->message_type();
  if (mask->full_name() != "google.protobuf.FieldMask" || !HasShape(mask, kFieldMask)) {
    return nullptr;
  }
  return fd;
}

void AppendNullPath(pb::Message* root,
                    const pb::FieldDescriptor* null_mask_fd,
                    std::string_view path) {
  pb::Message* fm = root->GetReflection()->MutableMessage(root, null_mask_fd);
  const pb::FieldDescriptor* paths_fd = fm->GetDescriptor()->FindFieldByName("paths");
  fm->GetReflection()->AddString(fm, paths_fd, std::string(path));
}

// --- pxf.BigInt / Decimal / BigFloat sugar -------------------------------

namespace {

void SetBytes(pb::Message* msg, const char* field, const std::vector<uint8_t>& v) {
  if (v.empty()) return;
  const pb::FieldDescriptor* fd = msg->GetDescriptor()->FindFieldByName(field);
  msg->GetReflection()->SetString(msg, fd, std::string(v.begin(), v.end()));
}

void SetBoolField(pb::Message* msg, const char* field, bool v) {
  if (!v) return;
  const pb::FieldDescriptor* fd = msg->GetDescriptor()->FindFieldByName(field);
  msg->GetReflection()->SetBool(msg, fd, true);
}

std::vector<uint8_t> GetBytes(const pb::Message& msg, const char* field) {
  const pb::FieldDescriptor* fd = msg.GetDescriptor()->FindFieldByName(field);
  std::string scratch;
  const std::string& s = msg.GetReflection()->GetStringReference(msg, fd, &scratch);
  return std::vector<uint8_t>(s.begin(), s.end());
}

bool GetBoolField(const pb::Message& msg, const char* field) {
  const pb::FieldDescriptor* fd = msg.GetDescriptor()->FindFieldByName(field);
  return msg.GetReflection()->GetBool(msg, fd);
}

int32_t GetInt32Field(const pb::Message& msg, const char* field) {
  const pb::FieldDescriptor* fd = msg.GetDescriptor()->FindFieldByName(field);
  return msg.GetReflection()->GetInt32(msg, fd);
}

uint32_t GetUInt32Field(const pb::Message& msg, const char* field) {
  const pb::FieldDescriptor* fd = msg.GetDescriptor()->FindFieldByName(field);
  return msg.GetReflection()->GetUInt32(msg, fd);
}

}  // namespace

bool SetBigIntFromString(pb::Message* msg, std::string_view literal) {
  protowire::pb::BigInt bi;
  if (!protowire::pb::ParseBigInt(std::string(literal), bi)) return false;
  SetBytes(msg, "abs", bi.abs);
  SetBoolField(msg, "negative", bi.negative);
  return true;
}

bool SetDecimalFromString(pb::Message* msg, std::string_view literal) {
  protowire::pb::Decimal d;
  if (!protowire::pb::ParseDecimal(std::string(literal), d)) return false;
  SetBytes(msg, "unscaled", d.unscaled);
  if (d.scale != 0) {
    const pb::FieldDescriptor* fd = msg->GetDescriptor()->FindFieldByName("scale");
    msg->GetReflection()->SetInt32(msg, fd, d.scale);
  }
  SetBoolField(msg, "negative", d.negative);
  return true;
}

bool SetBigFloatFromString(pb::Message* msg, std::string_view literal) {
  const size_t p = literal.find('p');
  if (p != std::string_view::npos && p + 1 == literal.size()) return false;
  const std::string_view number = literal.substr(0, p);
  const std::string_view prec =
      p == std::string_view::npos ? std::string_view() : literal.substr(p + 1);
  return SetBigFloatFromLiteral(msg, number, prec, kMaxNumericLiteralDigits, kMaxBigFloatExponent)
      .ok();
}

Status SetBigFloatFromLiteral(pb::Message* msg,
                              std::string_view literal,
                              std::string_view prec_digits,
                              int max_digits,
                              int max_exponent) {
  BigFloatFields f;
  if (Status s = ParseBigFloatLiteral(literal, prec_digits, max_digits, max_exponent, &f);
      !s.ok()) {
    return s;
  }
  SetBytes(msg, "mantissa", f.mantissa);
  const pb::Descriptor* d = msg->GetDescriptor();
  const pb::Reflection* r = msg->GetReflection();
  if (f.exponent != 0) {
    r->SetInt32(msg, d->FindFieldByName("exponent"), static_cast<int32_t>(f.exponent));
  }
  if (f.prec != 0) r->SetUInt32(msg, d->FindFieldByName("prec"), f.prec);
  SetBoolField(msg, "negative", f.negative);
  return Status::OK();
}

StatusOr<std::string> ReadBigIntAsString(const pb::Message& msg) {
  protowire::pb::BigInt bi;
  bi.abs = GetBytes(msg, "abs");
  bi.negative = GetBoolField(msg, "negative");
  return protowire::pb::FormatBigInt(bi);
}

StatusOr<std::string> ReadDecimalAsString(const pb::Message& msg) {
  protowire::pb::Decimal d;
  d.unscaled = GetBytes(msg, "unscaled");
  d.scale = GetInt32Field(msg, "scale");
  d.negative = GetBoolField(msg, "negative");
  return protowire::pb::FormatDecimal(d);
}

StatusOr<std::string> ReadBigFloatAsString(const pb::Message& msg) {
  BigFloatFields f;
  f.mantissa = GetBytes(msg, "mantissa");
  f.exponent = GetInt32Field(msg, "exponent");
  f.prec = GetUInt32Field(msg, "prec");
  f.negative = GetBoolField(msg, "negative");
  return FormatBigFloatLiteral(f, kMaxNumericLiteralDigits, kMaxBigFloatExponent);
}

std::vector<std::string> ReadNullPaths(const pb::Message& root,
                                       const pb::FieldDescriptor* null_mask_fd) {
  std::vector<std::string> out;
  if (!root.GetReflection()->HasField(root, null_mask_fd)) return out;
  const pb::Message& fm = root.GetReflection()->GetMessage(root, null_mask_fd);
  const pb::FieldDescriptor* paths_fd = fm.GetDescriptor()->FindFieldByName("paths");
  int n = fm.GetReflection()->FieldSize(fm, paths_fd);
  out.reserve(n);
  for (int i = 0; i < n; ++i) {
    std::string scratch;
    out.push_back(
        std::string(fm.GetReflection()->GetRepeatedStringReference(fm, paths_fd, i, &scratch)));
  }
  return out;
}

}  // namespace protowire::pxf

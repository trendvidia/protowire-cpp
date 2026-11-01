// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/wellknown.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "bigfloat.h"
#include "protowire/limits.h"
#include "protowire/pb_big.h"

namespace protowire::pxf {

namespace pb = google::protobuf;

bool IsTimestamp(const pb::Descriptor* d) {
  return d && d->full_name() == "google.protobuf.Timestamp";
}

bool IsDuration(const pb::Descriptor* d) {
  return d && d->full_name() == "google.protobuf.Duration";
}

bool IsAny(const pb::Descriptor* d) {
  return d && d->full_name() == "google.protobuf.Any";
}

int WrapperInnerCppType(const pb::Descriptor* d) {
  if (!d) return -1;
  const auto& name = d->full_name();
  if (name == "google.protobuf.BoolValue") return pb::FieldDescriptor::CPPTYPE_BOOL;
  if (name == "google.protobuf.BytesValue") return pb::FieldDescriptor::CPPTYPE_STRING;
  if (name == "google.protobuf.DoubleValue") return pb::FieldDescriptor::CPPTYPE_DOUBLE;
  if (name == "google.protobuf.FloatValue") return pb::FieldDescriptor::CPPTYPE_FLOAT;
  if (name == "google.protobuf.Int32Value") return pb::FieldDescriptor::CPPTYPE_INT32;
  if (name == "google.protobuf.Int64Value") return pb::FieldDescriptor::CPPTYPE_INT64;
  if (name == "google.protobuf.StringValue") return pb::FieldDescriptor::CPPTYPE_STRING;
  if (name == "google.protobuf.UInt32Value") return pb::FieldDescriptor::CPPTYPE_UINT32;
  if (name == "google.protobuf.UInt64Value") return pb::FieldDescriptor::CPPTYPE_UINT64;
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
  return d && d->full_name() == "pxf.BigInt";
}
bool IsDecimal(const pb::Descriptor* d) {
  return d && d->full_name() == "pxf.Decimal";
}
bool IsBigFloat(const pb::Descriptor* d) {
  return d && d->full_name() == "pxf.BigFloat";
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
  if (fd->message_type()->full_name() != "google.protobuf.FieldMask") {
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

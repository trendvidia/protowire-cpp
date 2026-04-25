#include "protowire/pxf/wellknown.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

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

void ReadTimestampFields(const pb::Message& msg, int64_t* seconds,
                         int32_t* nanos) {
  const pb::Descriptor* d = msg.GetDescriptor();
  const pb::Reflection* r = msg.GetReflection();
  *seconds = r->GetInt64(msg, d->FindFieldByName("seconds"));
  *nanos = r->GetInt32(msg, d->FindFieldByName("nanos"));
}

void ReadDurationFields(const pb::Message& msg, int64_t* seconds,
                        int32_t* nanos) {
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

void AppendNullPath(pb::Message* root, const pb::FieldDescriptor* null_mask_fd,
                    std::string_view path) {
  pb::Message* fm = root->GetReflection()->MutableMessage(root, null_mask_fd);
  const pb::FieldDescriptor* paths_fd =
      fm->GetDescriptor()->FindFieldByName("paths");
  fm->GetReflection()->AddString(fm, paths_fd, std::string(path));
}

// --- pxf.BigInt / Decimal / BigFloat sugar -------------------------------

namespace {

void SetBytes(pb::Message* msg, const char* field,
              const std::vector<uint8_t>& v) {
  if (v.empty()) return;
  const pb::FieldDescriptor* fd = msg->GetDescriptor()->FindFieldByName(field);
  msg->GetReflection()->SetString(
      msg, fd, std::string(v.begin(), v.end()));
}

void SetBoolField(pb::Message* msg, const char* field, bool v) {
  if (!v) return;
  const pb::FieldDescriptor* fd = msg->GetDescriptor()->FindFieldByName(field);
  msg->GetReflection()->SetBool(msg, fd, true);
}

std::vector<uint8_t> GetBytes(const pb::Message& msg, const char* field) {
  const pb::FieldDescriptor* fd = msg.GetDescriptor()->FindFieldByName(field);
  std::string scratch;
  const std::string& s =
      msg.GetReflection()->GetStringReference(msg, fd, &scratch);
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
    const pb::FieldDescriptor* fd =
        msg->GetDescriptor()->FindFieldByName("scale");
    msg->GetReflection()->SetInt32(msg, fd, d.scale);
  }
  SetBoolField(msg, "negative", d.negative);
  return true;
}

bool SetBigFloatFromString(pb::Message* msg, std::string_view literal) {
  // Parse via strtod into a double, then extract the IEEE 754 mantissa and
  // exponent. Precision is fixed at 53 bits — covers everyday floats
  // losslessly while avoiding a math-library dependency. The wire format
  // remains the open-ended BigFloat schema.
  std::string buf(literal);
  char* end = nullptr;
  double d = std::strtod(buf.c_str(), &end);
  if (end == buf.c_str() || static_cast<size_t>(end - buf.c_str()) != buf.size()) {
    return false;
  }
  if (d == 0.0) {
    // BigFloat zero: prec stays at default (0 means "nothing emitted").
    return true;
  }
  bool negative = d < 0;
  if (negative) d = -d;
  // Decompose into mantissa (in [0.5, 1.0)) and exponent.
  int exp = 0;
  double mant = std::frexp(d, &exp);  // d = mant * 2^exp
  // Scale to integer: mantInt = mant * 2^53 (53-bit mantissa).
  uint64_t mant_int = static_cast<uint64_t>(std::ldexp(mant, 53));
  // Strip trailing zero bits to keep the magnitude compact.
  while (mant_int != 0 && (mant_int & 1) == 0) {
    mant_int >>= 1;
    ++exp;
  }
  // Adjusted exponent: in our schema, value = mantInt × 2^(exp - prec).
  int32_t adj_exp = static_cast<int32_t>(exp) - 53;

  // Encode mantInt as big-endian byte magnitude.
  std::vector<uint8_t> mant_bytes;
  for (int i = 7; i >= 0; --i) {
    uint8_t b = static_cast<uint8_t>((mant_int >> (i * 8)) & 0xff);
    if (!mant_bytes.empty() || b != 0) mant_bytes.push_back(b);
  }
  SetBytes(msg, "mantissa", mant_bytes);
  if (adj_exp != 0) {
    const pb::FieldDescriptor* fd =
        msg->GetDescriptor()->FindFieldByName("exponent");
    msg->GetReflection()->SetInt32(msg, fd, adj_exp);
  }
  // Always emit prec since 0 is "no value".
  const pb::FieldDescriptor* prec_fd =
      msg->GetDescriptor()->FindFieldByName("prec");
  msg->GetReflection()->SetUInt32(msg, prec_fd, 53);
  SetBoolField(msg, "negative", negative);
  return true;
}

std::string ReadBigIntAsString(const pb::Message& msg) {
  protowire::pb::BigInt bi;
  bi.abs = GetBytes(msg, "abs");
  bi.negative = GetBoolField(msg, "negative");
  return protowire::pb::FormatBigInt(bi);
}

std::string ReadDecimalAsString(const pb::Message& msg) {
  protowire::pb::Decimal d;
  d.unscaled = GetBytes(msg, "unscaled");
  d.scale = GetInt32Field(msg, "scale");
  d.negative = GetBoolField(msg, "negative");
  return protowire::pb::FormatDecimal(d);
}

std::string ReadBigFloatAsString(const pb::Message& msg) {
  std::vector<uint8_t> mant = GetBytes(msg, "mantissa");
  int32_t exp = GetInt32Field(msg, "exponent");
  uint32_t prec = GetUInt32Field(msg, "prec");
  bool neg = GetBoolField(msg, "negative");
  if (prec == 0) return "0";
  // Reconstruct as a double with the limited precision we wrote.
  uint64_t mant_int = 0;
  for (uint8_t b : mant) mant_int = (mant_int << 8) | b;
  double d = std::ldexp(static_cast<double>(mant_int), exp);
  if (neg) d = -d;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  return std::string(buf);
}

std::vector<std::string> ReadNullPaths(const pb::Message& root,
                                       const pb::FieldDescriptor* null_mask_fd) {
  std::vector<std::string> out;
  if (!root.GetReflection()->HasField(root, null_mask_fd)) return out;
  const pb::Message& fm =
      root.GetReflection()->GetMessage(root, null_mask_fd);
  const pb::FieldDescriptor* paths_fd =
      fm.GetDescriptor()->FindFieldByName("paths");
  int n = fm.GetReflection()->FieldSize(fm, paths_fd);
  out.reserve(n);
  for (int i = 0; i < n; ++i) {
    std::string scratch;
    out.push_back(std::string(fm.GetReflection()->GetRepeatedStringReference(
        fm, paths_fd, i, &scratch)));
  }
  return out;
}

}  // namespace protowire::pxf

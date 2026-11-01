// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "carrier.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/unknown_field_set.h>

#include "bigfloat.h"
#include "protowire/detail/base64.h"
#include "protowire/limits.h"
#include "protowire/pb_big.h"
#include "protowire/pxf/wellknown.h"
#include "pxf/annotations.pb.h"

namespace protowire::pxf {

namespace {

namespace pb = google::protobuf;

// Shared by all eight kind-specific carriers.
constexpr int kExtAnnotations = 1327;

constexpr std::string_view kAnnotationRequired = "protowire.schema.v1.required";
constexpr std::string_view kAnnotationDefault = "protowire.schema.v1.default";
constexpr std::string_view kDefaultParamName = "value";

// Field numbers from protowire/proto/schema/v1/descriptor.proto.
enum : uint8_t {
  kListEntries = 1,     // AnnotationList.entries
  kAnnotationName = 1,  // Annotation.name
  kAnnotationArgs = 2,  // Annotation.args
  kArgName = 1,         // AnnotationArg.name
  kArgString = 10,      // string_value
  kArgInt = 11,         // int_value
  kArgDouble = 12,      // double_value
  kArgBool = 13,        // bool_value
  kArgBytes = 14,       // bytes_value
  kArgLiteral = 15,     // literal
  kArgBigInt = 16,      // big_int_value
  kArgDecimal = 17,     // decimal_value
  kArgBigFloat = 18,    // big_float_value
  kLiteralEnum = 1,     // Literal.enum_value
  kEnumValueName = 2,   // EnumLiteral.value_name
};

enum WireType : uint8_t { kVarint = 0, kFixed64 = 1, kBytes = 2, kFixed32 = 5 };

// WireField is one field of a serialized message.
struct WireField {
  uint32_t number = 0;
  uint32_t type = 0;
  uint64_t u = 0;      // varint, fixed32 or fixed64 payload
  std::string_view v;  // length-delimited payload
};

bool ReadVarint(std::string_view* b, uint64_t* out) {
  uint64_t v = 0;
  for (int i = 0; i < 10 && i < static_cast<int>(b->size()); ++i) {
    const uint8_t c = static_cast<uint8_t>((*b)[i]);
    v |= uint64_t{static_cast<uint8_t>(c & 0x7f)} << (7 * i);
    if ((c & 0x80) == 0) {
      b->remove_prefix(static_cast<size_t>(i) + 1);
      *out = v;
      return true;
    }
  }
  return false;
}

// NextField consumes one field. Group wire types are not produced by the
// lowering pass and are treated as malformed.
bool NextField(std::string_view* b, WireField* f) {
  uint64_t tag = 0;
  if (!ReadVarint(b, &tag)) return false;
  f->number = static_cast<uint32_t>(tag >> 3);
  f->type = static_cast<uint32_t>(tag & 7);
  f->u = 0;
  f->v = {};
  if (f->number == 0) return false;
  switch (f->type) {
    case kVarint:
      return ReadVarint(b, &f->u);
    case kFixed64:
    case kFixed32: {
      const size_t n = f->type == kFixed64 ? 8 : 4;
      if (b->size() < n) return false;
      for (size_t i = 0; i < n; ++i) {
        f->u |= uint64_t{static_cast<uint8_t>((*b)[i])} << (8 * i);
      }
      b->remove_prefix(n);
      return true;
    }
    case kBytes: {
      uint64_t n = 0;
      if (!ReadVarint(b, &n) || n > b->size()) return false;
      f->v = b->substr(0, n);
      b->remove_prefix(n);
      return true;
    }
    default:
      return false;
  }
}

bool IsUnsignedTarget(const pb::FieldDescriptor* fd) {
  switch (fd->cpp_type()) {
    case pb::FieldDescriptor::CPPTYPE_UINT32:
    case pb::FieldDescriptor::CPPTYPE_UINT64:
      return true;
    case pb::FieldDescriptor::CPPTYPE_MESSAGE: {
      const int inner = WrapperInnerCppType(fd->message_type());
      return inner == pb::FieldDescriptor::CPPTYPE_UINT32 ||
             inner == pb::FieldDescriptor::CPPTYPE_UINT64;
    }
    default:
      return false;
  }
}

// ShortestDouble returns the fewest significant digits that read back as v,
// in the given printf form: "%.*e" or "%.*g".
std::string ShortestDouble(double v, const char* form) {
  char buf[64];
  for (int p = 0; p <= 17; ++p) {
    std::snprintf(buf, sizeof(buf), form, p, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  return buf;
}

// FormatCarrierDouble reduces a double_value to the literal the bracket form
// would have carried. A pxf.BigInt or pxf.Decimal literal is positional, so
// the shortest digits are expanded: @default(1e100) is 10^100, what the
// author wrote, and not the double nearest it. Otherwise this is the form a
// bracket default is written in, so the same value does not read as a
// disagreement between the two spellings.
std::string FormatCarrierDouble(double v, const pb::FieldDescriptor* fd) {
  const bool positional = fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE &&
                          (IsBigInt(fd->message_type()) || IsDecimal(fd->message_type()));
  if (!positional) return ShortestDouble(v, "%.*g");
  std::string e = ShortestDouble(v, "%.*e");
  const size_t mark = e.find('e');
  if (mark == std::string::npos) return e;  // inf or nan: the binder refuses it
  std::string digits = e.substr(0, mark);
  const bool neg = !digits.empty() && digits[0] == '-';
  if (neg) digits.erase(0, 1);
  if (digits.size() > 1) digits.erase(1, 1);  // the point
  const long exp = std::strtol(e.c_str() + mark + 1, nullptr, 10);
  std::string out = neg ? "-" : "";
  if (exp < 0) {
    out += "0.";
    out.append(static_cast<size_t>(-exp - 1), '0');
    out += digits;
  } else if (static_cast<size_t>(exp) + 1 >= digits.size()) {
    out += digits;
    out.append(static_cast<size_t>(exp) + 1 - digits.size(), '0');
  } else {
    out += digits.substr(0, static_cast<size_t>(exp) + 1);
    out.push_back('.');
    out += digits.substr(static_cast<size_t>(exp) + 1);
  }
  return out;
}

std::vector<uint8_t> ToBytes(std::string_view v) {
  return std::vector<uint8_t>(v.begin(), v.end());
}

// The big-number literals return "" and set *problem when they cannot be
// reduced. Each message's shape is in proto/pxf/bignum.proto.

std::string BigIntLiteral(std::string_view b, std::string* problem) {
  protowire::pb::BigInt v;
  WireField f;
  while (!b.empty()) {
    if (!NextField(&b, &f)) {
      *problem = "@default's big_int_value is not well-formed";
      return "";
    }
    if (f.number == 1) v.abs = ToBytes(f.v);
    if (f.number == 2) v.negative = f.u != 0;
  }
  auto s = protowire::pb::FormatBigInt(v);
  if (!s.ok()) {
    *problem = "@default's big_int_value: " + s.status().message();
    return "";
  }
  return *s;
}

std::string DecimalLiteral(std::string_view b, std::string* problem) {
  protowire::pb::Decimal v;
  WireField f;
  while (!b.empty()) {
    if (!NextField(&b, &f)) {
      *problem = "@default's decimal_value is not well-formed";
      return "";
    }
    if (f.number == 1) v.unscaled = ToBytes(f.v);
    // `int32 scale = 2`: a plain varint, not zigzag.
    if (f.number == 2 && f.type == kVarint) v.scale = static_cast<int32_t>(f.u);
    if (f.number == 3) v.negative = f.u != 0;
  }
  auto s = protowire::pb::FormatDecimal(v);
  if (!s.ok()) {
    *problem = "@default's decimal_value: " + s.status().message();
    return "";
  }
  return *s;
}

std::string BigFloatLiteral(std::string_view b, std::string* problem) {
  BigFloatFields v;
  WireField f;
  while (!b.empty()) {
    if (!NextField(&b, &f)) {
      *problem = "@default's big_float_value is not well-formed";
      return "";
    }
    if (f.number == 1) v.mantissa = ToBytes(f.v);
    if (f.number == 2 && f.type == kVarint) v.exponent = static_cast<int32_t>(f.u);
    if (f.number == 3) v.prec = static_cast<uint32_t>(f.u);
    if (f.number == 4) v.negative = f.u != 0;
  }
  if (v.prec == 0) {
    *problem = "@default's big_float_value declares no precision";
    return "";
  }
  auto s = FormatBigFloatLiteral(v, kMaxNumericLiteralDigits, kMaxBigFloatExponent);
  if (!s.ok()) {
    *problem = "@default's big_float_value: " + s.status().message();
    return "";
  }
  return *s;
}

// LiteralValue reduces a Literal. Only the resolved enum reference has one:
// a message or list literal denotes what a single literal cannot.
std::string LiteralValue(std::string_view b, std::string* problem) {
  WireField f;
  while (!b.empty()) {
    if (!NextField(&b, &f)) {
      *problem = "@default's value is not well-formed";
      return "";
    }
    if (f.number != kLiteralEnum || f.type != kBytes) continue;
    std::string_view e = f.v;
    WireField g;
    while (!e.empty() && NextField(&e, &g)) {
      if (g.number == kEnumValueName && g.type == kBytes && !g.v.empty()) {
        return std::string(g.v);
      }
    }
    *problem = "@default names an enum value the lowering pass left unresolved";
    return "";
  }
  *problem = "@default carries a message or list literal, and a default is exactly one literal";
  return "";
}

// ArgLiteral reduces one AnnotationArg to the PXF literal the bracket form
// would have carried. AnnotationArg has no unsigned variant: the lowering
// pass reinterprets a large unsigned literal through int64, so fd decides
// how int_value reads.
std::string ArgLiteral(std::string_view b, const pb::FieldDescriptor* fd, std::string* problem) {
  WireField f;
  while (!b.empty()) {
    if (!NextField(&b, &f)) {
      *problem = "@default's value is not well-formed";
      return "";
    }
    switch (f.number) {
      case kArgString:
        if (f.type == kBytes) return std::string(f.v);
        break;
      case kArgInt:
        if (f.type == kVarint) {
          return IsUnsignedTarget(fd) ? std::to_string(f.u)
                                      : std::to_string(static_cast<int64_t>(f.u));
        }
        break;
      case kArgDouble:
        if (f.type == kFixed64) {
          double d = 0;
          std::memcpy(&d, &f.u, sizeof(d));
          return FormatCarrierDouble(d, fd);
        }
        break;
      case kArgBool:
        if (f.type == kVarint) return f.u != 0 ? "true" : "false";
        break;
      case kArgBytes:
        // The bracket form spells a bytes default as bare base64.
        if (f.type == kBytes) return detail::Base64EncodeStd(f.v);
        break;
      case kArgBigInt:
        if (f.type == kBytes) return BigIntLiteral(f.v, problem);
        break;
      case kArgDecimal:
        if (f.type == kBytes) return DecimalLiteral(f.v, problem);
        break;
      case kArgBigFloat:
        if (f.type == kBytes) return BigFloatLiteral(f.v, problem);
        break;
      case kArgLiteral:
        if (f.type == kBytes) return LiteralValue(f.v, problem);
        break;
      default:
        break;
    }
  }
  *problem = "@default's value is empty";
  return "";
}

std::string_view ArgName(std::string_view b) {
  WireField f;
  while (!b.empty() && NextField(&b, &f)) {
    if (f.number == kArgName && f.type == kBytes) return f.v;
  }
  return {};
}

struct CarrierOptions {
  bool required = false;
  std::optional<std::string> def;
  std::string problem;
};

// ApplyAnnotation folds one Annotation entry into got. A truncated entry
// contributes whatever it did name: dropping a @required because a later
// byte was garbled is silent non-enforcement.
void ApplyAnnotation(std::string_view b, const pb::FieldDescriptor* fd, CarrierOptions* got) {
  std::string_view name;
  std::vector<std::string_view> args;
  WireField f;
  while (!b.empty() && NextField(&b, &f)) {
    if (f.type != kBytes) continue;
    if (f.number == kAnnotationName) name = f.v;
    if (f.number == kAnnotationArgs) args.push_back(f.v);
  }
  if (name == kAnnotationRequired) {
    got->required = true;
    return;
  }
  if (name != kAnnotationDefault) return;
  if (got->def.has_value() || !got->problem.empty()) return;  // the first wins
  for (std::string_view arg : args) {
    const std::string_view n = ArgName(arg);
    if (!n.empty() && n != kDefaultParamName) continue;
    std::string problem;
    std::string lit = ArgLiteral(arg, fd, &problem);
    if (!problem.empty()) {
      got->problem = std::move(problem);
    } else {
      got->def = std::move(lit);
    }
    return;
  }
  got->problem = "@default carries no value argument";
}

CarrierOptions ParseAnnotationList(std::string_view b, const pb::FieldDescriptor* fd) {
  CarrierOptions got;
  WireField f;
  while (!b.empty() && NextField(&b, &f)) {
    if (f.number == kListEntries && f.type == kBytes) ApplyAnnotation(f.v, fd, &got);
  }
  return got;
}

// CarrierBytes collects the 1327 payload. It arrives as unknown bytes unless
// the process linked a generated schema package that registers the
// extension; then it is a known field and is re-serialized for the walk.
std::string CarrierBytes(const pb::FieldOptions& opts) {
  std::string out;
  const pb::Reflection* r = opts.GetReflection();
  const pb::UnknownFieldSet& unknown = r->GetUnknownFields(opts);
  for (int i = 0; i < unknown.field_count(); ++i) {
    const pb::UnknownField& u = unknown.field(i);
    if (u.number() == kExtAnnotations && u.type() == pb::UnknownField::TYPE_LENGTH_DELIMITED) {
      out.append(std::string(u.length_delimited()));
    }
  }
  static const pb::FieldDescriptor* known = r->FindKnownExtensionByNumber(kExtAnnotations);
  if (known != nullptr && known->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE &&
      !known->is_repeated() && r->HasField(opts, known)) {
    out += r->GetMessage(opts, known).SerializeAsString();
  }
  return out;
}

}  // namespace

FieldAnnotations ReadFieldAnnotations(const google::protobuf::FieldDescriptor* fd) {
  FieldAnnotations a;
  if (fd == nullptr) return a;
  const pb::FieldOptions& opts = fd->options();
  a.required = opts.GetExtension(::pxf::required);
  if (opts.HasExtension(::pxf::default_)) a.def = opts.GetExtension(::pxf::default_);

  const std::string carrier = CarrierBytes(opts);
  if (carrier.empty()) return a;
  // The two surfaces are not reconciled (RFC-001 §8.5). required is one
  // boolean, so either spelling makes the field required. default carries a
  // value, so a disagreement is the schema's error: applying either would be
  // the reconciliation the draft forbids. Equal values are no disagreement.
  CarrierOptions c = ParseAnnotationList(carrier, fd);
  if (c.required) {
    a.required_from_carrier = !a.required;
    a.required = true;
  }
  if (!c.problem.empty()) {
    a.default_problem = std::move(c.problem);
    a.default_from_carrier = true;
  } else if (c.def.has_value()) {
    if (!a.def.has_value()) {
      a.def = std::move(c.def);
      a.default_from_carrier = true;
    } else if (*a.def != *c.def) {
      a.default_problem = "(pxf.default) = \"" + *a.def + "\" and @default = \"" + *c.def +
                          "\" carry different values, and the two surfaces must not be "
                          "reconciled (RFC-001 §8.5)";
    }
  }
  return a;
}

}  // namespace protowire::pxf

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// The range of an (sbe.encoding) primitive and of the field it narrows, for
// the checks of draft -01 § SBE Annotations: "Emitters MUST reject values
// that fall outside the narrowed type's range". Marshal refuses a value the
// primitive cannot hold, and Unmarshal one the field cannot hold, rather
// than keep its low bits (protowire-go#252).
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <google/protobuf/descriptor.h>

#include "protowire/sbe/template.h"

namespace protowire::sbe {

inline bool IsIntEncoding(std::string_view e) {
  return e == kEncInt8 || e == kEncInt16 || e == kEncInt32 || e == kEncInt64 || e == kEncUint8 ||
         e == kEncUint16 || e == kEncUint32 || e == kEncUint64;
}

// EncodingRange holds, for an integer primitive, its range and the field's,
// and whether a value can fall outside the other. For a float primitive, the
// checks are a double that float cannot hold.
struct EncodingRange {
  int64_t enc_lo = 0;
  uint64_t enc_hi = 0;
  int64_t field_lo = 0;
  uint64_t field_hi = 0;
  bool enc_signed = false;
  bool write_check = false;
  bool read_check = false;
};

// FieldRange is the range of values an integer, bool or enum field holds. An
// enum is open in proto3, so any int32.
inline void FieldRange(const google::protobuf::FieldDescriptor* fd, int64_t* lo, uint64_t* hi) {
  using FD = google::protobuf::FieldDescriptor;
  switch (fd->type()) {
    case FD::TYPE_INT32:
    case FD::TYPE_SINT32:
    case FD::TYPE_SFIXED32:
    case FD::TYPE_ENUM:
      *lo = std::numeric_limits<int32_t>::min();
      *hi = std::numeric_limits<int32_t>::max();
      return;
    case FD::TYPE_INT64:
    case FD::TYPE_SINT64:
    case FD::TYPE_SFIXED64:
      *lo = std::numeric_limits<int64_t>::min();
      *hi = std::numeric_limits<int64_t>::max();
      return;
    case FD::TYPE_UINT32:
    case FD::TYPE_FIXED32:
      *lo = 0;
      *hi = std::numeric_limits<uint32_t>::max();
      return;
    case FD::TYPE_BOOL:
      *lo = 0;
      *hi = 1;
      return;
    default:  // TYPE_UINT64, TYPE_FIXED64
      *lo = 0;
      *hi = std::numeric_limits<uint64_t>::max();
      return;
  }
}

inline EncodingRange RangeOf(const FieldTemplate& ft) {
  using FD = google::protobuf::FieldDescriptor;
  EncodingRange r;
  if (ft.encoding == kEncFloat) {
    r.write_check = ft.fd->type() == FD::TYPE_DOUBLE;
  } else if (ft.encoding == kEncDouble) {
    r.read_check = ft.fd->type() == FD::TYPE_FLOAT;
  } else if (IsIntEncoding(ft.encoding)) {
    r.enc_signed = ft.encoding[0] == 'i';
    const unsigned bits = 8U * ft.size;
    if (r.enc_signed) {
      r.enc_hi = (uint64_t{1} << (bits - 1)) - 1;
      r.enc_lo = -static_cast<int64_t>(r.enc_hi) - 1;
    } else {
      r.enc_lo = 0;
      r.enc_hi = std::numeric_limits<uint64_t>::max() >> (64 - bits);
    }
    FieldRange(ft.fd, &r.field_lo, &r.field_hi);
    r.write_check = r.field_lo < r.enc_lo || r.field_hi > r.enc_hi;
    // A bool reads any nonzero primitive as true.
    r.read_check =
        ft.fd->type() != FD::TYPE_BOOL && (r.enc_lo < r.field_lo || r.enc_hi > r.field_hi);
  }
  return r;
}

// InRange reports whether the integer v is within [lo, hi]. v holds a
// uint64 when is_unsigned is true, and an int64's bits otherwise.
inline bool InRange(uint64_t v, bool is_unsigned, int64_t lo, uint64_t hi) {
  if (!is_unsigned && static_cast<int64_t>(v) < 0) return static_cast<int64_t>(v) >= lo;
  return v <= hi;
}

// IntString formats an integer held as in InRange.
inline std::string IntString(uint64_t v, bool is_unsigned) {
  return is_unsigned ? std::to_string(v) : std::to_string(static_cast<int64_t>(v));
}

// FloatFits reports whether a double is within float's range. NaN and the
// infinities are, as float has them; a finite value past ±max float is not,
// even one that would round down to it.
inline bool FloatFits(double f) {
  constexpr double kMax = std::numeric_limits<float>::max();
  return std::isinf(f) || !(f > kMax || f < -kMax);
}

// GoG formats f as Go's %g does: the shortest digits that read back as f,
// in %e form when the exponent is below -4 or at least 6.
inline std::string GoG(double f) {
  if (std::isnan(f)) return "NaN";
  if (std::isinf(f)) return f > 0 ? "+Inf" : "-Inf";
  char buf[64];
  auto sci = std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::scientific);
  std::string s(buf, sci.ptr);
  const size_t e = s.find('e');
  const int exp = std::stoi(s.substr(e + 1));
  if (exp < -4 || exp >= 6) return s;
  auto fix = std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::fixed);
  return std::string(buf, fix.ptr);
}

}  // namespace protowire::sbe

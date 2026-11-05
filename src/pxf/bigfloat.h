// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// pxf.BigFloat's text form (draft -01 § PXF Annotations, protowire#406,
// #410): a binary floating-point value, mantissa × 2^exponent at prec bits,
// written as a decimal literal with a precision suffix. Both directions are
// exact rational arithmetic, because a double cannot hold the range or the
// precision the type promises.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "protowire/detail/status.h"

namespace protowire::pxf {

// BigFloatFields is the message's four fields.
struct BigFloatFields {
  std::vector<uint8_t> mantissa;  // big-endian; empty for zero
  int64_t exponent = 0;
  uint32_t prec = 0;
  bool negative = false;
};

// CountLiteralDigits is what MaxNumericLiteralDigits counts: the significant
// digits of a literal's mantissa, either side of the point, leading zeros
// excluded (draft -01 § Mandatory Limits, protowire#410). An exponent and a
// precision suffix are bounded on their own, so the count stops at either.
int CountLiteralDigits(std::string_view literal);

// ParseBigFloatLiteral reads a literal at the precision its suffix states,
// or at 256 bits without one, rounding half to even. prec_digits is the
// suffix's digits, empty for none. The mantissa is the significand as a
// prec-bit integer and the exponent the binary exponent minus prec; a zero
// has an empty mantissa and exponent 0.
Status ParseBigFloatLiteral(std::string_view literal,
                            std::string_view prec_digits,
                            int max_digits,
                            int max_exponent,
                            BigFloatFields* out);

// BigFloatLimit is the first limit a value with a non-zero prec fails, in
// the order protowire-go asks them, and the number it fails with. Each
// writer words the refusal in its own way, as protowire-go does.
struct BigFloatLimit {
  enum Kind : uint8_t {
    kNone,
    // The binary exponent as the fields store it is past MaxBigFloatExponent.
    kExponent,
    // The mantissa rounded to prec carried the binary exponent past it
    // (protowire#410, protowire-go#261).
    kRoundedExponent,
    // prec renders to more digits than MaxNumericLiteralDigits.
    kDigits,
  };
  Kind kind = kNone;
  int64_t value = 0;  // the binary exponent, or the digit count
};

// CheckBigFloatLimits asks the limits of a value whose prec is not zero.
// The exponent comes first, since it bounds the cost of everything after.
BigFloatLimit CheckBigFloatLimits(const BigFloatFields& f, int max_digits, int max_exponent);

// FormatBigFloatLiteral writes the value as N = (decimal digits of 2^prec)
// + 1 significant digits, round half even, in C's %g form, then `p<prec>`.
// A value past the bounds has no literal, and is refused before any work.
StatusOr<std::string> FormatBigFloatLiteral(const BigFloatFields& f,
                                            int max_digits,
                                            int max_exponent);

}  // namespace protowire::pxf

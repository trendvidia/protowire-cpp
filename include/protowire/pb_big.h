// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Relative, because clang-tidy runs on this header with flags it infers, and
// they do not always carry the include root.
#include "detail/status.h"

namespace protowire::pb {

// Byte-backed BigInt. Magnitude is stored as unsigned big-endian bytes; sign
// is carried separately. This mirrors the wire format of pxf.BigInt and means
// users without a math library can still round-trip values.
struct BigInt {
  std::vector<uint8_t> abs;  // big-endian magnitude; empty == zero
  bool negative = false;

  bool IsZero() const { return abs.empty(); }
};

// Byte-backed Decimal. value = (-1)^negative × unscaled × 10^(-scale).
struct Decimal {
  std::vector<uint8_t> unscaled;
  int32_t scale = 0;
  bool negative = false;

  bool IsZero() const { return unscaled.empty(); }
};

// Byte-backed BigFloat. value = (-1)^negative × mantissa × 2^exponent, rounded
// to prec bits, half to even. prec == 0 means zero, signed by negative.
//
// The fields hold the value, not one spelling of it. Unmarshal narrows a mantissa
// wider than prec, and Marshal writes the one form protowire-go writes: mantissa
// a prec-bit integer with its top bit set. So {25, -5, 64} marshals as a mantissa
// of eight bytes and exponent -64, and reads back with those fields, the same
// number.
struct BigFloat {
  std::vector<uint8_t> mantissa;
  int32_t exponent = 0;
  uint32_t prec = 0;
  bool negative = false;

  bool IsZero() const { return prec == 0; }
};

// String parsers (decimal text → big number). Return false on syntax error.
bool ParseBigInt(const std::string& s, BigInt& out);
bool ParseDecimal(const std::string& s, Decimal& out);

// String formatters (big number → decimal text). Exact, or an error: a value
// past MaxNumericLiteralDigits has no literal (HARDENING.md § Arbitrary-
// precision magnitudes). A Decimal's scale is bounded before any work, since
// it is four bytes whoever wrote the message chose.
StatusOr<std::string> FormatBigInt(const BigInt& v);
StatusOr<std::string> FormatDecimal(const Decimal& v);

}  // namespace protowire::pb

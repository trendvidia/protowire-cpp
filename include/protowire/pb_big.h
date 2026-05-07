// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

// Byte-backed BigFloat. value = (-1)^negative × mantissa × 2^exponent, where
// mantissa is interpreted as an integer in [0, 2^prec). prec == 0 means zero.
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

// String formatters (big number → decimal text). Always exact.
std::string FormatBigInt(const BigInt& v);
std::string FormatDecimal(const Decimal& v);

}  // namespace protowire::pb

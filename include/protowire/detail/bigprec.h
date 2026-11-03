// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// The bound on a pxf.BigFloat's precision, shared by pxf, which bounds a
// literal's precision suffix and the literals it writes, and pb, which bounds
// a prec read off the wire (draft -01 § Mandatory Limits, protowire#406 and
// #499). protowire-go keeps the same code in internal/bigprec.

#pragma once

#include <cstdint>

namespace protowire::detail {

// Pow2Digits returns the number of decimal digits of 2^p:
// floor(p × log10(2)) + 1, with log10(2) as a 64-bit fraction. 2^p is never
// computed, because p comes off the wire or out of a document. It agrees
// with protowire-go's 128-bit product for every uint32 p (checked
// exhaustively on protowire-cpp#108).
inline int64_t Pow2Digits(uint32_t p) {
  constexpr uint64_t kLog10Of2 = 0x4d104d427de7fbccULL;  // floor(log10(2) × 2^64)
  const uint64_t high = p * (kLog10Of2 >> 32);
  const uint64_t low = p * (kLog10Of2 & 0xffffffffULL);
  return static_cast<int64_t>((high + (low >> 32)) >> 32) + 1;
}

// BigFloatPrecDigits is N, the significant digits a BigFloat of prec bits is
// written with: the decimal digits of 2^prec, plus one, the fewest for which
// every prec-bit value is recovered from its literal (protowire#406).
// MaxNumericLiteralDigits bounds it, so prec 13603 is the last allowed at the
// default.
inline int64_t BigFloatPrecDigits(uint32_t prec) {
  return Pow2Digits(prec) + 1;
}

}  // namespace protowire::detail

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Portable overflow-checked signed-64-bit arithmetic. Used by the
// duration parser to detect accumulator overflow without resorting to
// __int128 (which isn't available on MSVC).
//
// Each helper returns `true` on overflow, leaving the output
// unspecified on overflow. On success, the result is written to
// `*out`.

#pragma once

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace protowire::detail {

// Signed 64×64 → 64-bit multiply with overflow detection.
//
// GCC/Clang: __builtin_mul_overflow handles signed overflow correctly
// per C2x semantics.
//
// MSVC: _mul128 produces the full 128-bit signed product as a
// (low, high) pair. Overflow happened iff `hi` isn't the sign
// extension of `lo` — i.e. when the upper 64 bits aren't all 0
// (positive result fits) or all 1 (negative result fits).
inline bool MulOverflow(int64_t a, int64_t b, int64_t* out) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  long long tmp;
  bool overflow =
      __builtin_mul_overflow(static_cast<long long>(a), static_cast<long long>(b), &tmp);
  *out = static_cast<int64_t>(tmp);
  return overflow;
#elif defined(_MSC_VER)
  int64_t hi;
  int64_t lo = _mul128(a, b, &hi);
  *out = lo;
  return hi != (lo < 0 ? -1 : 0);
#else
#error "MulOverflow: no implementation for this compiler"
#endif
}

// Signed 64+64 → 64-bit add with overflow detection.
//
// GCC/Clang: __builtin_add_overflow.
//
// MSVC: classic two's-complement overflow check — overflow happens
// when the operands have the same sign and the result has the
// opposite sign, expressible as `((a ^ r) & (b ^ r)) < 0`.
inline bool AddOverflow(int64_t a, int64_t b, int64_t* out) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  long long tmp;
  bool overflow =
      __builtin_add_overflow(static_cast<long long>(a), static_cast<long long>(b), &tmp);
  *out = static_cast<int64_t>(tmp);
  return overflow;
#elif defined(_MSC_VER)
  int64_t r = static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
  *out = r;
  return ((a ^ r) & (b ^ r)) < 0;
#else
#error "AddOverflow: no implementation for this compiler"
#endif
}

// Negate with overflow detection. INT64_MIN is the only signed-int64
// value whose negation overflows.
inline bool NegOverflow(int64_t a, int64_t* out) noexcept {
  if (a == INT64_MIN) {
    *out = a;
    return true;
  }
  *out = -a;
  return false;
}

}  // namespace protowire::detail

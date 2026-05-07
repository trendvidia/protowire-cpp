// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/detail/duration.h"

#include <cstdio>
#include <cstdlib>

#include "protowire/detail/checked_arith.h"

namespace protowire::detail {

namespace {

// Returns nanoseconds per unit, or -1 if not a known unit. Updates len with
// the number of bytes the unit occupies (1, 2, or 3 — "µs" is 2 bytes UTF-8).
int64_t UnitNanos(std::string_view s, size_t& len) {
  if (s.empty()) return -1;
  if (s.size() >= 2) {
    // µs is 0xC2 0xB5 in UTF-8.
    if (static_cast<unsigned char>(s[0]) == 0xC2 && static_cast<unsigned char>(s[1]) == 0xB5 &&
        s.size() >= 3 && s[2] == 's') {
      len = 3;
      return 1'000;
    }
    if (s[0] == 'n' && s[1] == 's') {
      len = 2;
      return 1;
    }
    if (s[0] == 'u' && s[1] == 's') {
      len = 2;
      return 1'000;
    }
    if (s[0] == 'm' && s[1] == 's') {
      len = 2;
      return 1'000'000;
    }
  }
  if (s[0] == 's') {
    len = 1;
    return 1'000'000'000LL;
  }
  if (s[0] == 'm') {
    len = 1;
    return 60LL * 1'000'000'000LL;
  }
  if (s[0] == 'h') {
    len = 1;
    return 3600LL * 1'000'000'000LL;
  }
  return -1;
}

}  // namespace

std::optional<Duration> ParseDuration(std::string_view s) {
  if (s.empty()) return std::nullopt;
  bool neg = false;
  if (s[0] == '-' || s[0] == '+') {
    neg = (s[0] == '-');
    s.remove_prefix(1);
  }
  if (s.empty()) return std::nullopt;
  if (s == "0") return Duration{0, 0};

  // Total nanoseconds accumulator. Every multiply / add goes through
  // overflow-checked int64 helpers (see protowire/detail/checked_arith.h)
  // so the intermediate doesn't need to be 128-bit — a portability win
  // because MSVC has no __int128.
  int64_t total_ns = 0;

  while (!s.empty()) {
    // integer part
    size_t i = 0;
    bool any_digit = false;
    int64_t whole = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      // Overflow-check the digit accumulation too, so a 100-digit
      // "whole" before the unit suffix doesn't silently wrap.
      int64_t mul10;
      if (MulOverflow(whole, 10, &mul10)) return std::nullopt;
      if (AddOverflow(mul10, static_cast<int64_t>(s[i] - '0'), &whole)) return std::nullopt;
      any_digit = true;
      ++i;
    }
    int64_t frac_num = 0;
    int64_t frac_div = 1;
    if (i < s.size() && s[i] == '.') {
      ++i;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (frac_div < 1'000'000'000LL) {
          frac_num = frac_num * 10 + (s[i] - '0');
          frac_div *= 10;
        }
        any_digit = true;
        ++i;
      }
    }
    if (!any_digit) return std::nullopt;
    s.remove_prefix(i);

    size_t unit_len = 0;
    int64_t unit_ns = UnitNanos(s, unit_len);
    if (unit_ns < 0) return std::nullopt;
    s.remove_prefix(unit_len);

    // v = whole * unit_ns  (check overflow)
    int64_t v;
    if (MulOverflow(whole, unit_ns, &v)) return std::nullopt;

    if (frac_num > 0) {
      // frac_part = (frac_num * unit_ns) / frac_div  (mul-check;
      // divide is safe — frac_div is always > 0).
      int64_t frac_mul;
      if (MulOverflow(frac_num, unit_ns, &frac_mul)) return std::nullopt;
      int64_t frac_part = frac_mul / frac_div;
      if (AddOverflow(v, frac_part, &v)) return std::nullopt;
    }

    if (AddOverflow(total_ns, v, &total_ns)) return std::nullopt;
  }

  if (neg) {
    int64_t neg_total;
    if (NegOverflow(total_ns, &neg_total)) return std::nullopt;
    total_ns = neg_total;
  }

  Duration d;
  d.seconds = total_ns / 1'000'000'000LL;
  d.nanos = static_cast<int32_t>(total_ns % 1'000'000'000LL);
  // Normalize so that nanos is in [0, 1e9).
  if (d.nanos < 0) {
    --d.seconds;
    d.nanos += 1'000'000'000;
  }
  return d;
}

std::string FormatDuration(int64_t seconds, int32_t nanos) {
  // Total nanoseconds; preserve sign. We compute in int64 and refuse to
  // format values outside its range — the same precondition libprotobuf's
  // Duration formatter has.
  int64_t scaled;
  if (MulOverflow(seconds, 1'000'000'000LL, &scaled) ||
      AddOverflow(scaled, static_cast<int64_t>(nanos), &scaled)) {
    return "<duration overflow>";
  }
  int64_t total_ns = scaled;
  if (total_ns == 0) return "0s";
  bool neg = total_ns < 0;
  if (neg) {
    int64_t neg_total;
    if (NegOverflow(total_ns, &neg_total)) return "<duration overflow>";
    total_ns = neg_total;
  }

  // If smaller than 1 second, format with smallest unit ns/us/ms.
  std::string out;
  if (total_ns < 1'000'000'000LL) {
    // Pick unit.
    if (total_ns < 1'000) {
      out = std::to_string(total_ns) + "ns";
    } else if (total_ns < 1'000'000) {
      // µs
      int64_t us = total_ns / 1'000;
      int64_t rem_ns = total_ns % 1'000;
      char buf[64];
      if (rem_ns == 0) {
        std::snprintf(buf, sizeof(buf), "%lldµs", static_cast<long long>(us));
      } else {
        // Trim trailing zeros from rem_ns formatted with width 3.
        char tmp[8];
        std::snprintf(tmp, sizeof(tmp), "%03lld", static_cast<long long>(rem_ns));
        int n = 3;
        while (n > 0 && tmp[n - 1] == '0') --n;
        tmp[n] = 0;
        std::snprintf(buf, sizeof(buf), "%lld.%sµs", static_cast<long long>(us), tmp);
      }
      out = buf;
    } else {
      int64_t ms = total_ns / 1'000'000;
      int64_t rem_us = total_ns % 1'000'000;
      char buf[64];
      if (rem_us == 0) {
        std::snprintf(buf, sizeof(buf), "%lldms", static_cast<long long>(ms));
      } else {
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "%06lld", static_cast<long long>(rem_us));
        int n = 6;
        while (n > 0 && tmp[n - 1] == '0') --n;
        tmp[n] = 0;
        std::snprintf(buf, sizeof(buf), "%lld.%sms", static_cast<long long>(ms), tmp);
      }
      out = buf;
    }
  } else {
    int64_t s = total_ns / 1'000'000'000LL;
    int64_t rem_ns = total_ns % 1'000'000'000LL;
    int64_t h = s / 3600;
    int64_t m = (s / 60) % 60;
    int64_t sec = s % 60;
    if (h > 0) out += std::to_string(h) + "h";
    if (h > 0 || m > 0) out += std::to_string(m) + "m";
    // seconds (with fraction)
    if (rem_ns == 0) {
      out += std::to_string(sec) + "s";
    } else {
      char tmp[16];
      std::snprintf(tmp, sizeof(tmp), "%09lld", static_cast<long long>(rem_ns));
      int n = 9;
      while (n > 0 && tmp[n - 1] == '0') --n;
      tmp[n] = 0;
      out += std::to_string(sec) + "." + tmp + "s";
    }
  }
  return neg ? "-" + out : out;
}

}  // namespace protowire::detail

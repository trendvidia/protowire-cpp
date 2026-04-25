#include "protowire/detail/duration.h"

#include <cstdio>
#include <cstdlib>

namespace protowire::detail {

namespace {

// Returns nanoseconds per unit, or -1 if not a known unit. Updates len with
// the number of bytes the unit occupies (1, 2, or 3 — "µs" is 2 bytes UTF-8).
int64_t UnitNanos(std::string_view s, size_t& len) {
  if (s.empty()) return -1;
  if (s.size() >= 2) {
    // µs is 0xC2 0xB5 in UTF-8.
    if (static_cast<unsigned char>(s[0]) == 0xC2 &&
        static_cast<unsigned char>(s[1]) == 0xB5 && s.size() >= 3 &&
        s[2] == 's') {
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

  // Total nanoseconds accumulator (signed-safe via __int128 if available;
  // we'll keep checks lightweight and rely on int64 with overflow guard).
  __int128 total_ns = 0;

  while (!s.empty()) {
    // integer part
    size_t i = 0;
    bool any_digit = false;
    long long whole = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      whole = whole * 10 + (s[i] - '0');
      any_digit = true;
      ++i;
    }
    long long frac_num = 0;
    long long frac_div = 1;
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

    __int128 v = static_cast<__int128>(whole) * unit_ns;
    if (frac_num > 0) {
      v += (static_cast<__int128>(frac_num) * unit_ns) / frac_div;
    }
    total_ns += v;
  }

  if (neg) total_ns = -total_ns;

  // Range check against int64.
  constexpr __int128 kI64Min = static_cast<__int128>(INT64_MIN);
  constexpr __int128 kI64Max = static_cast<__int128>(INT64_MAX);
  if (total_ns < kI64Min || total_ns > kI64Max) return std::nullopt;

  int64_t total = static_cast<int64_t>(total_ns);
  Duration d;
  d.seconds = total / 1'000'000'000LL;
  d.nanos = static_cast<int32_t>(total % 1'000'000'000LL);
  // Normalize so that nanos is in [0, 1e9).
  if (d.nanos < 0) {
    --d.seconds;
    d.nanos += 1'000'000'000;
  }
  return d;
}

std::string FormatDuration(int64_t seconds, int32_t nanos) {
  // Total nanoseconds; preserve sign.
  __int128 total_ns =
      static_cast<__int128>(seconds) * 1'000'000'000LL + nanos;
  if (total_ns == 0) return "0s";
  bool neg = total_ns < 0;
  if (neg) total_ns = -total_ns;

  // If smaller than 1 second, format with smallest unit ns/us/ms.
  std::string out;
  if (total_ns < 1'000'000'000LL) {
    // Pick unit.
    if (total_ns < 1'000) {
      out = std::to_string(static_cast<long long>(total_ns)) + "ns";
    } else if (total_ns < 1'000'000) {
      // µs
      long long us = static_cast<long long>(total_ns / 1'000);
      long long rem_ns = static_cast<long long>(total_ns % 1'000);
      char buf[64];
      if (rem_ns == 0) {
        std::snprintf(buf, sizeof(buf), "%lldµs", us);
      } else {
        // Trim trailing zeros from rem_ns formatted with width 3.
        char tmp[8];
        std::snprintf(tmp, sizeof(tmp), "%03lld", rem_ns);
        int n = 3;
        while (n > 0 && tmp[n - 1] == '0') --n;
        tmp[n] = 0;
        std::snprintf(buf, sizeof(buf), "%lld.%sµs", us, tmp);
      }
      out = buf;
    } else {
      long long ms = static_cast<long long>(total_ns / 1'000'000);
      long long rem_us = static_cast<long long>(total_ns % 1'000'000);
      char buf[64];
      if (rem_us == 0) {
        std::snprintf(buf, sizeof(buf), "%lldms", ms);
      } else {
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "%06lld", rem_us);
        int n = 6;
        while (n > 0 && tmp[n - 1] == '0') --n;
        tmp[n] = 0;
        std::snprintf(buf, sizeof(buf), "%lld.%sms", ms, tmp);
      }
      out = buf;
    }
  } else {
    long long s = static_cast<long long>(total_ns / 1'000'000'000LL);
    long long rem_ns = static_cast<long long>(total_ns % 1'000'000'000LL);
    long long h = s / 3600;
    long long m = (s / 60) % 60;
    long long sec = s % 60;
    if (h > 0) out += std::to_string(h) + "h";
    if (h > 0 || m > 0) out += std::to_string(m) + "m";
    // seconds (with fraction)
    if (rem_ns == 0) {
      out += std::to_string(sec) + "s";
    } else {
      char tmp[16];
      std::snprintf(tmp, sizeof(tmp), "%09lld", rem_ns);
      int n = 9;
      while (n > 0 && tmp[n - 1] == '0') --n;
      tmp[n] = 0;
      out += std::to_string(sec) + "." + tmp + "s";
    }
  }
  return neg ? "-" + out : out;
}

}  // namespace protowire::detail

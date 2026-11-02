// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/detail/rfc3339.h"

#include <charconv>
#include <cstdio>

namespace protowire::detail {

namespace {

// Returns days from 1970-01-01 to year/month/day. Algorithm from
// "Days from Civil" by Howard Hinnant — works for any positive year.
int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return int64_t{era} * 146097 + int64_t{doe} - 719468;
}

bool ParseUInt(std::string_view s, int& out) {
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

}  // namespace

// ParseRFC3339 takes what Go's time.Parse takes for time.RFC3339, which is
// looser than the RFC in four ways: the hour may be one digit, the fraction may
// open with ',', a fraction past nine digits is cut, and an offset runs to
// 24:60. It is stricter in one: 'T' and 'Z' are upper-case only.
std::optional<Timestamp> ParseRFC3339(std::string_view s) {
  const auto digit = [&](size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
  // YYYY-MM-DDTH:MM:SSZ is the shortest.
  if (s.size() < 19) return std::nullopt;
  if (s[4] != '-' || s[7] != '-' || s[10] != 'T') return std::nullopt;
  const size_t hour_digits = digit(12) ? 2 : 1;
  const size_t minutes = 11 + hour_digits + 1;
  if (s.size() < minutes + 6 || s[minutes - 1] != ':' || s[minutes + 2] != ':') {
    return std::nullopt;
  }
  int year, month, day, hour, minute, second;
  if (!ParseUInt(s.substr(0, 4), year) || !ParseUInt(s.substr(5, 2), month) ||
      !ParseUInt(s.substr(8, 2), day) || !ParseUInt(s.substr(11, hour_digits), hour) ||
      !ParseUInt(s.substr(minutes, 2), minute) || !ParseUInt(s.substr(minutes + 3, 2), second)) {
    return std::nullopt;
  }

  size_t pos = minutes + 5;
  int32_t nanos = 0;
  if (pos < s.size() && (s[pos] == '.' || s[pos] == ',') && digit(pos + 1)) {
    ++pos;
    int frac = 0;
    int digits = 0;
    for (; digit(pos); ++pos) {
      if (digits == 9) continue;
      frac = frac * 10 + (s[pos] - '0');
      ++digits;
    }
    for (; digits < 9; ++digits) frac *= 10;
    nanos = frac;
  }

  if (pos >= s.size()) return std::nullopt;

  // Timezone designator.
  int tz_offset_sec = 0;
  char tz = s[pos];
  if (tz == 'Z') {
    if (pos + 1 != s.size()) return std::nullopt;
  } else if (tz == '+' || tz == '-') {
    if (s.size() - pos != 6 || s[pos + 3] != ':') return std::nullopt;
    int oh, om;
    if (!ParseUInt(s.substr(pos + 1, 2), oh) || !ParseUInt(s.substr(pos + 4, 2), om)) {
      return std::nullopt;
    }
    if (oh > 24 || om > 60) return std::nullopt;
    tz_offset_sec = oh * 3600 + om * 60;
    if (tz == '-') tz_offset_sec = -tz_offset_sec;
  } else {
    return std::nullopt;
  }

  // Validate ranges.
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  // No leap second: Go's time.Parse refuses :60 as well.
  if (month < 1 || month > 12 || day < 1 || day > kDays[month - 1] + (leap ? 1 : 0) || hour > 23 ||
      minute > 59 || second > 59) {
    return std::nullopt;
  }

  int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  // Widen each int operand to int64 before multiplying so the products are
  // computed in int64 arithmetic rather than int (clang-tidy:
  // bugprone-implicit-widening-of-multiplication-result).
  int64_t secs =
      days * 86400 + static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(minute) * 60 + second;
  secs -= tz_offset_sec;
  return Timestamp{secs, nanos};
}

std::string FormatRFC3339Nano(int64_t seconds, int32_t nanos) {
  // Convert seconds to civil date.
  int64_t days = seconds / 86400;
  int64_t tod = seconds % 86400;
  if (tod < 0) {
    --days;
    tod += 86400;
  }
  // Inverse of DaysFromCivil.
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = static_cast<unsigned>(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = static_cast<int64_t>(yoe) + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);

  int hh = static_cast<int>(tod / 3600);
  int mm = static_cast<int>((tod / 60) % 60);
  int ss = static_cast<int>(tod % 60);

  char buf[64];
  if (nanos == 0) {
    std::snprintf(buf,
                  sizeof(buf),
                  "%04lld-%02u-%02u"
                  "T%02d:%02d:%02dZ",
                  static_cast<long long>(y),
                  m,
                  d,
                  hh,
                  mm,
                  ss);
    return std::string(buf);
  }
  // Trim every trailing zero, as Go's time.RFC3339Nano does.
  int width = 9;
  int n = nanos;
  while (n % 10 == 0) {
    n /= 10;
    --width;
  }
  std::snprintf(buf,
                sizeof(buf),
                "%04lld-%02u-%02u"
                "T%02d:%02d:%02d.%0*dZ",
                static_cast<long long>(y),
                m,
                d,
                hh,
                mm,
                ss,
                width,
                n);
  return std::string(buf);
}

}  // namespace protowire::detail

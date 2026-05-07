// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/detail/rfc3339.h"

#include <cctype>
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

std::optional<Timestamp> ParseRFC3339(std::string_view s) {
  // Minimum: YYYY-MM-DDTHH:MM:SSZ → 20 chars
  if (s.size() < 20) return std::nullopt;
  // Components.
  int year, month, day, hour, minute, second;
  if (s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != 't') || s[13] != ':' ||
      s[16] != ':') {
    return std::nullopt;
  }
  if (!ParseUInt(s.substr(0, 4), year) || !ParseUInt(s.substr(5, 2), month) ||
      !ParseUInt(s.substr(8, 2), day) || !ParseUInt(s.substr(11, 2), hour) ||
      !ParseUInt(s.substr(14, 2), minute) || !ParseUInt(s.substr(17, 2), second)) {
    return std::nullopt;
  }

  size_t pos = 19;
  int32_t nanos = 0;
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    size_t digits_start = pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
      ++pos;
    }
    size_t digits = pos - digits_start;
    if (digits == 0 || digits > 9) return std::nullopt;
    int frac = 0;
    for (size_t i = 0; i < digits; ++i) {
      frac = frac * 10 + (s[digits_start + i] - '0');
    }
    for (size_t i = digits; i < 9; ++i) frac *= 10;
    nanos = frac;
  }

  if (pos >= s.size()) return std::nullopt;

  // Timezone designator.
  int tz_offset_sec = 0;
  char tz = s[pos];
  if (tz == 'Z' || tz == 'z') {
    if (pos + 1 != s.size()) return std::nullopt;
  } else if (tz == '+' || tz == '-') {
    if (s.size() - pos != 6 || s[pos + 3] != ':') return std::nullopt;
    int oh, om;
    if (!ParseUInt(s.substr(pos + 1, 2), oh) || !ParseUInt(s.substr(pos + 4, 2), om)) {
      return std::nullopt;
    }
    tz_offset_sec = oh * 3600 + om * 60;
    if (tz == '-') tz_offset_sec = -tz_offset_sec;
  } else {
    return std::nullopt;
  }

  // Validate ranges.
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
    return std::nullopt;
  }

  int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  int64_t secs = days * 86400 + hour * 3600 + minute * 60 + second;
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
  // Trim trailing zeros to 3/6/9 width.
  int width = 9;
  int n = nanos;
  while (width > 3 && n % 1000 == 0) {
    n /= 1000;
    width -= 3;
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

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/detail/go_strconv.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "protowire/detail/go_quote.h"

namespace protowire::detail {

namespace {

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}
bool IsHex(char c) {
  return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
char Lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

std::string NumError(const char* fn, std::string_view s, const char* why) {
  return std::string("strconv.") + fn + ": parsing " + GoQuote(s) + ": " + why;
}

constexpr const char* kSyntax = "invalid syntax";
constexpr const char* kRange = "value out of range";

enum class Uint { kOk, kSyntax, kRange };

// ParseUint's loop: a bad byte past the point of overflow is never reached.
Uint ReadUint(std::string_view s, int bits, uint64_t* out) {
  if (s.empty()) return Uint::kSyntax;
  const uint64_t max = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
  const uint64_t cutoff = UINT64_MAX / 10 + 1;
  uint64_t n = 0;
  for (char c : s) {
    if (!IsDigit(c)) return Uint::kSyntax;
    if (n >= cutoff) {
      *out = max;
      return Uint::kRange;
    }
    n *= 10;
    const uint64_t n1 = n + static_cast<uint64_t>(c - '0');
    if (n1 < n || n1 > max) {
      *out = max;
      return Uint::kRange;
    }
    n = n1;
  }
  *out = n;
  return Uint::kOk;
}

// UnderscoreOK is strconv's rule: an underscore separates digits, or follows a
// base prefix, and a number with no prefix may hold none.
bool UnderscoreOK(std::string_view s) {
  char prev = '^';
  size_t i = 0;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) s.remove_prefix(1);
  bool hex = false;
  if (s.size() >= 2 && s[0] == '0' &&
      (Lower(s[1]) == 'b' || Lower(s[1]) == 'o' || Lower(s[1]) == 'x')) {
    i = 2;
    prev = '0';
    hex = Lower(s[1]) == 'x';
  }
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (IsDigit(c) || (hex && IsHex(c))) {
      prev = '0';
    } else if (c == '_') {
      if (prev != '0') return false;
      prev = '_';
    } else {
      if (prev == '_') return false;
      prev = '!';
    }
  }
  return prev != '_';
}

// CommonPrefix is the length of the case-insensitive match of s and word.
size_t CommonPrefix(std::string_view s, std::string_view word) {
  size_t n = 0;
  while (n < s.size() && n < word.size() && Lower(s[n]) == word[n]) ++n;
  return n;
}

// TimeQuote is package time's quote: every byte outside printable ASCII is \xHH.
std::string TimeQuote(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c >= 0x80 || c < 0x20) {
      out += "\\x";
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    } else {
      if (ch == '"' || ch == '\\') out += '\\';
      out += ch;
    }
  }
  out += '"';
  return out;
}

constexpr std::string_view kLayout = "2006-01-02T15:04:05Z07:00";

std::string TimeError(std::string_view value, std::string_view rest, std::string_view elem) {
  return "parsing time " + TimeQuote(value) + " as " + TimeQuote(kLayout) + ": cannot parse " +
         TimeQuote(rest) + " as " + TimeQuote(elem);
}
std::string TimeMessage(std::string_view value, const std::string& message) {
  return "parsing time " + TimeQuote(value) + message;
}

// GetNum is package time's getnum: one or two digits, or exactly two if fixed.
bool GetNum(std::string_view* s, bool fixed, int* out) {
  if (s->empty() || !IsDigit((*s)[0])) return false;
  if (s->size() < 2 || !IsDigit((*s)[1])) {
    if (fixed) return false;
    *out = (*s)[0] - '0';
    s->remove_prefix(1);
    return true;
  }
  *out = ((*s)[0] - '0') * 10 + ((*s)[1] - '0');
  s->remove_prefix(2);
  return true;
}

}  // namespace

bool GoParseUint(std::string_view s, int bits, uint64_t* out, std::string* err) {
  switch (ReadUint(s, bits, out)) {
    case Uint::kOk:
      return true;
    case Uint::kSyntax:
      *err = NumError("ParseUint", s, kSyntax);
      return false;
    case Uint::kRange:
      *err = NumError("ParseUint", s, kRange);
      return false;
  }
  return false;
}

bool GoParseInt(std::string_view s, int bits, int64_t* out, std::string* err) {
  std::string_view digits = s;
  bool neg = false;
  if (!digits.empty() && (digits[0] == '+' || digits[0] == '-')) {
    neg = digits[0] == '-';
    digits.remove_prefix(1);
  }
  uint64_t un = 0;
  const Uint r = ReadUint(digits, bits, &un);
  if (r == Uint::kSyntax) {
    *err = NumError("ParseInt", s, kSyntax);
    return false;
  }
  const uint64_t cutoff = uint64_t{1} << (bits - 1);
  if (r == Uint::kRange || (!neg && un >= cutoff) || (neg && un > cutoff)) {
    *err = NumError("ParseInt", s, kRange);
    return false;
  }
  *out = neg ? static_cast<int64_t>(~un + 1) : static_cast<int64_t>(un);
  return true;
}

bool GoParseFloat(std::string_view s, int bits, double* out, std::string* err) {
  const auto syntax = [&] {
    *err = NumError("ParseFloat", s, kSyntax);
    return false;
  };
  if (s.empty()) return syntax();

  // Inf, Infinity and NaN, in any case. A sign is taken on the first two only.
  {
    std::string_view t = s;
    double sign = 1;
    bool signed_form = false;
    if (t[0] == '+' || t[0] == '-') {
      sign = t[0] == '-' ? -1 : 1;
      signed_form = true;
      t.remove_prefix(1);
    }
    if (!t.empty() && (t[0] == 'i' || t[0] == 'I')) {
      const size_t n = CommonPrefix(t, "infinity");
      if ((n == 3 && t.size() == 3) || (n == 8 && t.size() == 8)) {
        *out = sign * std::numeric_limits<double>::infinity();
        return true;
      }
    }
    if (!signed_form && CommonPrefix(t, "nan") == 3 && t.size() == 3) {
      // math.NaN()'s bits, not the machine's default NaN: the two differ on
      // the wire.
      const uint64_t bits_of_nan = 0x7FF8000000000001ULL;
      std::memcpy(out, &bits_of_nan, sizeof(*out));
      return true;
    }
  }

  size_t i = 0;
  if (s[0] == '+' || s[0] == '-') i = 1;
  bool hex = false;
  if (i + 2 < s.size() && s[i] == '0' && Lower(s[i + 1]) == 'x') {
    hex = true;
    i += 2;
  }
  bool saw_dot = false, saw_digits = false, underscores = false;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '_') {
      underscores = true;
    } else if (c == '.') {
      if (saw_dot) break;
      saw_dot = true;
    } else if (IsDigit(c) || (hex && IsHex(c))) {
      saw_digits = true;
    } else {
      break;
    }
  }
  if (!saw_digits) return syntax();
  bool saw_exp = false;
  if (i < s.size() && Lower(s[i]) == (hex ? 'p' : 'e')) {
    ++i;
    if (i >= s.size()) return syntax();
    if (s[i] == '+' || s[i] == '-') ++i;
    if (i >= s.size() || !IsDigit(s[i])) return syntax();
    for (; i < s.size() && (IsDigit(s[i]) || s[i] == '_'); ++i) {
      if (s[i] == '_') underscores = true;
    }
    saw_exp = true;
  }
  if (hex && !saw_exp) return syntax();
  if (i != s.size()) return syntax();
  if (underscores && !UnderscoreOK(s)) return syntax();

  std::string plain;
  plain.reserve(s.size());
  for (char c : s) {
    if (c != '_') plain += c;
  }
  const double v = bits == 32 ? static_cast<double>(std::strtof(plain.c_str(), nullptr))
                              : std::strtod(plain.c_str(), nullptr);
  if (std::isinf(v)) {
    *err = NumError("ParseFloat", s, kRange);
    return false;
  }
  *out = v;
  return true;
}

std::string GoDurationError(std::string_view orig) {
  const std::string invalid = "time: invalid duration " + TimeQuote(orig);
  std::string_view s = orig;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) s.remove_prefix(1);
  if (s.empty()) return invalid;
  while (!s.empty()) {
    if (s[0] != '.' && !IsDigit(s[0])) return invalid;
    // leadingInt: past 2^63 the whole is invalid, whatever follows.
    size_t n = 0;
    uint64_t whole = 0;
    for (; n < s.size() && IsDigit(s[n]); ++n) {
      if (whole > (uint64_t{1} << 63) / 10) return invalid;
      whole = whole * 10 + static_cast<uint64_t>(s[n] - '0');
      if (whole > uint64_t{1} << 63) return invalid;
    }
    const bool pre = n != 0;
    s.remove_prefix(n);
    bool post = false;
    if (!s.empty() && s[0] == '.') {
      s.remove_prefix(1);
      n = 0;
      while (n < s.size() && IsDigit(s[n])) ++n;
      post = n != 0;
      s.remove_prefix(n);
    }
    if (!pre && !post) return invalid;
    size_t i = 0;
    while (i < s.size() && s[i] != '.' && !IsDigit(s[i])) ++i;
    if (i == 0) return "time: missing unit in duration " + TimeQuote(orig);
    const std::string_view unit = s.substr(0, i);
    s.remove_prefix(i);
    if (unit != "ns" && unit != "us" && unit != "\xC2\xB5s" && unit != "\xCE\xBCs" &&
        unit != "ms" && unit != "s" && unit != "m" && unit != "h") {
      return "time: unknown unit " + TimeQuote(unit) + " in duration " + TimeQuote(orig);
    }
  }
  return invalid;  // Well-formed, so it overflowed.
}

std::string GoTimestampError(std::string_view v) {
  std::string_view value = v;
  const auto literal = [&](char c) {
    if (!value.empty() && value[0] == c) {
      value.remove_prefix(1);
      return true;
    }
    return false;
  };
  const auto range = [&](const char* what) {
    return TimeMessage(v, std::string(": ") + what + " out of range");
  };

  std::string_view hold = value;
  int year = 0;
  bool year_ok = value.size() >= 4 && IsDigit(value[0]);
  if (year_ok) {
    for (char c : value.substr(0, 4)) {
      if (!IsDigit(c)) year_ok = false;
      year = year * 10 + (c - '0');
    }
  }
  if (!year_ok) return TimeError(v, hold, "2006");
  value.remove_prefix(4);

  if (!literal('-')) return TimeError(v, value, "-");
  hold = value;
  int month = 0;
  if (!GetNum(&value, true, &month)) return TimeError(v, hold, "01");
  if (month <= 0 || month > 12) return range("month");

  if (!literal('-')) return TimeError(v, value, "-");
  hold = value;
  int day = 0;
  if (!GetNum(&value, true, &day)) return TimeError(v, hold, "02");

  if (!literal('T')) return TimeError(v, value, "T");
  hold = value;
  int hour = 0;
  if (!GetNum(&value, false, &hour)) return TimeError(v, hold, "15");
  if (hour >= 24) return range("hour");

  if (!literal(':')) return TimeError(v, value, ":");
  hold = value;
  int minute = 0;
  if (!GetNum(&value, true, &minute)) return TimeError(v, hold, "04");
  if (minute >= 60) return range("minute");

  if (!literal(':')) return TimeError(v, value, ":");
  hold = value;
  int second = 0;
  if (!GetNum(&value, true, &second)) return TimeError(v, hold, "05");
  if (second >= 60) return range("second");
  if (value.size() >= 2 && (value[0] == '.' || value[0] == ',') && IsDigit(value[1])) {
    size_t n = 2;
    while (n < value.size() && IsDigit(value[n])) ++n;
    value.remove_prefix(n);
  }

  hold = value;
  if (!value.empty() && value[0] == 'Z') {
    value.remove_prefix(1);
  } else {
    if (value.size() < 6 || value[3] != ':') return TimeError(v, hold, "Z07:00");
    std::string_view hh = value.substr(1, 2), mm = value.substr(4, 2);
    const char sign = value[0];
    value.remove_prefix(6);
    int oh = 0, om = 0;
    const bool ok = GetNum(&hh, true, &oh) && GetNum(&mm, true, &om);
    if (oh > 24) return range("time zone offset hour");
    if (om > 60) return range("time zone offset minute");
    if (!ok || (sign != '+' && sign != '-')) return TimeError(v, hold, "Z07:00");
  }

  if (!value.empty()) return TimeMessage(v, ": extra text: " + TimeQuote(value));
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  if (day < 1 || day > kDays[month - 1] + (leap ? 1 : 0)) return range("day");
  return TimeMessage(v, ": extra text: \"\"");  // Not reached for what Go refuses.
}

}  // namespace protowire::detail

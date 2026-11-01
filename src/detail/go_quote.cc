// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// strconv.Quote, byte for byte. A rune that is not printable is escaped, and
// a byte that is not part of valid UTF-8 is written \xhh.

#include "protowire/detail/go_quote.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace protowire::detail {

namespace {

struct Range {
  uint32_t lo;
  uint32_t hi;
};

constexpr Range kPrintable[] = {
#include "go_isprint.inc"
};

constexpr char kHex[] = "0123456789abcdef";

// DecodeRune reads the rune at s[0]. It returns false with *width 1 for a byte
// that does not start valid UTF-8: an overlong form, a surrogate, a value past
// U+10FFFF or a truncated sequence.
bool DecodeRune(std::string_view s, uint32_t* rune, size_t* width) {
  const auto b = [&](size_t i) { return static_cast<unsigned char>(s[i]); };
  const auto cont = [&](size_t i, unsigned lo = 0x80, unsigned hi = 0xBF) {
    return i < s.size() && b(i) >= lo && b(i) <= hi;
  };
  *width = 1;
  const unsigned c = b(0);
  if (c >= 0xC2 && c <= 0xDF && cont(1)) {
    *rune = (c & 0x1Fu) << 6 | (b(1) & 0x3Fu);
    *width = 2;
    return true;
  }
  if (c >= 0xE0 && c <= 0xEF) {
    const unsigned lo = c == 0xE0 ? 0xA0 : 0x80;
    const unsigned hi = c == 0xED ? 0x9F : 0xBF;
    if (cont(1, lo, hi) && cont(2)) {
      *rune = (c & 0x0Fu) << 12 | (b(1) & 0x3Fu) << 6 | (b(2) & 0x3Fu);
      *width = 3;
      return true;
    }
    return false;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    const unsigned lo = c == 0xF0 ? 0x90 : 0x80;
    const unsigned hi = c == 0xF4 ? 0x8F : 0xBF;
    if (cont(1, lo, hi) && cont(2) && cont(3)) {
      *rune = (c & 0x07u) << 18 | (b(1) & 0x3Fu) << 12 | (b(2) & 0x3Fu) << 6 | (b(3) & 0x3Fu);
      *width = 4;
      return true;
    }
  }
  return false;
}

void AppendHex(std::string* out, uint32_t v, int digits) {
  for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
    *out += kHex[(v >> shift) & 0xF];
  }
}

}  // namespace

bool GoIsPrint(uint32_t rune) {
  const Range* end = std::end(kPrintable);
  const Range* it = std::lower_bound(
      std::begin(kPrintable), end, rune, [](const Range& r, uint32_t v) { return r.hi < v; });
  return it != end && it->lo <= rune;
}

std::string GoQuote(std::string_view s) {
  std::string out = "\"";
  for (size_t i = 0; i < s.size();) {
    uint32_t r = static_cast<unsigned char>(s[i]);
    size_t width = 1;
    if (r >= 0x80 && !DecodeRune(s.substr(i), &r, &width)) {
      out += "\\x";
      AppendHex(&out, static_cast<unsigned char>(s[i]), 2);
      ++i;
      continue;
    }
    if (r == '"' || r == '\\') {
      out += '\\';
      out += static_cast<char>(r);
    } else if (GoIsPrint(r)) {
      out.append(s, i, width);
    } else if (r == '\a') {
      out += "\\a";
    } else if (r == '\b') {
      out += "\\b";
    } else if (r == '\f') {
      out += "\\f";
    } else if (r == '\n') {
      out += "\\n";
    } else if (r == '\r') {
      out += "\\r";
    } else if (r == '\t') {
      out += "\\t";
    } else if (r == '\v') {
      out += "\\v";
    } else if (r < 0x20 || r == 0x7F) {
      out += "\\x";
      AppendHex(&out, r, 2);
    } else if (r < 0x10000) {
      out += "\\u";
      AppendHex(&out, r, 4);
    } else {
      out += "\\U";
      AppendHex(&out, r, 8);
    }
    i += width;
  }
  out += '"';
  return out;
}

}  // namespace protowire::detail

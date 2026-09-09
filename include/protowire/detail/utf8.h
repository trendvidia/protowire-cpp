// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <cstdint>
#include <string_view>

namespace protowire::detail {

// IsValidUTF8 reports whether s is well-formed UTF-8 in the strict sense
// Go's utf8.Valid uses: no overlong forms, no surrogates (U+D800–U+DFFF),
// nothing above U+10FFFF. A proto3 string field holds exactly such a
// sequence (HARDENING.md § UTF-8).
inline bool IsValidUTF8(std::string_view s) {
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    uint8_t c = p[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len;
    uint32_t cp;
    uint8_t lo = 0x80, hi = 0xBF;  // bounds for the second byte
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0F;
      if (c == 0xE0) lo = 0xA0;  // no overlongs
      if (c == 0xED) hi = 0x9F;  // no surrogates
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07;
      if (c == 0xF0) lo = 0x90;  // no overlongs
      if (c == 0xF4) hi = 0x8F;  // nothing above U+10FFFF
    } else {
      return false;  // 0x80–0xC1 (continuation or overlong lead), 0xF5–0xFF
    }
    if (n - i < len) return false;
    uint8_t c1 = p[i + 1];
    if (c1 < lo || c1 > hi) return false;
    cp = (cp << 6) | (c1 & 0x3F);
    for (size_t k = 2; k < len; ++k) {
      uint8_t ck = p[i + k];
      if (ck < 0x80 || ck > 0xBF) return false;
      cp = (cp << 6) | (ck & 0x3F);
    }
    i += len;
  }
  return true;
}

}  // namespace protowire::detail

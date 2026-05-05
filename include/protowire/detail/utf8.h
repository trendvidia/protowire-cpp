#pragma once

#include <cstdint>
#include <string_view>

namespace protowire::detail {

// IsValidUtf8 returns true iff `s` is a sequence of well-formed UTF-8 bytes:
// no isolated continuation bytes, no overlong encodings, no surrogate code
// points (U+D800–U+DFFF), no values above U+10FFFF. Mirrors Go's utf8.Valid.
inline bool IsValidUtf8(std::string_view s) {
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    uint8_t b0 = p[i];
    if (b0 < 0x80) { ++i; continue; }
    uint32_t cp;
    size_t need;
    uint32_t lo, hi;
    if ((b0 & 0xE0) == 0xC0) {  // 110xxxxx
      need = 2; cp = b0 & 0x1F; lo = 0x80; hi = 0x7FF;
    } else if ((b0 & 0xF0) == 0xE0) {  // 1110xxxx
      need = 3; cp = b0 & 0x0F; lo = 0x800; hi = 0xFFFF;
    } else if ((b0 & 0xF8) == 0xF0) {  // 11110xxx
      need = 4; cp = b0 & 0x07; lo = 0x10000; hi = 0x10FFFF;
    } else {
      return false;  // continuation byte without leader, or 5/6-byte (illegal).
    }
    if (i + need > n) return false;
    for (size_t k = 1; k < need; ++k) {
      uint8_t bk = p[i + k];
      if ((bk & 0xC0) != 0x80) return false;  // not a continuation byte.
      cp = (cp << 6) | (bk & 0x3F);
    }
    if (cp < lo || cp > hi) return false;          // overlong / out-of-range.
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // surrogate.
    i += need;
  }
  return true;
}

}  // namespace protowire::detail

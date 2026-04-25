#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace protowire::wire {

enum WireType : uint8_t {
  kVarint = 0,
  kFixed64 = 1,
  kBytes = 2,
  kStartGroup = 3,
  kEndGroup = 4,
  kFixed32 = 5,
};

using FieldNumber = uint32_t;

inline uint64_t EncodeZigZag(int64_t v) {
  return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}
inline int64_t DecodeZigZag(uint64_t v) {
  return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}

inline void AppendVarint(std::vector<uint8_t>& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<uint8_t>(v) | 0x80);
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}

inline void AppendTag(std::vector<uint8_t>& out, FieldNumber num,
                      WireType type) {
  AppendVarint(out, (static_cast<uint64_t>(num) << 3) |
                        static_cast<uint64_t>(type));
}

inline void AppendFixed32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

inline void AppendFixed64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>(v >> (i * 8)));
  }
}

inline void AppendBytes(std::vector<uint8_t>& out, const uint8_t* data,
                        size_t n) {
  AppendVarint(out, static_cast<uint64_t>(n));
  out.insert(out.end(), data, data + n);
}

inline void AppendBytes(std::vector<uint8_t>& out, std::string_view s) {
  AppendBytes(out, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

inline void AppendBytes(std::vector<uint8_t>& out,
                        const std::vector<uint8_t>& v) {
  AppendBytes(out, v.data(), v.size());
}

// Returns the number of bytes consumed, or -1 on error.
inline int ConsumeVarint(std::span<const uint8_t> data, uint64_t& out) {
  uint64_t v = 0;
  for (size_t i = 0; i < data.size() && i < 10; ++i) {
    uint8_t b = data[i];
    v |= static_cast<uint64_t>(b & 0x7f) << (i * 7);
    if ((b & 0x80) == 0) {
      out = v;
      return static_cast<int>(i + 1);
    }
  }
  return -1;
}

inline int ConsumeTag(std::span<const uint8_t> data, FieldNumber& num,
                      WireType& type) {
  uint64_t v;
  int n = ConsumeVarint(data, v);
  if (n < 0) return -1;
  num = static_cast<FieldNumber>(v >> 3);
  type = static_cast<WireType>(v & 0x7);
  return n;
}

inline int ConsumeFixed32(std::span<const uint8_t> data, uint32_t& out) {
  if (data.size() < 4) return -1;
  out = static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
  return 4;
}

inline int ConsumeFixed64(std::span<const uint8_t> data, uint64_t& out) {
  if (data.size() < 8) return -1;
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(data[i]) << (i * 8);
  }
  out = v;
  return 8;
}

// Consumes a length-prefixed byte string. On success, sets `out` to a span
// referencing the underlying data and returns total bytes consumed.
inline int ConsumeBytes(std::span<const uint8_t> data,
                        std::span<const uint8_t>& out) {
  uint64_t len;
  int n = ConsumeVarint(data, len);
  if (n < 0) return -1;
  if (data.size() - n < len) return -1;
  out = data.subspan(n, len);
  return n + static_cast<int>(len);
}

// Skips a single field's value. Returns bytes consumed, or -1 on error.
inline int ConsumeFieldValue(std::span<const uint8_t> data, FieldNumber num,
                             WireType type) {
  switch (type) {
    case kVarint: {
      uint64_t v;
      return ConsumeVarint(data, v);
    }
    case kFixed64: {
      uint64_t v;
      return ConsumeFixed64(data, v);
    }
    case kBytes: {
      std::span<const uint8_t> s;
      return ConsumeBytes(data, s);
    }
    case kFixed32: {
      uint32_t v;
      return ConsumeFixed32(data, v);
    }
    case kStartGroup:
    case kEndGroup:
      // Group support is not needed for proto3.
      return -1;
  }
  return -1;
}

}  // namespace protowire::wire

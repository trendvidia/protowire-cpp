// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
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

// The negative results of the Consume functions below, as protobuf-go's
// protowire numbers them, so that ParseError gives Go's text for each.
inline constexpr int kErrTruncated = -1;
inline constexpr int kErrFieldNumber = -2;
inline constexpr int kErrOverflow = -3;
inline constexpr int kErrReserved = -4;
inline constexpr int kErrEndGroup = -5;
inline constexpr int kErrRecursionDepth = -6;

// ParseError is protobuf-go's protowire.ParseError: the text of a negative
// Consume result. A field skipped as unknown names it in its error, as
// protowire-go does (protowire#614).
inline const char* ParseError(int n) {
  switch (n) {
    case kErrTruncated:
      return "unexpected EOF";
    case kErrFieldNumber:
      return "proto: invalid field number";
    case kErrOverflow:
      return "proto: variable length integer overflow";
    case kErrReserved:
      return "proto: cannot parse reserved wire type";
    case kErrEndGroup:
      return "proto: mismatching end group marker";
    default:
      return "proto: parse error";
  }
}

// CorruptFieldText is the error of a field skipped as unknown whose value does
// not parse, in protowire-go's words: "corrupt field <num><in>: <ParseError>",
// where `in` names the message it is in (" in BigInt"), if any.
inline std::string CorruptFieldText(FieldNumber num, std::string_view in, int n) {
  std::string out = "corrupt field " + std::to_string(num);
  out += in;
  out += ": ";
  out += ParseError(n);
  return out;
}

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

inline void AppendTag(std::vector<uint8_t>& out, FieldNumber num, WireType type) {
  AppendVarint(out, (static_cast<uint64_t>(num) << 3) | static_cast<uint64_t>(type));
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

inline void AppendBytes(std::vector<uint8_t>& out, const uint8_t* data, size_t n) {
  AppendVarint(out, static_cast<uint64_t>(n));
  out.insert(out.end(), data, data + n);
}

inline void AppendBytes(std::vector<uint8_t>& out, std::string_view s) {
  AppendBytes(out, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

inline void AppendBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& v) {
  AppendBytes(out, v.data(), v.size());
}

// Each Consume function returns the number of bytes consumed, or one of the
// kErr codes above on error.
inline int ConsumeVarint(std::span<const uint8_t> data, uint64_t& out) {
  uint64_t v = 0;
  for (size_t i = 0; i < data.size() && i < 10; ++i) {
    uint8_t b = data[i];
    if (i == 9 && b > 1) return kErrOverflow;  // past 64 bits
    v |= static_cast<uint64_t>(b & 0x7f) << (i * 7);
    if ((b & 0x80) == 0) {
      out = v;
      return static_cast<int>(i + 1);
    }
  }
  return kErrTruncated;
}

// The wire type is not checked here: 6 and 7 are refused by whatever reads
// the field's value, ConsumeFieldValue for one read as unknown.
inline int ConsumeTag(std::span<const uint8_t> data, FieldNumber& num, WireType& type) {
  uint64_t v;
  int n = ConsumeVarint(data, v);
  if (n < 0) return n;
  const uint64_t field = v >> 3;
  if (field == 0 || field > 0x7fffffff) return kErrFieldNumber;
  num = static_cast<FieldNumber>(field);
  type = static_cast<WireType>(v & 0x7);
  return n;
}

inline int ConsumeFixed32(std::span<const uint8_t> data, uint32_t& out) {
  if (data.size() < 4) return kErrTruncated;
  out = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return 4;
}

inline int ConsumeFixed64(std::span<const uint8_t> data, uint64_t& out) {
  if (data.size() < 8) return kErrTruncated;
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(data[i]) << (i * 8);
  }
  out = v;
  return 8;
}

// Consumes a length-prefixed byte string. On success, sets `out` to a span
// referencing the underlying data and returns total bytes consumed.
inline int ConsumeBytes(std::span<const uint8_t> data, std::span<const uint8_t>& out) {
  uint64_t len;
  int n = ConsumeVarint(data, len);
  if (n < 0) return n;
  if (data.size() - n < len) return kErrTruncated;
  out = data.subspan(n, len);
  return n + static_cast<int>(len);
}

// How many groups ConsumeFieldValue follows inside the first, as Go's
// protowire.DefaultRecursionLimit.
inline constexpr size_t kFieldValueRecursionLimit = 10000;

// Skips a single field's value. Returns bytes consumed, or a kErr code on
// error, as protobuf-go's protowire.ConsumeFieldValue: wire types 6 and 7 are
// kErrReserved, and an end-group that ends nothing, or another group, is
// kErrEndGroup.
//
// Nested groups are followed with a list rather than by recursion. The limit
// is Go's, and Go's stacks grow: ten thousand frames from a hostile message
// overflow a fixed C++ stack.
inline int ConsumeFieldValue(std::span<const uint8_t> data, FieldNumber num, WireType type) {
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
      break;
    case kEndGroup:
      return kErrEndGroup;
    default:
      return kErrReserved;
  }
  const size_t size = data.size();
  std::vector<FieldNumber> open = {num};
  while (!open.empty()) {
    FieldNumber inner;
    WireType inner_type;
    int n = ConsumeTag(data, inner, inner_type);
    if (n < 0) return n;
    data = data.subspan(n);
    if (inner_type == kEndGroup) {
      if (inner != open.back()) return kErrEndGroup;
      open.pop_back();
    } else if (inner_type == kStartGroup) {
      if (open.size() > kFieldValueRecursionLimit) return kErrRecursionDepth;
      open.push_back(inner);
    } else {
      n = ConsumeFieldValue(data, inner, inner_type);  // not a group, so one level
      if (n < 0) return n;
      data = data.subspan(n);
    }
  }
  return static_cast<int>(size - data.size());
}

}  // namespace protowire::wire

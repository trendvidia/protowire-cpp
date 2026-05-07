// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pb_big.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "protowire/detail/status.h"
#include "protowire/detail/wire.h"

namespace protowire::pb {

namespace {

// Multiplies a big-endian byte magnitude by 10 in place.
void MulBy10(std::vector<uint8_t>& mag) {
  uint16_t carry = 0;
  for (auto it = mag.rbegin(); it != mag.rend(); ++it) {
    uint16_t v = static_cast<uint16_t>(*it) * 10 + carry;
    *it = static_cast<uint8_t>(v & 0xff);
    carry = v >> 8;
  }
  while (carry > 0) {
    mag.insert(mag.begin(), static_cast<uint8_t>(carry & 0xff));
    carry >>= 8;
  }
}

// Adds an unsigned 8-bit value to a big-endian byte magnitude in place.
void AddU8(std::vector<uint8_t>& mag, uint8_t v) {
  uint16_t carry = v;
  for (auto it = mag.rbegin(); it != mag.rend() && carry; ++it) {
    uint16_t s = static_cast<uint16_t>(*it) + carry;
    *it = static_cast<uint8_t>(s & 0xff);
    carry = s >> 8;
  }
  while (carry > 0) {
    mag.insert(mag.begin(), static_cast<uint8_t>(carry & 0xff));
    carry >>= 8;
  }
}

// Divides a big-endian byte magnitude by 10 in place; returns the remainder.
uint8_t DivBy10(std::vector<uint8_t>& mag) {
  uint16_t rem = 0;
  for (auto& byte : mag) {
    uint16_t cur = (rem << 8) | byte;
    byte = static_cast<uint8_t>(cur / 10);
    rem = cur % 10;
  }
  while (!mag.empty() && mag.front() == 0) mag.erase(mag.begin());
  return static_cast<uint8_t>(rem);
}

bool ParseDigits(const std::string& s, size_t pos, size_t end, std::vector<uint8_t>& mag) {
  if (pos == end) return false;
  for (size_t i = pos; i < end; ++i) {
    char c = s[i];
    if (c < '0' || c > '9') return false;
    MulBy10(mag);
    AddU8(mag, static_cast<uint8_t>(c - '0'));
  }
  // Strip leading zero bytes; fully zero magnitude is represented as empty.
  while (!mag.empty() && mag.front() == 0) mag.erase(mag.begin());
  return true;
}

std::string FormatMagnitude(const std::vector<uint8_t>& mag) {
  if (mag.empty()) return "0";
  std::vector<uint8_t> tmp = mag;
  std::string digits;
  while (!tmp.empty()) {
    digits.push_back(static_cast<char>('0' + DivBy10(tmp)));
  }
  std::reverse(digits.begin(), digits.end());
  return digits;
}

}  // namespace

bool ParseBigInt(const std::string& s, BigInt& out) {
  out = {};
  if (s.empty()) return false;
  size_t pos = 0;
  if (s[0] == '-') {
    out.negative = true;
    pos = 1;
  } else if (s[0] == '+') {
    pos = 1;
  }
  if (!ParseDigits(s, pos, s.size(), out.abs)) return false;
  if (out.abs.empty()) out.negative = false;  // -0 → 0
  return true;
}

bool ParseDecimal(const std::string& s, Decimal& out) {
  out = {};
  if (s.empty()) return false;
  size_t pos = 0;
  if (s[0] == '-') {
    out.negative = true;
    pos = 1;
  } else if (s[0] == '+') {
    pos = 1;
  }
  // Find dot.
  size_t dot = s.find('.', pos);
  size_t int_end = (dot == std::string::npos) ? s.size() : dot;
  size_t frac_start = (dot == std::string::npos) ? s.size() : dot + 1;
  size_t frac_end = s.size();

  // Build unscaled magnitude from concatenated int + frac digits.
  std::vector<uint8_t> mag;
  if (int_end > pos) {
    if (!ParseDigits(s, pos, int_end, mag)) return false;
  }
  if (frac_start < frac_end) {
    // Continue multiplying mag by 10 for each fraction digit.
    for (size_t i = frac_start; i < frac_end; ++i) {
      char c = s[i];
      if (c < '0' || c > '9') return false;
      MulBy10(mag);
      AddU8(mag, static_cast<uint8_t>(c - '0'));
    }
    out.scale = static_cast<int32_t>(frac_end - frac_start);
  }
  while (!mag.empty() && mag.front() == 0) mag.erase(mag.begin());
  if (mag.empty() && int_end == pos && frac_start == frac_end) {
    return false;  // empty input besides sign
  }
  out.unscaled = std::move(mag);
  if (out.unscaled.empty()) out.negative = false;
  return true;
}

std::string FormatBigInt(const BigInt& v) {
  std::string digits = FormatMagnitude(v.abs);
  if (v.negative && !v.abs.empty()) return "-" + digits;
  return digits;
}

std::string FormatDecimal(const Decimal& v) {
  std::string digits = FormatMagnitude(v.unscaled);
  std::string out;
  if (v.negative && !v.unscaled.empty()) out = "-";
  if (v.scale <= 0) {
    out += digits;
    return out;
  }
  // Pad with leading zeros so we have at least scale+1 digits.
  while (static_cast<int32_t>(digits.size()) <= v.scale) digits = "0" + digits;
  size_t int_len = digits.size() - static_cast<size_t>(v.scale);
  out += digits.substr(0, int_len);
  out += '.';
  out += digits.substr(int_len);
  return out;
}

// --- Wire-format nested-message helpers (called from pb.h templates) -----

namespace detail {

void MarshalBigIntMsg(const BigInt& v, std::vector<uint8_t>& out) {
  if (!v.abs.empty()) {
    wire::AppendTag(out, 1, wire::kBytes);
    wire::AppendBytes(out, v.abs);
  }
  if (v.negative) {
    wire::AppendTag(out, 2, wire::kVarint);
    wire::AppendVarint(out, 1);
  }
}

void MarshalDecimalMsg(const Decimal& v, std::vector<uint8_t>& out) {
  if (!v.unscaled.empty()) {
    wire::AppendTag(out, 1, wire::kBytes);
    wire::AppendBytes(out, v.unscaled);
  }
  if (v.scale != 0) {
    wire::AppendTag(out, 2, wire::kVarint);
    wire::AppendVarint(out, wire::EncodeZigZag(static_cast<int64_t>(v.scale)));
  }
  if (v.negative) {
    wire::AppendTag(out, 3, wire::kVarint);
    wire::AppendVarint(out, 1);
  }
}

void MarshalBigFloatMsg(const BigFloat& v, std::vector<uint8_t>& out) {
  if (v.prec == 0) return;
  if (!v.mantissa.empty()) {
    wire::AppendTag(out, 1, wire::kBytes);
    wire::AppendBytes(out, v.mantissa);
  }
  if (v.exponent != 0) {
    wire::AppendTag(out, 2, wire::kVarint);
    wire::AppendVarint(out, wire::EncodeZigZag(static_cast<int64_t>(v.exponent)));
  }
  wire::AppendTag(out, 3, wire::kVarint);
  wire::AppendVarint(out, v.prec);
  if (v.negative) {
    wire::AppendTag(out, 4, wire::kVarint);
    wire::AppendVarint(out, 1);
  }
}

namespace {

template <class Out, class Msg>
Status ReadFields(std::span<const uint8_t> data, Msg& out, Out fn) {
  while (!data.empty()) {
    wire::FieldNumber num;
    wire::WireType type;
    int n = wire::ConsumeTag(data, num, type);
    if (n < 0) return Status::Error("corrupt big-number tag");
    data = data.subspan(n);
    int consumed = 0;
    Status st = fn(data, num, type, out, consumed);
    if (!st.ok()) return st;
    if (consumed == 0) {
      int skipped = wire::ConsumeFieldValue(data, num, type);
      if (skipped < 0) return Status::Error("corrupt big-number field");
      consumed = skipped;
    }
    data = data.subspan(consumed);
  }
  return Status::OK();
}

}  // namespace

Status UnmarshalBigIntMsg(std::span<const uint8_t> data, BigInt& out) {
  out = {};
  return ReadFields(data,
                    out,
                    [](std::span<const uint8_t> d,
                       wire::FieldNumber num,
                       wire::WireType,
                       BigInt& v,
                       int& consumed) -> Status {
                      if (num == 1) {
                        std::span<const uint8_t> b;
                        int n = wire::ConsumeBytes(d, b);
                        if (n < 0) return Status::Error("corrupt BigInt.abs");
                        v.abs.assign(b.begin(), b.end());
                        consumed = n;
                      } else if (num == 2) {
                        uint64_t x;
                        int n = wire::ConsumeVarint(d, x);
                        if (n < 0) return Status::Error("corrupt BigInt.negative");
                        v.negative = x != 0;
                        consumed = n;
                      }
                      return Status::OK();
                    });
}

Status UnmarshalDecimalMsg(std::span<const uint8_t> data, Decimal& out) {
  out = {};
  return ReadFields(data,
                    out,
                    [](std::span<const uint8_t> d,
                       wire::FieldNumber num,
                       wire::WireType,
                       Decimal& v,
                       int& consumed) -> Status {
                      if (num == 1) {
                        std::span<const uint8_t> b;
                        int n = wire::ConsumeBytes(d, b);
                        if (n < 0) return Status::Error("corrupt Decimal.unscaled");
                        v.unscaled.assign(b.begin(), b.end());
                        consumed = n;
                      } else if (num == 2) {
                        uint64_t x;
                        int n = wire::ConsumeVarint(d, x);
                        if (n < 0) return Status::Error("corrupt Decimal.scale");
                        v.scale = static_cast<int32_t>(wire::DecodeZigZag(x));
                        consumed = n;
                      } else if (num == 3) {
                        uint64_t x;
                        int n = wire::ConsumeVarint(d, x);
                        if (n < 0) return Status::Error("corrupt Decimal.negative");
                        v.negative = x != 0;
                        consumed = n;
                      }
                      return Status::OK();
                    });
}

Status UnmarshalBigFloatMsg(std::span<const uint8_t> data, BigFloat& out) {
  out = {};
  return ReadFields(data,
                    out,
                    [](std::span<const uint8_t> d,
                       wire::FieldNumber num,
                       wire::WireType,
                       BigFloat& v,
                       int& consumed) -> Status {
                      if (num == 1) {
                        std::span<const uint8_t> b;
                        int n = wire::ConsumeBytes(d, b);
                        if (n < 0) return Status::Error("corrupt BigFloat.mantissa");
                        v.mantissa.assign(b.begin(), b.end());
                        consumed = n;
                      } else if (num == 2) {
                        uint64_t x;
                        int n = wire::ConsumeVarint(d, x);
                        if (n < 0) return Status::Error("corrupt BigFloat.exponent");
                        v.exponent = static_cast<int32_t>(wire::DecodeZigZag(x));
                        consumed = n;
                      } else if (num == 3) {
                        uint64_t x;
                        int n = wire::ConsumeVarint(d, x);
                        if (n < 0) return Status::Error("corrupt BigFloat.prec");
                        v.prec = static_cast<uint32_t>(x);
                        consumed = n;
                      } else if (num == 4) {
                        uint64_t x;
                        int n = wire::ConsumeVarint(d, x);
                        if (n < 0) return Status::Error("corrupt BigFloat.negative");
                        v.negative = x != 0;
                        consumed = n;
                      }
                      return Status::OK();
                    });
}

}  // namespace detail
}  // namespace protowire::pb

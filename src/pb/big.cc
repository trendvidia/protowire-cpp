// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/limits.h"
#include "protowire/pb_big.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "protowire/detail/status.h"
#include "protowire/detail/wire.h"

// protowire_pb does not link protobuf, so its headers must not pull it in.
#ifdef GOOGLE_PROTOBUF_VERSION
#error "protowire/pb.h includes protobuf"
#endif

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

void TrimLeadingZeros(std::vector<uint8_t>& mag) {
  mag.erase(mag.begin(), std::find_if(mag.begin(), mag.end(), [](uint8_t b) { return b != 0; }));
}

// The bit length of a magnitude with no leading zero byte.
int64_t BitLen(const std::vector<uint8_t>& mag) {
  if (mag.empty()) return 0;
  int64_t n = static_cast<int64_t>(mag.size() - 1) * 8;
  for (uint8_t top = mag.front(); top != 0; top >>= 1) ++n;
  return n;
}

// Drops the low n bits of a magnitude, 0 < n < its bit length, rounding half to
// even as big.Float does.
void ShiftRightRounded(std::vector<uint8_t>& mag, int64_t n) {
  const auto bit = [&](int64_t i) {
    return ((mag[mag.size() - 1 - static_cast<size_t>(i / 8)] >> (i % 8)) & 1) != 0;
  };
  const bool half = bit(n - 1);
  bool sticky = false;
  for (int64_t i = 0; i < n - 1 && !sticky; ++i) sticky = bit(i);

  mag.resize(mag.size() - static_cast<size_t>(n / 8));
  if (const int shift = static_cast<int>(n % 8); shift != 0) {
    for (size_t i = mag.size(); i-- > 0;) {
      const unsigned high = i == 0 ? 0 : mag[i - 1];
      mag[i] = static_cast<uint8_t>(((high << 8) | mag[i]) >> shift);
    }
  }
  if (half && (sticky || (mag.back() & 1) != 0)) AddU8(mag, 1);
  TrimLeadingZeros(mag);
}

void ShiftLeft(std::vector<uint8_t>& mag, int64_t n) {
  if (const int shift = static_cast<int>(n % 8); shift != 0) {
    unsigned carry = 0;
    for (auto it = mag.rbegin(); it != mag.rend(); ++it) {
      const unsigned v = (static_cast<unsigned>(*it) << shift) | carry;
      *it = static_cast<uint8_t>(v & 0xff);
      carry = v >> 8;
    }
    if (carry != 0) mag.insert(mag.begin(), static_cast<uint8_t>(carry));
  }
  mag.resize(mag.size() + static_cast<size_t>(n / 8), 0);
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

namespace {

// The digits a literal is bounded by: leading zeros are not significant, so
// 0.000…5 at the largest scale counts one.
size_t SignificantDigits(const std::string& literal) {
  size_t n = 0;
  bool leading = true;
  for (char c : literal) {
    if (c < '0' || c > '9' || (leading && c == '0')) continue;
    leading = false;
    ++n;
  }
  return n;
}

StatusOr<std::string> BoundLiteral(const char* kind, std::string literal) {
  if (literal.size() > static_cast<size_t>(kMaxNumericLiteralDigits)) {
    size_t n = SignificantDigits(literal);
    if (n > static_cast<size_t>(kMaxNumericLiteralDigits)) {
      return Status::Error(
          std::string(kind) + " renders to " + std::to_string(n) +
          " digits; MaxNumericLiteralDigits=" + std::to_string(kMaxNumericLiteralDigits));
    }
  }
  return literal;
}

}  // namespace

StatusOr<std::string> FormatBigInt(const BigInt& v) {
  std::string digits = FormatMagnitude(v.abs);
  if (v.negative && !v.abs.empty()) digits.insert(digits.begin(), '-');
  return BoundLiteral("pxf.BigInt", std::move(digits));
}

StatusOr<std::string> FormatDecimal(const Decimal& v) {
  // Compared as int64 so -scale is defined at INT32_MIN.
  const int64_t scale = v.scale;
  if (scale > kMaxNumericLiteralDigits || scale < -int64_t{kMaxNumericLiteralDigits}) {
    return Status::Error(
        "pxf.Decimal scale " + std::to_string(scale) +
        " exceeds MaxNumericLiteralDigits=" + std::to_string(kMaxNumericLiteralDigits));
  }
  std::string digits = FormatMagnitude(v.unscaled);
  std::string out;
  if (v.negative && !v.unscaled.empty()) out = "-";
  if (scale <= 0) {
    out += digits;
    // A negative scale means trailing zeros: unscaled 5, scale -2 is 500.
    if (!v.unscaled.empty()) out.append(static_cast<size_t>(-scale), '0');
    return BoundLiteral("pxf.Decimal", std::move(out));
  }
  // Pad with leading zeros so we have at least scale+1 digits.
  const size_t want = static_cast<size_t>(scale) + 1;
  if (digits.size() < want) digits.insert(0, want - digits.size(), '0');
  size_t int_len = digits.size() - static_cast<size_t>(scale);
  out += digits.substr(0, int_len);
  out += '.';
  out += digits.substr(int_len);
  return BoundLiteral("pxf.Decimal", std::move(out));
}

// --- Wire-format nested-message helpers (called from pb.h templates) -----

namespace {

// Reduce works on base 2^32 limbs, least significant first, a batch of nine
// digits at a time. One digit at a time over bytes took 48 s on a 4 MiB value,
// and a message that size must not be able to stall Marshal.
constexpr uint32_t kPow10[] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
constexpr int kBatchDigits = 9;

std::vector<uint32_t> ToLimbs(const std::vector<uint8_t>& mag) {
  std::vector<uint32_t> limbs((mag.size() + 3) / 4, 0);
  for (size_t i = 0; i < mag.size(); ++i) {
    const size_t pos = mag.size() - 1 - i;  // bytes from the least significant
    limbs[i / 4] |= static_cast<uint32_t>(mag[pos]) << (8 * (i % 4));
  }
  return limbs;
}

std::vector<uint8_t> FromLimbs(const std::vector<uint32_t>& limbs) {
  std::vector<uint8_t> mag(limbs.size() * 4);
  for (size_t i = 0; i < mag.size(); ++i) {
    mag[mag.size() - 1 - i] = static_cast<uint8_t>(limbs[i / 4] >> (8 * (i % 4)));
  }
  TrimLeadingZeros(mag);
  return mag;
}

void MultiplyLimbs(std::vector<uint32_t>& limbs, uint32_t factor) {
  uint64_t carry = 0;
  for (uint32_t& limb : limbs) {
    const uint64_t v = uint64_t{limb} * factor + carry;
    limb = static_cast<uint32_t>(v);
    carry = v >> 32;
  }
  if (carry != 0) limbs.push_back(static_cast<uint32_t>(carry));
}

// Leaves the quotient in quotient and returns the remainder.
uint32_t DivideLimbs(const std::vector<uint32_t>& limbs,
                     uint32_t divisor,
                     std::vector<uint32_t>& quotient) {
  quotient.resize(limbs.size());
  uint64_t rem = 0;
  for (size_t i = limbs.size(); i-- > 0;) {
    const uint64_t cur = (rem << 32) | limbs[i];
    quotient[i] = static_cast<uint32_t>(cur / divisor);
    rem = cur % divisor;
  }
  while (!quotient.empty() && quotient.back() == 0) quotient.pop_back();
  return static_cast<uint32_t>(rem);
}

// Rewrites a non-zero unscaled × 10^(-scale) with the smallest scale >= 0 that
// holds it exactly, which is what a big.Rat comes back as.
void Reduce(std::vector<uint8_t>& unscaled, int64_t& scale) {
  std::vector<uint32_t> limbs = ToLimbs(unscaled);
  while (scale < 0) {
    const int step = static_cast<int>(std::min<int64_t>(kBatchDigits, -scale));
    MultiplyLimbs(limbs, kPow10[step]);
    scale += step;
  }
  std::vector<uint32_t> quotient;
  for (int step : {kBatchDigits, 1}) {
    while (scale >= step && DivideLimbs(limbs, kPow10[step], quotient) == 0) {
      limbs.swap(quotient);
      scale -= step;
    }
  }
  unscaled = FromLimbs(limbs);
}

}  // namespace

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

// Writes the form protowire-go writes, which is one per value: Go holds a
// big.Rat, which has no scale to keep, so 1.00 goes out as 1 and 5e2 as 500. The
// text path does not come through here, and keeps the scale a literal gave it.
void MarshalDecimalMsg(const Decimal& v, std::vector<uint8_t>& out) {
  std::vector<uint8_t> unscaled = v.unscaled;
  TrimLeadingZeros(unscaled);
  if (unscaled.empty()) return;  // a zero at any scale, with no sign

  int64_t scale = v.scale;
  // Past the bound there is no decoder to agree with, Go's Marshal refuses, and
  // pb::Marshal cannot (#93): such a value keeps the form it was given, which
  // is the same number. The bound also stops scale -2^31 asking for 2 GiB.
  if (scale >= -int64_t{kMaxNumericLiteralDigits} && scale != 0) {
    Reduce(unscaled, scale);
  }
  if (scale > kMaxNumericLiteralDigits || scale < -int64_t{kMaxNumericLiteralDigits}) {
    unscaled = v.unscaled;
    scale = v.scale;
  }

  wire::AppendTag(out, 1, wire::kBytes);
  wire::AppendBytes(out, unscaled);
  if (scale != 0) {
    // `int32 scale = 2`: a plain varint, a negative value sign-extended to 64
    // bits. Zigzag is sint32, which this field is not.
    wire::AppendTag(out, 2, wire::kVarint);
    wire::AppendVarint(out, static_cast<uint64_t>(scale));
  }
  if (v.negative) {
    wire::AppendTag(out, 3, wire::kVarint);
    wire::AppendVarint(out, 1);
  }
}

// Writes the form protowire-go writes, which is one per value: the mantissa as
// a prec-bit integer with its top bit set, and a zero with no exponent. A value
// is rounded to prec first, since that is what prec means.
void MarshalBigFloatMsg(const BigFloat& v, std::vector<uint8_t>& out) {
  if (v.prec == 0) {
    // ±0, and its sign is all there is to write (protowire#410).
    if (v.negative) {
      wire::AppendTag(out, 4, wire::kVarint);
      wire::AppendVarint(out, 1);
    }
    return;
  }
  const int64_t prec = v.prec;
  std::vector<uint8_t> mantissa = v.mantissa;
  TrimLeadingZeros(mantissa);
  int64_t exponent = 0;
  if (!mantissa.empty()) {
    exponent = v.exponent;
    if (const int64_t bits = BitLen(mantissa); bits > prec) {
      ShiftRightRounded(mantissa, bits - prec);
      exponent += bits - prec;
    }
    const int64_t bits = BitLen(mantissa);
    const int64_t widened = exponent + bits - prec;
    // Two values have no canonical form, and pb::Marshal cannot refuse them
    // (#93), so they keep the form they were given, which is the same number.
    // The exponent can leave int32. And from 2^31 bits Go's Marshal has no
    // behaviour to match, since it crashes, while widening here would allocate
    // up to 512 MiB for five bytes of prec.
    if (prec > std::numeric_limits<int32_t>::max() ||
        widened < std::numeric_limits<int32_t>::min() ||
        widened > std::numeric_limits<int32_t>::max()) {
      mantissa = v.mantissa;
      exponent = v.exponent;
    } else {
      if (bits > prec) {
        ShiftRightRounded(mantissa, bits - prec);  // the round up carried past the top bit
      } else {
        ShiftLeft(mantissa, prec - bits);
      }
      exponent = widened;
    }
  }
  if (!mantissa.empty()) {
    wire::AppendTag(out, 1, wire::kBytes);
    wire::AppendBytes(out, mantissa);
  }
  if (exponent != 0) {
    // `int32 exponent = 2`: a plain varint, as for Decimal.scale.
    wire::AppendTag(out, 2, wire::kVarint);
    wire::AppendVarint(out, static_cast<uint64_t>(exponent));
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
Status ReadFields(std::span<const uint8_t> data, const char* message, Msg& out, Out fn) {
  while (!data.empty()) {
    wire::FieldNumber num;
    wire::WireType type;
    int n = wire::ConsumeTag(data, num, type);
    if (n < 0) return Status::Error(std::string("corrupt tag in ") + message);
    data = data.subspan(n);
    int consumed = 0;
    Status st = fn(data, num, type, out, consumed);
    if (!st.ok()) return st;
    if (consumed == 0) {
      int skipped = wire::ConsumeFieldValue(data, num, type);
      if (skipped < 0) {
        return Status::Error("corrupt field " + std::to_string(num) + " in " + message);
      }
      consumed = skipped;
    }
    data = data.subspan(consumed);
  }
  return Status::OK();
}

}  // namespace

Status UnmarshalBigIntMsg(std::span<const uint8_t> data, BigInt& out) {
  out = {};
  Status st = ReadFields(data,
                         "BigInt",
                         out,
                         [](std::span<const uint8_t> d,
                            wire::FieldNumber num,
                            wire::WireType,
                            BigInt& v,
                            int& consumed) -> Status {
                           if (num == 1) {
                             std::span<const uint8_t> b;
                             int n = wire::ConsumeBytes(d, b);
                             if (n < 0) return Status::Error("corrupt abs in BigInt");
                             v.abs.assign(b.begin(), b.end());
                             consumed = n;
                           } else if (num == 2) {
                             uint64_t x;
                             int n = wire::ConsumeVarint(d, x);
                             if (n < 0) return Status::Error("corrupt negative in BigInt");
                             v.negative = x != 0;
                             consumed = n;
                           }
                           return Status::OK();
                         });
  if (!st.ok()) return st;
  // A value, as in protowire-go: no leading zeros, and no -0.
  TrimLeadingZeros(out.abs);
  if (out.abs.empty()) out.negative = false;
  return Status::OK();
}

Status UnmarshalDecimalMsg(std::span<const uint8_t> data, Decimal& out, int max_digits) {
  out = {};
  if (max_digits <= 0) max_digits = kMaxNumericLiteralDigits;
  Status st = ReadFields(data,
                         "Decimal",
                         out,
                         [](std::span<const uint8_t> d,
                            wire::FieldNumber num,
                            wire::WireType,
                            Decimal& v,
                            int& consumed) -> Status {
                           if (num == 1) {
                             std::span<const uint8_t> b;
                             int n = wire::ConsumeBytes(d, b);
                             if (n < 0) return Status::Error("corrupt unscaled in Decimal");
                             v.unscaled.assign(b.begin(), b.end());
                             consumed = n;
                           } else if (num == 2) {
                             uint64_t x;
                             int n = wire::ConsumeVarint(d, x);
                             if (n < 0) return Status::Error("corrupt scale in Decimal");
                             // Plain varint: the low 32 bits, read as signed.
                             v.scale = static_cast<int32_t>(x);
                             consumed = n;
                           } else if (num == 3) {
                             uint64_t x;
                             int n = wire::ConsumeVarint(d, x);
                             if (n < 0) return Status::Error("corrupt negative in Decimal");
                             v.negative = x != 0;
                             consumed = n;
                           }
                           return Status::OK();
                         });
  if (!st.ok()) return st;
  // The scale is a digit count and MaxNumericLiteralDigits bounds its
  // magnitude, on both signs: value = unscaled × 10^(−scale), so a
  // consumer that materialises the value computes 10^|scale| from five
  // attacker-written bytes (HARDENING.md § Mandatory limits, protowire#279).
  if (out.scale > max_digits || out.scale < -max_digits) {
    return Status::Error("pxf.Decimal scale " + std::to_string(out.scale) +
                         " exceeds MaxNumericLiteralDigits=" + std::to_string(max_digits));
  }
  // A value, as in protowire-go: no leading zeros, and no -0.
  TrimLeadingZeros(out.unscaled);
  if (out.unscaled.empty()) out.negative = false;
  return Status::OK();
}

Status UnmarshalBigFloatMsg(std::span<const uint8_t> data, BigFloat& out, int max_exponent) {
  out = {};
  if (max_exponent <= 0) max_exponent = kMaxBigFloatExponent;
  Status st = ReadFields(data,
                         "BigFloat",
                         out,
                         [](std::span<const uint8_t> d,
                            wire::FieldNumber num,
                            wire::WireType,
                            BigFloat& v,
                            int& consumed) -> Status {
                           if (num == 1) {
                             std::span<const uint8_t> b;
                             int n = wire::ConsumeBytes(d, b);
                             if (n < 0) return Status::Error("corrupt mantissa in BigFloat");
                             v.mantissa.assign(b.begin(), b.end());
                             consumed = n;
                           } else if (num == 2) {
                             uint64_t x;
                             int n = wire::ConsumeVarint(d, x);
                             if (n < 0) return Status::Error("corrupt exponent in BigFloat");
                             // Plain varint, as for Decimal.scale.
                             v.exponent = static_cast<int32_t>(x);
                             consumed = n;
                           } else if (num == 3) {
                             uint64_t x;
                             int n = wire::ConsumeVarint(d, x);
                             if (n < 0) return Status::Error("corrupt prec in BigFloat");
                             // big.Float's largest precision, which SetPrec clamps to.
                             v.prec = static_cast<uint32_t>(
                                 std::min<uint64_t>(x, std::numeric_limits<uint32_t>::max()));
                             consumed = n;
                           } else if (num == 4) {
                             uint64_t x;
                             int n = wire::ConsumeVarint(d, x);
                             if (n < 0) return Status::Error("corrupt negative in BigFloat");
                             v.negative = x != 0;
                             consumed = n;
                           }
                           return Status::OK();
                         });
  if (!st.ok()) return st;
  // A precision of zero is a zero, signed by negative, whatever else is there.
  if (out.prec == 0) {
    out.mantissa.clear();
    out.exponent = 0;
    return Status::OK();
  }
  TrimLeadingZeros(out.mantissa);
  const int64_t bits = BitLen(out.mantissa);
  if (bits == 0) {  // a zero at any exponent
    out.exponent = 0;
    return Status::OK();
  }
  // Rendering the value costs time that grows with the square of its binary
  // exponent, and five attacker-written bytes set that, so the bound comes
  // first (HARDENING.md § Mandatory limits, protowire#281).
  const int64_t binary = out.exponent + bits;
  if (binary > max_exponent || binary < -int64_t{max_exponent}) {
    return Status::Error("pxf.BigFloat binary exponent " + std::to_string(binary) +
                         " exceeds MaxBigFloatExponent=" + std::to_string(max_exponent));
  }
  // The value is the mantissa rounded to prec. The bound was on the wire's
  // mantissa, so a round up can land one past it, as in protowire-go. Only a
  // narrowing is done here: widening to prec is Marshal's, because a decode
  // must not allocate what five bytes of prec ask for.
  const int64_t prec = out.prec;
  int64_t exponent = out.exponent;
  if (bits > prec) {
    ShiftRightRounded(out.mantissa, bits - prec);
    exponent += bits - prec;
  }
  // Only a bound raised near INT32_MAX reaches this: big.Float's exponent is
  // an int32, and past it the value would be an infinity (protowire#278).
  if (exponent + BitLen(out.mantissa) > std::numeric_limits<int32_t>::max()) {
    return Status::Error("pxf.BigFloat exponent " + std::to_string(out.exponent) + " with a " +
                         std::to_string(bits) + "-bit mantissa overflows big.Float");
  }
  out.exponent = static_cast<int32_t>(exponent);
  return Status::OK();
}

}  // namespace detail
}  // namespace protowire::pb

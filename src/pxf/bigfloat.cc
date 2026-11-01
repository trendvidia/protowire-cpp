// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "bigfloat.h"

#include <algorithm>
#include <cstdio>
#include <tuple>
#include <utility>

namespace protowire::pxf {

namespace {

constexpr uint32_t kDefaultPrec = 256;

// Nat is an arbitrary-precision natural number, little-endian in 32-bit
// limbs, with no leading zero limb. Operands here are bounded by the
// literal limits before they are built, so schoolbook algorithms suffice.
class Nat {
 public:
  Nat() = default;
  explicit Nat(uint32_t v) {
    if (v != 0) w_.push_back(v);
  }

  static Nat FromBytes(const std::vector<uint8_t>& be) {
    Nat n;
    n.w_.assign((be.size() + 3) / 4, 0);
    for (size_t i = 0; i < be.size(); ++i) {
      n.w_[i / 4] |= uint32_t{be[be.size() - 1 - i]} << (8 * (i % 4));
    }
    n.Trim();
    return n;
  }

  std::vector<uint8_t> ToBytes() const {
    std::vector<uint8_t> be;
    for (size_t i = (BitLen() + 7) / 8; i > 0; --i) {
      be.push_back(static_cast<uint8_t>(w_[(i - 1) / 4] >> (8 * ((i - 1) % 4))));
    }
    return be;
  }

  bool IsZero() const { return w_.empty(); }
  bool IsOdd() const { return !w_.empty() && (w_[0] & 1) != 0; }

  uint64_t BitLen() const {
    if (w_.empty()) return 0;
    uint64_t n = (w_.size() - 1) * 32;
    for (uint32_t top = w_.back(); top != 0; top >>= 1) ++n;
    return n;
  }

  bool Bit(uint64_t i) const {
    const size_t limb = i / 32;
    return limb < w_.size() && ((w_[limb] >> (i % 32)) & 1) != 0;
  }

  // AnyBitBelow reports whether any bit under position i is set.
  bool AnyBitBelow(uint64_t i) const {
    const size_t limbs = std::min<size_t>(i / 32, w_.size());
    for (size_t k = 0; k < limbs; ++k) {
      if (w_[k] != 0) return true;
    }
    return limbs < w_.size() && (i % 32) != 0 && (w_[limbs] << (32 - i % 32)) != 0;
  }

  void MulSmall(uint32_t m) {
    uint64_t carry = 0;
    for (uint32_t& limb : w_) {
      const uint64_t v = uint64_t{limb} * m + carry;
      limb = static_cast<uint32_t>(v);
      carry = v >> 32;
    }
    if (carry != 0) w_.push_back(static_cast<uint32_t>(carry));
    Trim();
  }

  void AddSmall(uint32_t a) {
    uint64_t carry = a;
    for (size_t i = 0; carry != 0; ++i) {
      if (i == w_.size()) w_.push_back(0);
      const uint64_t v = uint64_t{w_[i]} + carry;
      w_[i] = static_cast<uint32_t>(v);
      carry = v >> 32;
    }
  }

  // DivSmall divides in place and returns the remainder.
  uint32_t DivSmall(uint32_t d) {
    uint64_t rem = 0;
    for (size_t i = w_.size(); i > 0; --i) {
      const uint64_t v = (rem << 32) | w_[i - 1];
      w_[i - 1] = static_cast<uint32_t>(v / d);
      rem = v % d;
    }
    Trim();
    return static_cast<uint32_t>(rem);
  }

  Nat Shl(uint64_t bits) const {
    if (w_.empty()) return *this;
    Nat out;
    const size_t limbs = bits / 32;
    const unsigned rest = bits % 32;
    out.w_.assign(w_.size() + limbs + 1, 0);
    for (size_t i = 0; i < w_.size(); ++i) {
      const uint64_t v = uint64_t{w_[i]} << rest;
      out.w_[i + limbs] |= static_cast<uint32_t>(v);
      out.w_[i + limbs + 1] |= static_cast<uint32_t>(v >> 32);
    }
    out.Trim();
    return out;
  }

  Nat Shr(uint64_t bits) const {
    Nat out;
    const size_t limbs = bits / 32;
    if (limbs >= w_.size()) return out;
    const unsigned rest = bits % 32;
    out.w_.assign(w_.size() - limbs, 0);
    for (size_t i = limbs; i < w_.size(); ++i) {
      uint64_t v = w_[i];
      if (i + 1 < w_.size()) v |= uint64_t{w_[i + 1]} << 32;
      out.w_[i - limbs] = static_cast<uint32_t>(v >> rest);
    }
    out.Trim();
    return out;
  }

  static int Cmp(const Nat& a, const Nat& b) {
    if (a.w_.size() != b.w_.size()) return a.w_.size() < b.w_.size() ? -1 : 1;
    for (size_t i = a.w_.size(); i > 0; --i) {
      if (a.w_[i - 1] != b.w_[i - 1]) return a.w_[i - 1] < b.w_[i - 1] ? -1 : 1;
    }
    return 0;
  }

  static Nat Mul(const Nat& a, const Nat& b) {
    Nat out;
    if (a.IsZero() || b.IsZero()) return out;
    out.w_.assign(a.w_.size() + b.w_.size(), 0);
    for (size_t i = 0; i < a.w_.size(); ++i) {
      uint64_t carry = 0;
      for (size_t j = 0; j < b.w_.size(); ++j) {
        const uint64_t v = uint64_t{a.w_[i]} * b.w_[j] + out.w_[i + j] + carry;
        out.w_[i + j] = static_cast<uint32_t>(v);
        carry = v >> 32;
      }
      out.w_[i + b.w_.size()] = static_cast<uint32_t>(carry);
    }
    out.Trim();
    return out;
  }

  static Nat Pow10(int64_t n) {
    Nat out(1);
    for (; n >= 9; n -= 9) out.MulSmall(1000000000);
    for (; n > 0; --n) out.MulSmall(10);
    return out;
  }

  // DivMod is Knuth's Algorithm D. d must not be zero.
  static std::pair<Nat, Nat> DivMod(const Nat& a, const Nat& d) {
    if (Cmp(a, d) < 0) return {Nat(), a};
    if (d.w_.size() == 1) {
      Nat q = a;
      const uint32_t r = q.DivSmall(d.w_[0]);
      return {std::move(q), Nat(r)};
    }
    // Normalize so the divisor's top bit is set.
    unsigned shift = 0;
    for (uint32_t top = d.w_.back(); (top & 0x80000000u) == 0; top <<= 1) ++shift;
    const Nat v = d.Shl(shift);
    Nat u = a.Shl(shift);
    const size_t n = v.w_.size();
    u.w_.resize(std::max(u.w_.size(), a.w_.size()) + 1, 0);
    const size_t m = u.w_.size() - n - 1;
    Nat q;
    q.w_.assign(m + 1, 0);
    const uint64_t b = uint64_t{1} << 32;
    for (size_t j = m + 1; j > 0; --j) {
      const size_t k = j - 1;
      const uint64_t top = (uint64_t{u.w_[k + n]} << 32) | u.w_[k + n - 1];
      uint64_t qhat = top / v.w_[n - 1];
      uint64_t rhat = top % v.w_[n - 1];
      while (qhat >= b || qhat * v.w_[n - 2] > ((rhat << 32) | u.w_[k + n - 2])) {
        --qhat;
        rhat += v.w_[n - 1];
        if (rhat >= b) break;
      }
      // Multiply and subtract.
      int64_t borrow = 0;
      uint64_t carry = 0;
      for (size_t i = 0; i < n; ++i) {
        const uint64_t p = qhat * v.w_[i] + carry;
        carry = p >> 32;
        const int64_t t = int64_t{u.w_[i + k]} - borrow - static_cast<int64_t>(p & 0xffffffffu);
        u.w_[i + k] = static_cast<uint32_t>(t);
        borrow = t < 0 ? 1 : 0;
      }
      const int64_t t = int64_t{u.w_[k + n]} - borrow - static_cast<int64_t>(carry);
      u.w_[k + n] = static_cast<uint32_t>(t);
      if (t < 0) {
        // qhat was one too large: add the divisor back.
        --qhat;
        uint64_t c = 0;
        for (size_t i = 0; i < n; ++i) {
          const uint64_t s = uint64_t{u.w_[i + k]} + v.w_[i] + c;
          u.w_[i + k] = static_cast<uint32_t>(s);
          c = s >> 32;
        }
        u.w_[k + n] = static_cast<uint32_t>(u.w_[k + n] + c);
      }
      q.w_[k] = static_cast<uint32_t>(qhat);
    }
    q.Trim();
    u.Trim();
    return {std::move(q), u.Shr(shift)};
  }

  std::string ToDecimal() const {
    if (IsZero()) return "0";
    Nat n = *this;
    std::string out;
    while (!n.IsZero()) {
      uint32_t chunk = n.DivSmall(1000000000);
      for (int i = 0; i < 9; ++i) {
        out.push_back(static_cast<char>('0' + chunk % 10));
        chunk /= 10;
      }
    }
    while (out.size() > 1 && out.back() == '0') out.pop_back();
    std::reverse(out.begin(), out.end());
    return out;
  }

 private:
  void Trim() {
    while (!w_.empty() && w_.back() == 0) w_.pop_back();
  }

  std::vector<uint32_t> w_;
};

// RoundToBits keeps m's top `bits` bits, round half to even, where sticky
// says the value has more below m. It returns the shift applied, so
// value = *m × 2^shift. A round up that carries leaves bits + 1 bits, which
// is a power of two; the caller normalizes it if it needs exactly `bits`.
uint64_t RoundToBits(Nat* m, uint64_t bits, bool sticky) {
  const uint64_t len = m->BitLen();
  if (len <= bits) return 0;
  const uint64_t shift = len - bits;
  const bool half = m->Bit(shift - 1);
  const bool rest = sticky || m->AnyBitBelow(shift - 1);
  Nat kept = m->Shr(shift);
  if (half && (rest || kept.IsOdd())) kept.AddSmall(1);
  *m = std::move(kept);
  return shift;
}

// Pow2Digits returns the number of decimal digits of 2^p:
// floor(p × log10(2)) + 1, with log10(2) as a 64-bit fraction.
int64_t Pow2Digits(uint32_t p) {
  constexpr uint64_t kLog10Of2 = 0x4d104d427de7fbccULL;  // floor(log10(2) × 2^64)
  const uint64_t high = p * (kLog10Of2 >> 32);
  const uint64_t low = p * (kLog10Of2 & 0xffffffffULL);
  return static_cast<int64_t>((high + (low >> 32)) >> 32) + 1;
}

std::string Clip(std::string_view s) {
  constexpr size_t kMax = 40;
  if (s.size() <= kMax) return std::string(s);
  return std::string(s.substr(0, kMax)) + "...";
}

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

// BinaryExponent is the value's exponent e with value in [2^(e-1), 2^e).
int64_t BinaryExponent(const Nat& mantissa, int64_t exponent) {
  return exponent + static_cast<int64_t>(mantissa.BitLen());
}

bool ExceedsBound(int64_t e, int limit) {
  return e > limit || e < -static_cast<int64_t>(limit);
}

// ParsePrecision reads a suffix's digits. Longer than any uint32 is refused
// on its length, so `1p999…` costs nothing.
bool ParsePrecision(std::string_view digits, uint32_t* out) {
  if (digits.empty() || digits.size() > 10) return false;
  uint64_t v = 0;
  for (char c : digits) {
    if (!IsDigit(c)) return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  if (v == 0 || v > 0xffffffffULL) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

// Literal is a decimal literal taken apart: value = digits × 10^exp10.
struct Literal {
  bool negative = false;
  std::string digits;  // no leading or trailing zero; empty for zero
  int64_t exp10 = 0;
  size_t mantissa_digits = 0;  // as written, for MaxNumericLiteralDigits
};

// SplitLiteral accepts the numeric literal grammar. The exponent saturates:
// a value that far out is refused on its magnitude, not its spelling.
bool SplitLiteral(std::string_view s, Literal* out) {
  size_t i = 0;
  if (i < s.size() && s[i] == '-') {
    out->negative = true;
    ++i;
  }
  const size_t int_start = i;
  while (i < s.size() && IsDigit(s[i])) ++i;
  if (i == int_start) return false;
  std::string digits(s.substr(int_start, i - int_start));
  int64_t exp10 = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    const size_t frac_start = i;
    while (i < s.size() && IsDigit(s[i])) ++i;
    digits.append(s.substr(frac_start, i - frac_start));
    exp10 = -static_cast<int64_t>(i - frac_start);
  }
  out->mantissa_digits = digits.size();
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    const size_t exp_start = i;
    constexpr int64_t kSaturated = int64_t{1} << 60;
    int64_t e = 0;
    for (; i < s.size() && IsDigit(s[i]); ++i) {
      if (e < kSaturated) e = e * 10 + (s[i] - '0');
    }
    if (i == exp_start) return false;
    e = std::min(e, kSaturated);
    exp10 += neg ? -e : e;
  }
  if (i != s.size()) return false;
  const size_t first = digits.find_first_not_of('0');
  if (first == std::string::npos) return true;  // zero
  const size_t last = digits.find_last_not_of('0');
  exp10 += static_cast<int64_t>(digits.size() - 1 - last);
  out->digits = digits.substr(first, last - first + 1);
  out->exp10 = exp10;
  return true;
}

}  // namespace

Status ParseBigFloatLiteral(std::string_view literal,
                            std::string_view prec_digits,
                            int max_digits,
                            int max_exponent,
                            BigFloatFields* out) {
  const bool suffixed = !prec_digits.empty();
  uint32_t prec = kDefaultPrec;
  if (suffixed) {
    if (!ParsePrecision(prec_digits, &prec)) {
      return Status::Error("big float literal " + Clip(literal) + "p" + Clip(prec_digits) +
                           ": precision suffix p" + Clip(prec_digits) +
                           " is not a positive bit count");
    }
    const int64_t n = Pow2Digits(prec) + 1;
    if (n > max_digits) {
      return Status::Error("big float literal " + Clip(literal) + "p" + std::to_string(prec) +
                           ": precision p" + std::to_string(prec) + " renders to " +
                           std::to_string(n) +
                           " digits; MaxNumericLiteralDigits=" + std::to_string(max_digits));
    }
  }
  Literal lit;
  if (!SplitLiteral(literal, &lit)) {
    return Status::Error("invalid big float: " + Clip(literal));
  }
  if (lit.mantissa_digits > static_cast<size_t>(max_digits)) {
    return Status::Error("numeric literal has " + std::to_string(lit.mantissa_digits) +
                         " digits; MaxNumericLiteralDigits=" + std::to_string(max_digits));
  }
  *out = BigFloatFields{};
  out->negative = lit.negative;
  if (lit.digits.empty()) {
    // A zero written without a suffix has no precision to keep.
    out->prec = suffixed ? prec : 0;
    return Status::OK();
  }
  // Refuse what is certainly past the bound before building 10^exp10, whose
  // size is whatever the literal says. The value lies in
  // [10^(point-1), 10^point), and log2(10) > 3, so these two tests only
  // ever fire on values truly past it. The exact check follows the rounding.
  const int64_t point = static_cast<int64_t>(lit.digits.size()) + lit.exp10;
  const int64_t reach = (static_cast<int64_t>(max_exponent) + 8) / 3 + 1;
  if (point - 1 > reach || point < -reach) {
    return Status::Error("big float literal " + Clip(literal) +
                         " is past MaxBigFloatExponent=" + std::to_string(max_exponent));
  }
  Nat num;
  for (char c : lit.digits) {
    num.MulSmall(10);
    num.AddSmall(static_cast<uint32_t>(c - '0'));
  }
  Nat den(1);
  if (lit.exp10 >= 0) {
    num = Nat::Mul(num, Nat::Pow10(lit.exp10));
  } else {
    den = Nat::Pow10(-lit.exp10);
  }
  // Scale so the integer quotient has at least prec + 2 bits; the remainder
  // then only ever acts as a sticky bit.
  const int64_t scale = static_cast<int64_t>(prec) + 2 - static_cast<int64_t>(num.BitLen()) +
                        static_cast<int64_t>(den.BitLen());
  if (scale >= 0) {
    num = num.Shl(static_cast<uint64_t>(scale));
  } else {
    den = den.Shl(static_cast<uint64_t>(-scale));
  }
  auto [q, r] = Nat::DivMod(num, den);
  int64_t exponent = -scale + static_cast<int64_t>(RoundToBits(&q, prec, !r.IsZero()));
  if (q.BitLen() > prec) {  // the round up carried past the top bit
    q = q.Shr(1);
    ++exponent;
  }
  const int64_t bin = BinaryExponent(q, exponent);
  if (ExceedsBound(bin, max_exponent)) {
    return Status::Error(
        "big float literal " + Clip(literal) + " has binary exponent " + std::to_string(bin) +
        ", and MaxBigFloatExponent=" + std::to_string(max_exponent) + " bounds its magnitude");
  }
  out->mantissa = q.ToBytes();
  out->exponent = exponent;
  out->prec = prec;
  return Status::OK();
}

StatusOr<std::string> FormatBigFloatLiteral(const BigFloatFields& f,
                                            int max_digits,
                                            int max_exponent) {
  // A precision of zero is zero whatever the other fields say, and the one
  // BigFloat written without a suffix (protowire#410).
  if (f.prec == 0) return std::string(f.negative ? "-0" : "0");
  const std::string suffix = "p" + std::to_string(f.prec);
  const std::string sign = f.negative ? "-" : "";
  Nat m = Nat::FromBytes(f.mantissa);
  if (m.IsZero()) return sign + "0" + suffix;
  // Rendering costs time that grows with the square of the exponent, so
  // both bounds come before anything is built.
  int64_t exponent = f.exponent;
  const int64_t bin = BinaryExponent(m, exponent);
  if (ExceedsBound(bin, max_exponent)) {
    return Status::Error("pxf.BigFloat binary exponent " + std::to_string(bin) +
                         " exceeds MaxBigFloatExponent=" + std::to_string(max_exponent));
  }
  const int64_t digits = Pow2Digits(f.prec) + 1;
  if (digits > max_digits) {
    return Status::Error("pxf.BigFloat prec " + std::to_string(f.prec) + " bits renders to " +
                         std::to_string(digits) +
                         " digits; MaxNumericLiteralDigits=" + std::to_string(max_digits));
  }
  // A mantissa wider than prec is the value rounded to prec.
  exponent += static_cast<int64_t>(RoundToBits(&m, f.prec, false));

  Nat num = exponent >= 0 ? m.Shl(static_cast<uint64_t>(exponent)) : m;
  Nat den = exponent >= 0 ? Nat(1) : Nat(1).Shl(static_cast<uint64_t>(-exponent));

  // Find x with value in [10^x, 10^(x+1)), then the integer of `digits`
  // digits nearest value × 10^(digits-1-x). The estimate is at most one off.
  int64_t x = (bin - 1) * 30103 / 100000 - ((bin - 1) < 0 ? 1 : 0);
  const Nat low = Nat::Pow10(digits - 1);
  const Nat high = Nat::Pow10(digits);
  Nat q;
  Nat r;
  Nat divisor;
  for (;;) {
    const int64_t p = digits - 1 - x;
    Nat a = p >= 0 ? Nat::Mul(num, Nat::Pow10(p)) : num;
    divisor = p >= 0 ? den : Nat::Mul(den, Nat::Pow10(-p));
    std::tie(q, r) = Nat::DivMod(a, divisor);
    if (Nat::Cmp(q, high) >= 0) {
      ++x;
    } else if (Nat::Cmp(q, low) < 0) {
      --x;
    } else {
      break;
    }
  }
  const int half = Nat::Cmp(r.Shl(1), divisor);
  if (half > 0 || (half == 0 && q.IsOdd())) q.AddSmall(1);
  if (Nat::Cmp(q, high) == 0) {  // 99…9 rounded up to the next power
    q = low;
    ++x;
  }
  std::string d = q.ToDecimal();
  d.erase(d.find_last_not_of('0') + 1);

  // C's %g: positional unless x < -4 or x >= the digit count asked for, and
  // trailing zeros removed.
  std::string out = sign;
  if (x < -4 || x >= digits) {
    out.push_back(d[0]);
    if (d.size() > 1) {
      out.push_back('.');
      out.append(d, 1);
    }
    char e[32];
    std::snprintf(
        e, sizeof(e), "e%c%02lld", x < 0 ? '-' : '+', static_cast<long long>(x < 0 ? -x : x));
    out.append(e);
  } else if (x < 0) {
    out.append("0.");
    out.append(static_cast<size_t>(-x - 1), '0');
    out.append(d);
  } else {
    const size_t whole = static_cast<size_t>(x) + 1;
    if (d.size() <= whole) {
      out.append(d);
      out.append(whole - d.size(), '0');
    } else {
      out.append(d, 0, whole);
      out.push_back('.');
      out.append(d, whole);
    }
  }
  return out + suffix;
}

}  // namespace protowire::pxf

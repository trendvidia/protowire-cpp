// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Schema-free protobuf binary marshaling for native C++ structs.
//
// Field numbers are declared with the PROTOWIRE_FIELDS / PROTOWIRE_FIELD
// macros — this is the C++ analogue of Go's `protowire:"N"` struct tag.
// PROTOWIRE_FIELDS takes the enclosing struct name as its first argument:
//
//   struct Endpoint {
//     std::string path;
//     std::string method;
//     int32_t     port;
//     PROTOWIRE_FIELDS(Endpoint,
//       PROTOWIRE_FIELD(1, path),
//       PROTOWIRE_FIELD(2, method),
//       PROTOWIRE_FIELD(3, port))
//   };
//
//   protowire::StatusOr<std::vector<uint8_t>> data = protowire::pb::Marshal(endpoint);
//   if (!data.ok()) { /* data.status().message() says why */ }
//   protowire::pb::Unmarshal(*data, endpoint);
//
// The output is standard protobuf binary, wire-compatible with any .proto
// definition using the same field numbers. Proto3 semantics: zero-value
// fields are omitted on the wire.
//
// Supported member types:
//   bool, integers (proto3 int32/int64 plain varint by default; PROTOWIRE_ZIGZAG
//   selects sint32/sint64 zigzag), float/double, std::string,
//   std::vector<uint8_t> (bytes), nested struct (must also use the macros),
//   std::vector<T> (repeated; numeric element types packed as proto3 does,
//   other element types one tag+value per element),
//   std::optional<T> (presence — zero values still emitted when set),
//   std::unique_ptr<T> / std::shared_ptr<T> (presence — null skipped on wire),
//   std::map<K,V> / std::unordered_map<K,V> (proto3 maps as repeated MapEntry).
//
// Big numbers (BigInt, Decimal, BigFloat) live in protowire/pb_big.h and are
// encoded as nested messages matching the pxf.BigInt/Decimal/BigFloat schemas.

#pragma once

#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "protowire/check_seam.h"
#include "protowire/detail/status.h"
#include "protowire/detail/stream.h"
#include "protowire/detail/utf8.h"
#include "protowire/detail/wire.h"
#include "protowire/limits.h"
#include "protowire/pb_big.h"

namespace protowire::pb {

// UnmarshalOptions carries the HARDENING.md § Mandatory limits a decode
// call enforces, each configurable per call; 0 selects the default in
// protowire/limits.h. max_message_size caps the input to this call and is
// checked before anything is read; max_nesting_depth caps submessage /
// map-entry nesting (the root is depth 0, every submessage, big-number
// message or map entry one descent); max_repeated_count caps the element
// count of any repeated or map field, checked before each element is
// appended.
struct UnmarshalOptions {
  int max_message_size = 0;
  int max_nesting_depth = 0;
  // Bounds the magnitude of pxf.Decimal.scale on the wire (a digit count), and
  // the digits a pxf.BigFloat's prec implies: prec 13603 at the default.
  int max_numeric_literal_digits = 0;
  // Bounds the magnitude of a pxf.BigFloat value's binary exponent: exponent
  // plus the mantissa's bit length.
  int max_big_float_exponent = 0;
  int max_repeated_count = 0;
  // Runs after a successful decode, on the decoded struct, and on each frame of
  // a Decoder. Derive from check::StructValidator<T>, or override
  // check::Validator::ValidateStruct. Not owned; may be null.
  check::Validator* validator = nullptr;
  // Receives the validator's report, since a Status has only text. May be null.
  check::Report* report = nullptr;
};

// MarshalOptions configures Marshal and an Encoder. The zero value is
// Marshal's default.
struct MarshalOptions {
  // The deepest value Marshal writes, counted as Unmarshal counts it: the root
  // is depth 0, and every submessage, big-number message or map entry is one
  // descent. 0 selects kMaxNestingDepth. Raise it only with
  // UnmarshalOptions::max_nesting_depth on every reader (HARDENING.md §
  // Recursion, protowire#502).
  int max_nesting_depth = 0;
};

// ---- Field descriptor ---------------------------------------------------

// clang-format off
// Pointer-to-member declarations: clang-format's PointerAlignment
// settings (Left/Right) handle plain pointer types but produce
// inconsistent results for `Member Class::* ptr` across versions.
// Format these by hand and skip the formatter on this block.
template <class Class, class Member>
struct FieldDef {
  uint32_t number;
  Member Class::*ptr;
  bool zigzag = false;  // signed ints: zigzag (sint32/sint64) vs plain varint (int32/int64)
  const char* name = "";  // the member, for error text
};

template <uint32_t N, class Class, class Member>
constexpr FieldDef<Class, Member> MakeField(Member Class::*ptr, const char* name) {
  return {N, ptr, false, name};
}

template <uint32_t N, class Class, class Member>
constexpr FieldDef<Class, Member> MakeFieldZigzag(Member Class::*ptr, const char* name) {
  return {N, ptr, true, name};
}
// clang-format on

// ---- Type traits --------------------------------------------------------

template <class T>
struct IsVector : std::false_type {};
template <class T, class A>
struct IsVector<std::vector<T, A>> : std::true_type {
  using element = T;
};

template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {
  using element = T;
};

template <class T>
struct IsSmartPtr : std::false_type {};
template <class T, class D>
struct IsSmartPtr<std::unique_ptr<T, D>> : std::true_type {
  using element = T;
};
template <class T>
struct IsSmartPtr<std::shared_ptr<T>> : std::true_type {
  using element = T;
};

template <class T>
struct IsMap : std::false_type {};
template <class K, class V, class C, class A>
struct IsMap<std::map<K, V, C, A>> : std::true_type {
  using key_type = K;
  using mapped_type = V;
};
template <class K, class V, class H, class E, class A>
struct IsMap<std::unordered_map<K, V, H, E, A>> : std::true_type {
  using key_type = K;
  using mapped_type = V;
};

// Detect a struct that has registered protowire fields — i.e. one that
// defines a `static constexpr auto _protowire_fields()` accessor via the
// PROTOWIRE_FIELDS macro.
template <class T, class = void>
struct HasFields : std::false_type {};
template <class T>
struct HasFields<T, std::void_t<decltype(T::_protowire_fields())>> : std::true_type {};

// Big-number tag (specialized below).
template <class T>
struct IsBigNumber : std::false_type {};
template <>
struct IsBigNumber<BigInt> : std::true_type {};
template <>
struct IsBigNumber<Decimal> : std::true_type {};
template <>
struct IsBigNumber<BigFloat> : std::true_type {};

// ---- Forward declarations ----------------------------------------------

// Marshal refuses what a decoder at the default limits would refuse, rather
// than write it: a value nested past max_nesting_depth, a Decimal scale past
// MaxNumericLiteralDigits, and a BigFloat whose prec or binary exponent is past
// its bound. protowire-go's Marshal refuses the same values.
template <class T>
StatusOr<std::vector<uint8_t>> Marshal(const T& v, MarshalOptions opts = {});

template <class T>
Status Unmarshal(std::span<const uint8_t> data, T& v, UnmarshalOptions opts = {});

namespace detail {

// Limits is UnmarshalOptions with the defaults applied, threaded through
// every nested decode together with the current depth so a fresh inner
// span cannot reset the recursion counter (HARDENING.md § Recursion).
struct Limits {
  int max_message_size = kMaxMessageSize;
  int max_depth = kMaxNestingDepth;
  int max_digits = kMaxNumericLiteralDigits;
  int max_big_float_exp = kMaxBigFloatExponent;
  int max_repeated = kMaxRepeatedCount;
};

inline Limits ResolveLimits(const UnmarshalOptions& o) {
  Limits lim;
  if (o.max_message_size > 0) lim.max_message_size = o.max_message_size;
  if (o.max_nesting_depth > 0) lim.max_depth = o.max_nesting_depth;
  if (o.max_numeric_literal_digits > 0) lim.max_digits = o.max_numeric_literal_digits;
  if (o.max_big_float_exponent > 0) lim.max_big_float_exp = o.max_big_float_exponent;
  if (o.max_repeated_count > 0) lim.max_repeated = o.max_repeated_count;
  return lim;
}

inline Status DepthError(int max_depth) {
  return Status::Error("nesting depth exceeds MaxNestingDepth=" + std::to_string(max_depth));
}

inline Status DepthError(const Limits& lim) {
  return DepthError(lim.max_depth);
}

// MarshalStruct writes v at the given depth, counted as UnmarshalStruct
// counts it, so nothing it writes is refused for depth when read back.
template <class T>
Status MarshalStruct(const T& v, std::vector<uint8_t>& out, int depth, int max_depth);

template <class T>
Status UnmarshalStruct(std::span<const uint8_t> data, T& v, int depth, const Limits& lim);

template <class T>
Status UnmarshalField(std::span<const uint8_t> data,
                      uint32_t num,
                      wire::WireType type,
                      T& v,
                      int& consumed,
                      bool zigzag,
                      int depth,
                      const Limits& lim);

// ---- Big-number helpers (defined in big.cc) ------------------------------

void MarshalBigIntMsg(const BigInt& v, std::vector<uint8_t>& out);
// Refuses a scale past MaxNumericLiteralDigits once reduced.
Status MarshalDecimalMsg(const Decimal& v, std::vector<uint8_t>& out);
// Refuses a prec or a binary exponent past its bound, as the decoder does.
Status MarshalBigFloatMsg(const BigFloat& v, std::vector<uint8_t>& out);
Status UnmarshalBigIntMsg(std::span<const uint8_t> data, BigInt& out);
// max_digits bounds |Decimal.scale| (HARDENING.md MaxNumericLiteralDigits);
// 0 selects the default.
Status UnmarshalDecimalMsg(std::span<const uint8_t> data, Decimal& out, int max_digits);
// max_exponent bounds the value's binary exponent (HARDENING.md
// MaxBigFloatExponent) and max_digits the digits its prec implies
// (MaxNumericLiteralDigits, protowire#499); 0 selects either default.
Status UnmarshalBigFloatMsg(std::span<const uint8_t> data,
                            BigFloat& out,
                            int max_exponent,
                            int max_digits);

// ---- Marshal dispatch ----------------------------------------------------

template <class T>
inline bool IsZeroValue(const T& v) {
  if constexpr (std::is_same_v<T, bool>) {
    return !v;
  } else if constexpr (std::is_arithmetic_v<T>) {
    return v == T{};
  } else if constexpr (std::is_same_v<T, std::string>) {
    return v.empty();
  } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
    return v.empty();
  } else if constexpr (IsVector<T>::value) {
    return v.empty();
  } else if constexpr (std::is_same_v<T, BigFloat>) {
    // Only +0 at prec 0 is the empty message: -0 there is {negative: true}.
    return v.prec == 0 && !v.negative;
  } else if constexpr (IsBigNumber<T>::value) {
    return v.IsZero();
  } else {
    return false;  // structs always emitted
  }
}

// DoubleBits and FloatBits are the bits Marshal writes for a float field. A
// struct field is a language value, so a NaN is written as the canonical
// quiet NaN whatever its payload (draft -01 § Numeric Literals,
// protowire#512, protowire-go#241).
inline uint64_t DoubleBits(double v) {
  if (v != v) return 0x7FF8000000000000ULL;
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  return bits;
}

inline uint32_t FloatBits(float v) {
  if (v != v) return 0x7FC00000U;
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  return bits;
}

// IsPackable: proto3 encodes a repeated field of these element types
// packed by default — one LEN record carrying the concatenated element
// encodings. The marshaller writes that form and the decoder accepts both
// it and the one-record-per-element form.
template <class T>
inline constexpr bool IsPackable = std::is_arithmetic_v<T>;

// AppendPackedElement writes one element of a packed repeated field: the
// value encoding alone, with no tag.
template <class T>
inline void AppendPackedElement(std::vector<uint8_t>& payload, T v, bool zigzag) {
  if constexpr (std::is_same_v<T, bool>) {
    wire::AppendVarint(payload, v ? 1 : 0);
  } else if constexpr (std::is_same_v<T, double>) {
    wire::AppendFixed64(payload, DoubleBits(v));
  } else if constexpr (std::is_same_v<T, float>) {
    wire::AppendFixed32(payload, FloatBits(v));
  } else if constexpr (std::is_signed_v<T>) {
    if (zigzag) {
      wire::AppendVarint(payload, wire::EncodeZigZag(static_cast<int64_t>(v)));
    } else {
      wire::AppendVarint(payload, static_cast<uint64_t>(static_cast<int64_t>(v)));
    }
  } else {
    wire::AppendVarint(payload, static_cast<uint64_t>(v));
  }
}

template <class T>
inline Status MarshalScalar(
    std::vector<uint8_t>& out, uint32_t num, const T& v, bool zigzag, int depth, int max_depth) {
  if constexpr (std::is_same_v<T, bool>) {
    wire::AppendTag(out, num, wire::kVarint);
    wire::AppendVarint(out, v ? 1 : 0);
  } else if constexpr (std::is_same_v<T, double>) {
    wire::AppendTag(out, num, wire::kFixed64);
    wire::AppendFixed64(out, DoubleBits(v));
  } else if constexpr (std::is_same_v<T, float>) {
    wire::AppendTag(out, num, wire::kFixed32);
    wire::AppendFixed32(out, FloatBits(v));
  } else if constexpr (std::is_signed_v<T>) {
    wire::AppendTag(out, num, wire::kVarint);
    if (zigzag) {
      wire::AppendVarint(out, wire::EncodeZigZag(static_cast<int64_t>(v)));
    } else {
      // proto3 int32/int64: plain varint; negatives sign-extend to uint64.
      wire::AppendVarint(out, static_cast<uint64_t>(static_cast<int64_t>(v)));
    }
  } else if constexpr (std::is_unsigned_v<T>) {
    wire::AppendTag(out, num, wire::kVarint);
    wire::AppendVarint(out, static_cast<uint64_t>(v));
  } else if constexpr (std::is_same_v<T, std::string>) {
    wire::AppendTag(out, num, wire::kBytes);
    wire::AppendBytes(out, v);
  } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
    wire::AppendTag(out, num, wire::kBytes);
    wire::AppendBytes(out, v);
  } else if constexpr (IsBigNumber<T>::value) {
    // A big-number message is a descent, as UnmarshalScalar counts it.
    if (depth + 1 > max_depth) return DepthError(max_depth);
    std::vector<uint8_t> sub;
    if constexpr (std::is_same_v<T, BigInt>) {
      MarshalBigIntMsg(v, sub);
    } else if constexpr (std::is_same_v<T, Decimal>) {
      if (Status st = MarshalDecimalMsg(v, sub); !st.ok()) return st;
    } else {
      if (Status st = MarshalBigFloatMsg(v, sub); !st.ok()) return st;
    }
    wire::AppendTag(out, num, wire::kBytes);
    wire::AppendBytes(out, sub);
  } else if constexpr (HasFields<T>::value) {
    std::vector<uint8_t> sub;
    if (Status st = MarshalStruct(v, sub, depth + 1, max_depth); !st.ok()) return st;
    wire::AppendTag(out, num, wire::kBytes);
    wire::AppendBytes(out, sub);
  } else {
    static_assert(sizeof(T) == 0, "protowire::pb: unsupported field type");
  }
  return Status::OK();
}

// MarshalMapValue writes field 2 of a map entry unconditionally. A value
// held through std::optional or a smart pointer that is unset is written
// as its zero value, so every entry carries both fields on the wire.
template <class T>
inline Status MarshalMapValue(
    std::vector<uint8_t>& out, uint32_t num, const T& v, bool zigzag, int depth, int max_depth) {
  if constexpr (IsOptional<T>::value) {
    using E = typename IsOptional<T>::element;
    return MarshalScalar(out, num, v.has_value() ? *v : E{}, zigzag, depth, max_depth);
  } else if constexpr (IsSmartPtr<T>::value) {
    using E = typename IsSmartPtr<T>::element;
    if (v) return MarshalScalar(out, num, *v, zigzag, depth, max_depth);
    return MarshalScalar(out, num, E{}, zigzag, depth, max_depth);
  } else {
    return MarshalScalar(out, num, v, zigzag, depth, max_depth);
  }
}

template <class T>
inline Status MarshalField(
    std::vector<uint8_t>& out, uint32_t num, const T& v, bool zigzag, int depth, int max_depth) {
  if constexpr (IsOptional<T>::value) {
    if (v.has_value()) return MarshalScalar(out, num, *v, zigzag, depth, max_depth);
    return Status::OK();
  } else if constexpr (IsSmartPtr<T>::value) {
    if (v) return MarshalScalar(out, num, *v, zigzag, depth, max_depth);
    return Status::OK();
  } else if constexpr (IsMap<T>::value) {
    // proto3 maps: each entry is a length-prefixed MapEntry message with
    // key at field 1 and value at field 2. Both are written whether or
    // not they are zero-valued — the layout protobuf-go, protoc and C++
    // protobuf write, fixed across the family by protowire#295 (#24): a
    // zero key or value is a present field of the entry, not an absent
    // one, so proto3 zero-skip does not apply inside it.
    if (v.empty()) return Status::OK();
    // A map entry is a descent whatever its value's type (protowire#449).
    if (depth + 1 > max_depth) return DepthError(max_depth);
    for (const auto& [k, val] : v) {
      std::vector<uint8_t> entry;
      if (Status st = MarshalScalar(entry, 1, k, zigzag, depth + 1, max_depth); !st.ok()) {
        return Status::Error("map key: " + st.message());
      }
      if (Status st = MarshalMapValue(entry, 2, val, zigzag, depth + 1, max_depth); !st.ok()) {
        return Status::Error("map value: " + st.message());
      }
      wire::AppendTag(out, num, wire::kBytes);
      wire::AppendBytes(out, entry);
    }
    return Status::OK();
  } else if constexpr (IsVector<T>::value && !std::is_same_v<T, std::vector<uint8_t>>) {
    using E = typename IsVector<T>::element;
    if (v.empty()) return Status::OK();
    if constexpr (IsPackable<E>) {
      // Repeated numeric scalars: packed, the proto3 default and what
      // every other encoder in the family writes — one LEN record
      // carrying the concatenated element encodings. Packed encoding has
      // no per-element presence, so every element is emitted, zeros
      // included (#32).
      std::vector<uint8_t> payload;
      for (const auto& el : v) AppendPackedElement(payload, static_cast<E>(el), zigzag);
      wire::AppendTag(out, num, wire::kBytes);
      wire::AppendBytes(out, payload);
    } else {
      for (const auto& el : v) {
        if (Status st = MarshalScalar(out, num, el, zigzag, depth, max_depth); !st.ok()) return st;
      }
    }
    return Status::OK();
  } else {
    if (IsZeroValue(v)) return Status::OK();
    return MarshalScalar(out, num, v, zigzag, depth, max_depth);
  }
}

template <class T>
inline Status MarshalStruct(const T& v, std::vector<uint8_t>& out, int depth, int max_depth) {
  if (depth > max_depth) return DepthError(max_depth);
  Status st;
  // Stops at the first field that fails, naming it as Unmarshal does.
  const auto one = [&](const auto& f) {
    Status fst = MarshalField(out, f.number, v.*(f.ptr), f.zigzag, depth, max_depth);
    if (fst.ok()) return true;
    st = Status::Error(std::string("field ") + f.name + ": " + fst.message());
    return false;
  };
  std::apply([&](const auto&... f) { (one(f) && ...); }, T::_protowire_fields());
  return st;
}

// ---- Unmarshal dispatch -------------------------------------------------

template <class T>
inline Status UnmarshalScalar(std::span<const uint8_t> data,
                              wire::WireType type,
                              T& v,
                              int& consumed,
                              bool zigzag,
                              int depth,
                              const Limits& lim) {
  if constexpr (std::is_same_v<T, bool>) {
    uint64_t x;
    int n = wire::ConsumeVarint(data, x);
    if (n < 0) return Status::Error("corrupt bool");
    consumed = n;
    v = (x != 0);
  } else if constexpr (std::is_same_v<T, double>) {
    uint64_t bits;
    int n = wire::ConsumeFixed64(data, bits);
    if (n < 0) return Status::Error("corrupt float64");
    consumed = n;
    std::memcpy(&v, &bits, 8);
  } else if constexpr (std::is_same_v<T, float>) {
    uint32_t bits;
    int n = wire::ConsumeFixed32(data, bits);
    if (n < 0) return Status::Error("corrupt float32");
    consumed = n;
    std::memcpy(&v, &bits, 4);
  } else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>) {
    uint64_t x;
    int n = wire::ConsumeVarint(data, x);
    if (n < 0) return Status::Error("corrupt int");
    consumed = n;
    if (zigzag) {
      v = static_cast<T>(wire::DecodeZigZag(x));
    } else {
      // proto3 int32/int64: low bits as signed.
      v = static_cast<T>(static_cast<int64_t>(x));
    }
  } else if constexpr (std::is_unsigned_v<T> && std::is_integral_v<T>) {
    uint64_t x;
    int n = wire::ConsumeVarint(data, x);
    if (n < 0) return Status::Error("corrupt uint");
    consumed = n;
    v = static_cast<T>(x);
  } else if constexpr (std::is_same_v<T, std::string>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt string");
    consumed = n;
    // HARDENING.md § UTF-8. Every string reaches this case: a singular field, a
    // list element, a map key and a map value. Declare a proto bytes field as
    // std::vector<uint8_t>.
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!::protowire::detail::IsValidUTF8(text)) return Status::Error("invalid UTF-8 in string");
    v.assign(text);
  } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt bytes");
    consumed = n;
    v.assign(bytes.begin(), bytes.end());
  } else if constexpr (std::is_same_v<T, BigInt>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt embedded message");
    consumed = n;
    if (depth + 1 > lim.max_depth) return DepthError(lim);
    return UnmarshalBigIntMsg(bytes, v);
  } else if constexpr (std::is_same_v<T, Decimal>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt embedded message");
    consumed = n;
    if (depth + 1 > lim.max_depth) return DepthError(lim);
    return UnmarshalDecimalMsg(bytes, v, lim.max_digits);
  } else if constexpr (std::is_same_v<T, BigFloat>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt embedded message");
    consumed = n;
    if (depth + 1 > lim.max_depth) return DepthError(lim);
    return UnmarshalBigFloatMsg(bytes, v, lim.max_big_float_exp, lim.max_digits);
  } else if constexpr (HasFields<T>::value) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt embedded message");
    consumed = n;
    // The submessage is decoded from a fresh span; the counter goes with
    // it rather than restarting at zero.
    return UnmarshalStruct(bytes, v, depth + 1, lim);
  } else {
    static_assert(sizeof(T) == 0, "protowire::pb: unsupported field type");
  }
  return Status::OK();
}

template <class T>
inline Status UnmarshalField(std::span<const uint8_t> data,
                             uint32_t num,
                             wire::WireType type,
                             T& v,
                             int& consumed,
                             bool zigzag,
                             int depth,
                             const Limits& lim) {
  if constexpr (IsOptional<T>::value) {
    using E = typename IsOptional<T>::element;
    E tmp{};
    Status st = UnmarshalScalar(data, type, tmp, consumed, zigzag, depth, lim);
    if (!st.ok()) return st;
    v = std::move(tmp);
    return Status::OK();
  } else if constexpr (IsSmartPtr<T>::value) {
    using E = typename IsSmartPtr<T>::element;
    if (!v) v.reset(new E());
    return UnmarshalScalar(data, type, *v, consumed, zigzag, depth, lim);
  } else if constexpr (IsMap<T>::value) {
    using K = typename IsMap<T>::key_type;
    using V = typename IsMap<T>::mapped_type;
    std::span<const uint8_t> entry_bytes;
    int n = wire::ConsumeBytes(data, entry_bytes);
    if (n < 0) return Status::Error("corrupt map entry");
    consumed = n;
    // A map entry is a submessage: one descent (HARDENING.md § Recursion).
    if (depth + 1 > lim.max_depth) return DepthError(lim);
    K key{};
    V val{};
    while (!entry_bytes.empty()) {
      wire::FieldNumber enum_num;
      wire::WireType etyp;
      int en = wire::ConsumeTag(entry_bytes, enum_num, etyp);
      if (en < 0) return Status::Error("corrupt tag in map entry");
      entry_bytes = entry_bytes.subspan(en);
      int sub_consumed = 0;
      if (enum_num == 1) {
        Status st = UnmarshalField(entry_bytes, 1, etyp, key, sub_consumed, zigzag, depth + 1, lim);
        if (!st.ok()) return Status::Error("map key: " + st.message());
        entry_bytes = entry_bytes.subspan(sub_consumed);
      } else if (enum_num == 2) {
        Status st = UnmarshalField(entry_bytes, 2, etyp, val, sub_consumed, zigzag, depth + 1, lim);
        if (!st.ok()) return Status::Error("map value: " + st.message());
        entry_bytes = entry_bytes.subspan(sub_consumed);
      } else {
        int skipped = wire::ConsumeFieldValue(entry_bytes, enum_num, etyp);
        if (skipped < 0) {
          return Status::Error("corrupt unknown field " + std::to_string(enum_num) +
                               " in map entry");
        }
        entry_bytes = entry_bytes.subspan(skipped);
      }
    }
    if (static_cast<int>(v.size()) >= lim.max_repeated && v.find(key) == v.end()) {
      return Status::Error("map field exceeds MaxRepeatedCount=" +
                           std::to_string(lim.max_repeated));
    }
    v.insert_or_assign(std::move(key), std::move(val));
    return Status::OK();
  } else if constexpr (IsVector<T>::value && !std::is_same_v<T, std::vector<uint8_t>>) {
    using E = typename IsVector<T>::element;
    if constexpr (IsPackable<E>) {
      if (type == wire::kBytes) {
        // Packed: a LEN record of concatenated elements, each read with
        // its natural encoding. The count is checked before each append.
        std::span<const uint8_t> payload;
        int n = wire::ConsumeBytes(data, payload);
        if (n < 0) return Status::Error("corrupt packed field");
        consumed = n;
        while (!payload.empty()) {
          E tmp{};
          int sub_consumed = 0;
          Status st = UnmarshalScalar(payload, type, tmp, sub_consumed, zigzag, depth, lim);
          if (!st.ok()) return st;
          payload = payload.subspan(sub_consumed);
          if (static_cast<int>(v.size()) >= lim.max_repeated) {
            return Status::Error("repeated field exceeds MaxRepeatedCount=" +
                                 std::to_string(lim.max_repeated));
          }
          v.push_back(tmp);
        }
        return Status::OK();
      }
    }
    E tmp{};
    Status st = UnmarshalScalar(data, type, tmp, consumed, zigzag, depth, lim);
    if (!st.ok()) return st;
    if (static_cast<int>(v.size()) >= lim.max_repeated) {
      return Status::Error("repeated field exceeds MaxRepeatedCount=" +
                           std::to_string(lim.max_repeated));
    }
    v.push_back(std::move(tmp));
    return Status::OK();
  } else {
    return UnmarshalScalar(data, type, v, consumed, zigzag, depth, lim);
  }
}

// UnmarshalStruct decodes data into v. depth is the current submessage
// depth — the root is 0, per HARDENING.md § Recursion, so a message exactly
// max_depth submessages deep is accepted and one deeper is not.
template <class T>
inline Status UnmarshalStruct(std::span<const uint8_t> data, T& v, int depth, const Limits& lim) {
  if (depth > lim.max_depth) return DepthError(lim);
  while (!data.empty()) {
    wire::FieldNumber num;
    wire::WireType type;
    int n = wire::ConsumeTag(data, num, type);
    if (n < 0) return Status::Error("corrupt tag");
    data = data.subspan(n);

    const char* matched = nullptr;
    Status st = Status::OK();
    int consumed = 0;
    std::apply(
        [&](auto&&... f) {
          (((!matched && f.number == num)
                ? (matched = f.name,
                   st = UnmarshalField(data, num, type, v.*(f.ptr), consumed, f.zigzag, depth, lim),
                   0)
                : 0),
           ...);
        },
        T::_protowire_fields());

    if (matched) {
      if (!st.ok()) return Status::Error(std::string("field ") + matched + ": " + st.message());
      data = data.subspan(consumed);
    } else {
      int skipped = wire::ConsumeFieldValue(data, num, type);
      if (skipped < 0) return Status::Error("corrupt field " + std::to_string(num));
      data = data.subspan(skipped);
    }
  }
  return Status::OK();
}

}  // namespace detail

// ---- Public API ---------------------------------------------------------

template <class T>
StatusOr<std::vector<uint8_t>> Marshal(const T& v, MarshalOptions opts) {
  static_assert(HasFields<T>::value, "Marshal: type must declare PROTOWIRE_FIELDS(...)");
  const int max_depth = opts.max_nesting_depth > 0 ? opts.max_nesting_depth : kMaxNestingDepth;
  std::vector<uint8_t> out;
  if (Status st = detail::MarshalStruct(v, out, /*depth=*/0, max_depth); !st.ok()) return st;
  return out;
}

template <class T>
Status Unmarshal(std::span<const uint8_t> data, T& v, UnmarshalOptions opts) {
  static_assert(HasFields<T>::value, "Unmarshal: type must declare PROTOWIRE_FIELDS(...)");
  detail::Limits lim = detail::ResolveLimits(opts);
  // MaxMessageSize is checked before anything is read.
  if (data.size() > static_cast<size_t>(lim.max_message_size)) {
    return Status::Error("input of " + std::to_string(data.size()) +
                         " bytes exceeds MaxMessageSize=" + std::to_string(lim.max_message_size));
  }
  if (Status st = detail::UnmarshalStruct(data, v, /*depth=*/0, lim); !st.ok()) return st;
  return check::ValidateStruct(opts.validator, v, opts.report);
}

template <class T>
Status Unmarshal(const std::vector<uint8_t>& data, T& v, UnmarshalOptions opts = {}) {
  return Unmarshal(std::span<const uint8_t>(data.data(), data.size()), v, opts);
}

// ---- Streaming ----------------------------------------------------------

// kDefaultMaxFrameSize caps each frame at 16 MiB by default. Streams face
// per-connection adversary risk; callers can adjust with SetMaxFrameSize. It is
// below kMaxMessageSize, which still bounds a frame's payload through the
// Decoder's UnmarshalOptions when a caller raises the frame cap.
inline constexpr int kDefaultMaxFrameSize = 16 << 20;

// kErrFramingCorrupt opens the error of invalid length-prefix framing: a
// corrupt varint, a frame larger than the maximum. The byte stream is then
// desynchronized and the decoder is permanently unusable. IsFramingCorrupt tests
// for it.
inline constexpr const char* kErrFramingCorrupt = "pb: stream framing corrupt";
bool IsFramingCorrupt(const Status& st);

// Encoder writes length-prefixed pb-encoded messages using the
// protobuf-delimited framing convention (varint length + body), each under
// opts. w must outlive the encoder. A message Marshal refuses writes nothing.
class Encoder {
 public:
  explicit Encoder(std::ostream* w, MarshalOptions opts = {}) : w_(w), opts_(opts) {}

  template <class T>
  Status Encode(const T& v) {
    StatusOr<std::vector<uint8_t>> body = Marshal(v, opts_);
    if (!body.ok()) return body.status();
    return WriteFrame(*body);
  }

 private:
  Status WriteFrame(std::span<const uint8_t> body);

  std::ostream* w_;
  MarshalOptions opts_;
};

// Decoder reads length-prefixed pb-encoded messages, each under opts. r must
// outlive the decoder.
//
// Decode returns a Status for which IsEOF is true at the end of the stream, and
// IsUnexpectedEOF when it ends inside a frame. A framing error is sticky: every
// later call returns it. A frame whose body does not decode is not, since the
// next frame is at a known boundary.
class Decoder {
 public:
  explicit Decoder(std::istream* r, UnmarshalOptions opts = {}) : r_(r), opts_(opts) {}

  void SetMaxFrameSize(int64_t n) { max_ = n; }

  template <class T>
  Status Decode(T& v) {
    if (Status st = ReadFrame(); !st.ok()) return st;
    return Unmarshal(std::span<const uint8_t>(buf_.data(), buf_.size()), v, opts_);
  }

 private:
  Status ReadFrame();

  std::istream* r_;
  UnmarshalOptions opts_;
  std::vector<uint8_t> buf_;  // reused; Unmarshal copies what it keeps
  int64_t max_ = kDefaultMaxFrameSize;
  Status err_;
};

}  // namespace protowire::pb

// ---- Macros -------------------------------------------------------------
//
// PROTOWIRE_FIELDS(Type, ...) declares the list of (number, member-pointer)
// pairs for a struct. PROTOWIRE_FIELD(N, member) wraps one entry. Place
// inside the struct definition, after all member declarations.

#define PROTOWIRE_FIELD(N, MEMBER) \
  ::protowire::pb::MakeField<(N)>(&_protowire_self_t::MEMBER, #MEMBER)

// Like PROTOWIRE_FIELD, but encodes signed integers with zigzag varint
// (proto3 sint32 / sint64). More compact than plain int32 / int64 for
// negative values.
#define PROTOWIRE_ZIGZAG(N, MEMBER) \
  ::protowire::pb::MakeFieldZigzag<(N)>(&_protowire_self_t::MEMBER, #MEMBER)

#define PROTOWIRE_FIELDS(SELF, ...)           \
  using _protowire_self_t = SELF;             \
  static constexpr auto _protowire_fields() { \
    return std::make_tuple(__VA_ARGS__);      \
  }

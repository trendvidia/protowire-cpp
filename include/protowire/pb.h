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
//   std::vector<uint8_t> data = protowire::pb::Marshal(endpoint);
//   protowire::pb::Unmarshal(data, endpoint);
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

#include "protowire/detail/status.h"
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
  // Bounds the magnitude of pxf.Decimal.scale on the wire (a digit count).
  int max_numeric_literal_digits = 0;
  int max_repeated_count = 0;
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
};

template <uint32_t N, class Class, class Member>
constexpr FieldDef<Class, Member> MakeField(Member Class::*ptr) {
  return {N, ptr, false};
}

template <uint32_t N, class Class, class Member>
constexpr FieldDef<Class, Member> MakeFieldZigzag(Member Class::*ptr) {
  return {N, ptr, true};
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

template <class T>
std::vector<uint8_t> Marshal(const T& v);

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
  int max_repeated = kMaxRepeatedCount;
};

inline Limits ResolveLimits(const UnmarshalOptions& o) {
  Limits lim;
  if (o.max_message_size > 0) lim.max_message_size = o.max_message_size;
  if (o.max_nesting_depth > 0) lim.max_depth = o.max_nesting_depth;
  if (o.max_numeric_literal_digits > 0) lim.max_digits = o.max_numeric_literal_digits;
  if (o.max_repeated_count > 0) lim.max_repeated = o.max_repeated_count;
  return lim;
}

inline Status DepthError(const Limits& lim) {
  return Status::Error("nesting depth exceeds MaxNestingDepth=" + std::to_string(lim.max_depth));
}

template <class T>
void MarshalStruct(const T& v, std::vector<uint8_t>& out);

template <class T>
Status UnmarshalStruct(std::span<const uint8_t> data, T& v, int depth, const Limits& lim);

template <class T>
void MarshalField(std::vector<uint8_t>& out, uint32_t num, const T& v);

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
void MarshalDecimalMsg(const Decimal& v, std::vector<uint8_t>& out);
void MarshalBigFloatMsg(const BigFloat& v, std::vector<uint8_t>& out);
Status UnmarshalBigIntMsg(std::span<const uint8_t> data, BigInt& out);
// max_digits bounds |Decimal.scale| (HARDENING.md MaxNumericLiteralDigits);
// 0 selects the default.
Status UnmarshalDecimalMsg(std::span<const uint8_t> data, Decimal& out, int max_digits);
Status UnmarshalBigFloatMsg(std::span<const uint8_t> data, BigFloat& out);

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
  } else if constexpr (IsBigNumber<T>::value) {
    return v.IsZero();
  } else {
    return false;  // structs always emitted
  }
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
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    wire::AppendFixed64(payload, bits);
  } else if constexpr (std::is_same_v<T, float>) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    wire::AppendFixed32(payload, bits);
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
inline void MarshalScalar(std::vector<uint8_t>& out,
                          uint32_t num,
                          const T& v,
                          bool zigzag = false) {
  if constexpr (std::is_same_v<T, bool>) {
    wire::AppendTag(out, num, wire::kVarint);
    wire::AppendVarint(out, v ? 1 : 0);
  } else if constexpr (std::is_same_v<T, double>) {
    wire::AppendTag(out, num, wire::kFixed64);
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    wire::AppendFixed64(out, bits);
  } else if constexpr (std::is_same_v<T, float>) {
    wire::AppendTag(out, num, wire::kFixed32);
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    wire::AppendFixed32(out, bits);
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
  } else if constexpr (std::is_same_v<T, BigInt>) {
    std::vector<uint8_t> sub;
    MarshalBigIntMsg(v, sub);
    wire::AppendTag(out, num, wire::kBytes);
    wire::AppendBytes(out, sub);
  } else if constexpr (std::is_same_v<T, Decimal>) {
    std::vector<uint8_t> sub;
    MarshalDecimalMsg(v, sub);
    wire::AppendTag(out, num, wire::kBytes);
    wire::AppendBytes(out, sub);
  } else if constexpr (std::is_same_v<T, BigFloat>) {
    std::vector<uint8_t> sub;
    MarshalBigFloatMsg(v, sub);
    wire::AppendTag(out, num, wire::kBytes);
    wire::AppendBytes(out, sub);
  } else if constexpr (HasFields<T>::value) {
    std::vector<uint8_t> sub;
    MarshalStruct(v, sub);
    wire::AppendTag(out, num, wire::kBytes);
    wire::AppendBytes(out, sub);
  } else {
    static_assert(sizeof(T) == 0, "protowire::pb: unsupported field type");
  }
}

// MarshalMapValue writes field 2 of a map entry unconditionally. A value
// held through std::optional or a smart pointer that is unset is written
// as its zero value, so every entry carries both fields on the wire.
template <class T>
inline void MarshalMapValue(std::vector<uint8_t>& out,
                            uint32_t num,
                            const T& v,
                            bool zigzag = false) {
  if constexpr (IsOptional<T>::value) {
    using E = typename IsOptional<T>::element;
    MarshalScalar(out, num, v.has_value() ? *v : E{}, zigzag);
  } else if constexpr (IsSmartPtr<T>::value) {
    using E = typename IsSmartPtr<T>::element;
    if (v) {
      MarshalScalar(out, num, *v, zigzag);
    } else {
      MarshalScalar(out, num, E{}, zigzag);
    }
  } else {
    MarshalScalar(out, num, v, zigzag);
  }
}

template <class T>
inline void MarshalField(std::vector<uint8_t>& out, uint32_t num, const T& v, bool zigzag = false) {
  if constexpr (IsOptional<T>::value) {
    if (v.has_value()) MarshalScalar(out, num, *v, zigzag);
    return;
  } else if constexpr (IsSmartPtr<T>::value) {
    if (v) MarshalScalar(out, num, *v, zigzag);
    return;
  } else if constexpr (IsMap<T>::value) {
    // proto3 maps: each entry is a length-prefixed MapEntry message with
    // key at field 1 and value at field 2. Both are written whether or
    // not they are zero-valued — the layout protobuf-go, protoc and C++
    // protobuf write, fixed across the family by protowire#295 (#24): a
    // zero key or value is a present field of the entry, not an absent
    // one, so proto3 zero-skip does not apply inside it.
    if (v.empty()) return;
    for (const auto& [k, val] : v) {
      std::vector<uint8_t> entry;
      MarshalScalar(entry, 1, k, zigzag);
      MarshalMapValue(entry, 2, val, zigzag);
      wire::AppendTag(out, num, wire::kBytes);
      wire::AppendBytes(out, entry);
    }
    return;
  } else if constexpr (IsVector<T>::value && !std::is_same_v<T, std::vector<uint8_t>>) {
    using E = typename IsVector<T>::element;
    if (v.empty()) return;
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
      for (const auto& el : v) MarshalScalar(out, num, el, zigzag);
    }
  } else {
    if (IsZeroValue(v)) return;
    MarshalScalar(out, num, v, zigzag);
  }
}

template <class T>
inline void MarshalStruct(const T& v, std::vector<uint8_t>& out) {
  std::apply([&](auto&&... f) { ((MarshalField(out, f.number, v.*(f.ptr), f.zigzag)), ...); },
             T::_protowire_fields());
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
    if (n < 0) return Status::Error("corrupt double");
    consumed = n;
    std::memcpy(&v, &bits, 8);
  } else if constexpr (std::is_same_v<T, float>) {
    uint32_t bits;
    int n = wire::ConsumeFixed32(data, bits);
    if (n < 0) return Status::Error("corrupt float");
    consumed = n;
    std::memcpy(&v, &bits, 4);
  } else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>) {
    uint64_t x;
    int n = wire::ConsumeVarint(data, x);
    if (n < 0) return Status::Error("corrupt signed int");
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
    if (n < 0) return Status::Error("corrupt unsigned int");
    consumed = n;
    v = static_cast<T>(x);
  } else if constexpr (std::is_same_v<T, std::string>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt string");
    consumed = n;
    v.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt bytes");
    consumed = n;
    v.assign(bytes.begin(), bytes.end());
  } else if constexpr (std::is_same_v<T, BigInt>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt BigInt");
    consumed = n;
    if (depth + 1 > lim.max_depth) return DepthError(lim);
    return UnmarshalBigIntMsg(bytes, v);
  } else if constexpr (std::is_same_v<T, Decimal>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt Decimal");
    consumed = n;
    if (depth + 1 > lim.max_depth) return DepthError(lim);
    return UnmarshalDecimalMsg(bytes, v, lim.max_digits);
  } else if constexpr (std::is_same_v<T, BigFloat>) {
    std::span<const uint8_t> bytes;
    int n = wire::ConsumeBytes(data, bytes);
    if (n < 0) return Status::Error("corrupt BigFloat");
    consumed = n;
    if (depth + 1 > lim.max_depth) return DepthError(lim);
    return UnmarshalBigFloatMsg(bytes, v);
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
        if (!st.ok()) return st;
        entry_bytes = entry_bytes.subspan(sub_consumed);
      } else if (enum_num == 2) {
        Status st = UnmarshalField(entry_bytes, 2, etyp, val, sub_consumed, zigzag, depth + 1, lim);
        if (!st.ok()) return st;
        entry_bytes = entry_bytes.subspan(sub_consumed);
      } else {
        int skipped = wire::ConsumeFieldValue(entry_bytes, enum_num, etyp);
        if (skipped < 0) return Status::Error("corrupt unknown field in map entry");
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

    bool matched = false;
    Status st = Status::OK();
    int consumed = 0;
    std::apply(
        [&](auto&&... f) {
          (((!matched && f.number == num)
                ? (matched = true,
                   st = UnmarshalField(data, num, type, v.*(f.ptr), consumed, f.zigzag, depth, lim),
                   0)
                : 0),
           ...);
        },
        T::_protowire_fields());

    if (matched) {
      if (!st.ok()) return st;
      data = data.subspan(consumed);
    } else {
      int skipped = wire::ConsumeFieldValue(data, num, type);
      if (skipped < 0) return Status::Error("corrupt unknown field");
      data = data.subspan(skipped);
    }
  }
  return Status::OK();
}

}  // namespace detail

// ---- Public API ---------------------------------------------------------

template <class T>
std::vector<uint8_t> Marshal(const T& v) {
  static_assert(HasFields<T>::value, "Marshal: type must declare PROTOWIRE_FIELDS(...)");
  std::vector<uint8_t> out;
  detail::MarshalStruct(v, out);
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
  return detail::UnmarshalStruct(data, v, /*depth=*/0, lim);
}

template <class T>
Status Unmarshal(const std::vector<uint8_t>& data, T& v, UnmarshalOptions opts = {}) {
  return Unmarshal(std::span<const uint8_t>(data.data(), data.size()), v, opts);
}

}  // namespace protowire::pb

// ---- Macros -------------------------------------------------------------
//
// PROTOWIRE_FIELDS(Type, ...) declares the list of (number, member-pointer)
// pairs for a struct. PROTOWIRE_FIELD(N, member) wraps one entry. Place
// inside the struct definition, after all member declarations.

#define PROTOWIRE_FIELD(N, MEMBER) ::protowire::pb::MakeField<(N)>(&_protowire_self_t::MEMBER)

// Like PROTOWIRE_FIELD, but encodes signed integers with zigzag varint
// (proto3 sint32 / sint64). More compact than plain int32 / int64 for
// negative values.
#define PROTOWIRE_ZIGZAG(N, MEMBER) \
  ::protowire::pb::MakeFieldZigzag<(N)>(&_protowire_self_t::MEMBER)

#define PROTOWIRE_FIELDS(SELF, ...)           \
  using _protowire_self_t = SELF;             \
  static constexpr auto _protowire_fields() { \
    return std::make_tuple(__VA_ARGS__);      \
  }

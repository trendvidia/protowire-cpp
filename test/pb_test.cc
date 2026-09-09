// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pb.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "protowire/limits.h"
#include "protowire/pb_big.h"

namespace {

using protowire::pb::BigFloat;
using protowire::pb::BigInt;
using protowire::pb::Decimal;
using protowire::pb::Marshal;
using protowire::pb::Unmarshal;

struct Inner {
  std::string name;
  int32_t value = 0;
  PROTOWIRE_FIELDS(Inner, PROTOWIRE_FIELD(1, name), PROTOWIRE_FIELD(2, value))
  bool operator==(const Inner&) const = default;
};

struct Outer {
  std::string title;
  uint32_t count = 0;
  double score = 0;
  bool active = false;
  std::vector<uint8_t> data;
  std::vector<Inner> items;
  int64_t signed_v = 0;
  float small_f = 0;
  // No PROTOWIRE_FIELD entry: should be skipped on round-trip.
  std::string untagged;
  PROTOWIRE_FIELDS(Outer,
                   PROTOWIRE_FIELD(1, title),
                   PROTOWIRE_FIELD(2, count),
                   PROTOWIRE_FIELD(3, score),
                   PROTOWIRE_FIELD(4, active),
                   PROTOWIRE_FIELD(5, data),
                   PROTOWIRE_FIELD(6, items),
                   PROTOWIRE_FIELD(8, signed_v),
                   PROTOWIRE_FIELD(9, small_f))
  bool operator==(const Outer&) const = default;
};

TEST(Pb, RoundTrip) {
  Outer orig;
  orig.title = "hello";
  orig.count = 42;
  orig.score = 3.14;
  orig.active = true;
  orig.data = {0xDE, 0xAD};
  orig.items = {{"a", 1}, {"b", -7}};
  orig.signed_v = -12345;
  orig.small_f = 2.5f;
  orig.untagged = "should be ignored";

  auto bytes = Marshal(orig);

  Outer got;
  ASSERT_TRUE(Unmarshal(bytes, got).ok());

  // The untagged field is dropped on the wire.
  Outer expected = orig;
  expected.untagged.clear();
  EXPECT_EQ(got, expected);
}

TEST(Pb, ZeroValuesProduceEmptyWire) {
  Outer orig;
  auto bytes = Marshal(orig);
  EXPECT_TRUE(bytes.empty());

  Outer got;
  ASSERT_TRUE(Unmarshal(bytes, got).ok());
  EXPECT_EQ(got, orig);
}

TEST(Pb, UnknownFieldsSkipped) {
  struct Big {
    std::string a, b, c;
    PROTOWIRE_FIELDS(Big, PROTOWIRE_FIELD(1, a), PROTOWIRE_FIELD(2, b), PROTOWIRE_FIELD(3, c))
  };
  Big big{"aa", "bb", "cc"};
  auto data = Marshal(big);

  struct Small {
    std::string a;
    PROTOWIRE_FIELDS(Small, PROTOWIRE_FIELD(1, a))
  };
  Small small;
  ASSERT_TRUE(Unmarshal(data, small).ok());
  EXPECT_EQ(small.a, "aa");
}

struct BigNumStruct {
  BigInt balance;
  Decimal price;
  BigFloat coefficient;
  PROTOWIRE_FIELDS(BigNumStruct,
                   PROTOWIRE_FIELD(1, balance),
                   PROTOWIRE_FIELD(2, price),
                   PROTOWIRE_FIELD(3, coefficient))
};

TEST(Pb, BigNumRoundTrip) {
  BigNumStruct orig;
  // 115792089237316195423570985008687907853269984665640564039457584007913129639935
  // is 2^256 - 1 — fits in 32 bytes of unsigned big-endian magnitude.
  orig.balance.abs = std::vector<uint8_t>(32, 0xFF);
  orig.balance.negative = false;

  // Decimal 3.1415 — unscaled = 31415, scale = 4.
  orig.price.unscaled = {0x7A, 0xB7};  // 31415 = 0x7AB7
  orig.price.scale = 4;
  orig.price.negative = false;

  // BigFloat with prec 128 and a synthetic mantissa.
  orig.coefficient.mantissa = {0x12, 0x34, 0x56, 0x78};
  orig.coefficient.exponent = 5;
  orig.coefficient.prec = 128;
  orig.coefficient.negative = false;

  auto bytes = Marshal(orig);

  BigNumStruct got;
  ASSERT_TRUE(Unmarshal(bytes, got).ok());
  EXPECT_EQ(got.balance.abs, orig.balance.abs);
  EXPECT_EQ(got.balance.negative, orig.balance.negative);
  EXPECT_EQ(got.price.unscaled, orig.price.unscaled);
  EXPECT_EQ(got.price.scale, orig.price.scale);
  EXPECT_EQ(got.price.negative, orig.price.negative);
  EXPECT_EQ(got.coefficient.mantissa, orig.coefficient.mantissa);
  EXPECT_EQ(got.coefficient.exponent, orig.coefficient.exponent);
  EXPECT_EQ(got.coefficient.prec, orig.coefficient.prec);
  EXPECT_EQ(got.coefficient.negative, orig.coefficient.negative);
}

TEST(Pb, BigNumZeroProducesEmpty) {
  BigNumStruct orig;
  auto bytes = Marshal(orig);
  EXPECT_TRUE(bytes.empty());
}

TEST(Pb, BigNumNegative) {
  BigNumStruct orig;
  orig.balance.abs = {0xE8, 0xD4, 0xA5, 0x10, 0x00};  // arbitrary
  orig.balance.negative = true;

  auto bytes = Marshal(orig);
  BigNumStruct got;
  ASSERT_TRUE(Unmarshal(bytes, got).ok());
  EXPECT_TRUE(got.balance.negative);
  EXPECT_EQ(got.balance.abs, orig.balance.abs);
}

TEST(Pb, ParseFormatBigInt) {
  BigInt b;
  ASSERT_TRUE(protowire::pb::ParseBigInt("12345678901234567890", b));
  EXPECT_FALSE(b.negative);
  EXPECT_EQ(protowire::pb::FormatBigInt(b), "12345678901234567890");

  BigInt n;
  ASSERT_TRUE(protowire::pb::ParseBigInt("-42", n));
  EXPECT_TRUE(n.negative);
  EXPECT_EQ(protowire::pb::FormatBigInt(n), "-42");

  BigInt z;
  ASSERT_TRUE(protowire::pb::ParseBigInt("0", z));
  EXPECT_FALSE(z.negative);
  EXPECT_TRUE(z.abs.empty());
  EXPECT_EQ(protowire::pb::FormatBigInt(z), "0");
}

TEST(Pb, ParseFormatDecimal) {
  Decimal d;
  ASSERT_TRUE(protowire::pb::ParseDecimal("3.14", d));
  EXPECT_EQ(d.scale, 2);
  EXPECT_EQ(protowire::pb::FormatDecimal(d), "3.14");

  Decimal small;
  ASSERT_TRUE(protowire::pb::ParseDecimal("0.05", small));
  EXPECT_EQ(small.scale, 2);
  EXPECT_EQ(protowire::pb::FormatDecimal(small), "0.05");

  Decimal neg;
  ASSERT_TRUE(protowire::pb::ParseDecimal("-1.000", neg));
  EXPECT_TRUE(neg.negative);
  EXPECT_EQ(neg.scale, 3);
  EXPECT_EQ(protowire::pb::FormatDecimal(neg), "-1.000");
}

struct WithZigZag {
  int64_t a = 0;  // proto3 int64 (plain varint)
  int64_t b = 0;  // proto3 sint64 (zigzag varint)
  PROTOWIRE_FIELDS(WithZigZag, PROTOWIRE_FIELD(1, a), PROTOWIRE_ZIGZAG(2, b))
  bool operator==(const WithZigZag&) const = default;
};

TEST(Pb, ZigZagMacro) {
  WithZigZag orig{-1, -1};
  auto bytes = Marshal(orig);
  // Field 1 (a=-1, plain varint): tag 0x08 + 10 bytes (sign-extended).
  // Field 2 (b=-1, zigzag): tag 0x10 + 1 byte (zigzag(-1) = 1 = 0x01).
  // Total: 1 + 10 + 1 + 1 = 13 bytes.
  EXPECT_EQ(bytes.size(), 13u);

  WithZigZag got;
  ASSERT_TRUE(Unmarshal(bytes, got).ok());
  EXPECT_EQ(orig, got);
}

}  // namespace

// ---- HARDENING.md § Mandatory limits (#25, #26) --------------------------

struct Node {
  std::unique_ptr<Node> child;
  std::string label;
  std::vector<int32_t> values;
  std::map<std::string, int32_t> counts;
  protowire::pb::Decimal decimal;
  PROTOWIRE_FIELDS(Node,
                   PROTOWIRE_FIELD(1, child),
                   PROTOWIRE_FIELD(2, label),
                   PROTOWIRE_FIELD(3, values),
                   PROTOWIRE_FIELD(4, counts),
                   PROTOWIRE_FIELD(5, decimal))
};

// n nested submessages under the root: n descents.
std::vector<uint8_t> NestedNode(int n) {
  Node root;
  Node* cur = &root;
  for (int i = 0; i < n; ++i) {
    cur->child = std::make_unique<Node>();
    cur = cur->child.get();
  }
  cur->label = "leaf";
  return Marshal(root);
}

TEST(PbHardening, DepthBoundBothSides) {
  Node at;
  EXPECT_TRUE(Unmarshal(NestedNode(protowire::kMaxNestingDepth), at).ok());
  Node over;
  auto st = Unmarshal(NestedNode(protowire::kMaxNestingDepth + 1), over);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("MaxNestingDepth=100"), std::string::npos) << st.ToString();
  protowire::pb::UnmarshalOptions opts;
  opts.max_nesting_depth = 3;
  Node n3;
  EXPECT_TRUE(Unmarshal(NestedNode(3), n3, opts).ok());
  Node n4;
  EXPECT_FALSE(Unmarshal(NestedNode(4), n4, opts).ok());
}

TEST(PbHardening, MessageSizeBound) {
  Node n;
  n.label = std::string(2000, 'x');
  auto bytes = Marshal(n);
  protowire::pb::UnmarshalOptions opts;
  opts.max_message_size = 1024;
  Node got;
  auto st = Unmarshal(bytes, got, opts);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("exceeds MaxMessageSize=1024"), std::string::npos) << st.ToString();
  opts.max_message_size = 4096;
  EXPECT_TRUE(Unmarshal(bytes, got, opts).ok());
  std::vector<uint8_t> huge(static_cast<size_t>(protowire::kMaxMessageSize) + 1, 0);
  st = Unmarshal(huge, got);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("MaxMessageSize=67108864"), std::string::npos) << st.ToString();
}

TEST(PbHardening, RepeatedCountBoundPackedAndUnpacked) {
  Node n;
  for (int i = 0; i < 16; ++i) n.values.push_back(i);
  auto unpacked = Marshal(n);  // one record per element
  // The same field packed: tag 3 LEN, then 16 varints.
  std::vector<uint8_t> packed = {0x1a, 16};
  for (int i = 0; i < 16; ++i) packed.push_back(static_cast<uint8_t>(i));
  for (const auto& bytes : {unpacked, packed}) {
    protowire::pb::UnmarshalOptions opts;
    opts.max_repeated_count = 8;
    Node got;
    auto st = Unmarshal(bytes, got, opts);
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find("repeated field exceeds MaxRepeatedCount=8"), std::string::npos)
        << st.ToString();
    opts.max_repeated_count = 16;
    Node ok;
    ASSERT_TRUE(Unmarshal(bytes, ok, opts).ok());
    EXPECT_EQ(ok.values, n.values);
  }

  Node m;
  for (int i = 0; i < 16; ++i) m.counts["k" + std::to_string(i)] = i;
  auto bytes = Marshal(m);
  protowire::pb::UnmarshalOptions opts;
  opts.max_repeated_count = 8;
  Node got;
  auto st = Unmarshal(bytes, got, opts);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("map field exceeds MaxRepeatedCount=8"), std::string::npos);
  opts.max_repeated_count = 16;
  EXPECT_TRUE(Unmarshal(bytes, got, opts).ok());
}

// proto3 packs repeated numerics by default; every other encoder in the
// family writes ListHolder { values: [1, 2, 3] } as `0a 03 01 02 03`, which
// this codec read as one element followed by corrupt tags.
TEST(PbHardening, PackedRepeatedDecodes) {
  std::vector<uint8_t> packed_i32 = {0x1a, 0x03, 0x01, 0x02, 0x03};
  Node got;
  ASSERT_TRUE(Unmarshal(packed_i32, got).ok());
  EXPECT_EQ(got.values, (std::vector<int32_t>{1, 2, 3}));
  struct Floats {
    std::vector<double> d;
    std::vector<bool> b;
    PROTOWIRE_FIELDS(Floats, PROTOWIRE_FIELD(1, d), PROTOWIRE_FIELD(2, b))
  };
  // d = [1.0, 2.0] packed (two fixed64), b = [true, false] packed.
  std::vector<uint8_t> bytes = {0x0a, 16, 0, 0, 0, 0, 0,    0,    0xf0, 0x3f, 0,
                                0,    0,  0, 0, 0, 0, 0x40, 0x12, 2,    1,    0};
  Floats f;
  ASSERT_TRUE(Unmarshal(bytes, f).ok());
  EXPECT_EQ(f.d, (std::vector<double>{1.0, 2.0}));
  EXPECT_EQ(f.b, (std::vector<bool>{true, false}));
}

TEST(PbHardening, DecimalScaleBound) {
  // Decimal { scale = 2^31-1 } and { scale = -2^31 }: refused; 4096 accepted.
  auto with_scale = [](int32_t scale) {
    protowire::pb::Decimal d;
    d.unscaled = {1};
    d.scale = scale;
    Node n;
    n.decimal = d;
    return Marshal(n);
  };
  Node got;
  auto st = Unmarshal(with_scale(2147483647), got);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("Decimal.scale 2147483647 exceeds MaxNumericLiteralDigits=4096"),
            std::string::npos)
      << st.ToString();
  EXPECT_FALSE(Unmarshal(with_scale(-2147483647 - 1), got).ok());
  EXPECT_TRUE(Unmarshal(with_scale(4096), got).ok());
  EXPECT_TRUE(Unmarshal(with_scale(-4096), got).ok());
  protowire::pb::UnmarshalOptions opts;
  opts.max_numeric_literal_digits = 8;
  EXPECT_FALSE(Unmarshal(with_scale(9), got, opts).ok());
  EXPECT_TRUE(Unmarshal(with_scale(8), got, opts).ok());
}

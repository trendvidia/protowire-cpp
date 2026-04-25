#include "protowire/pb.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
  PROTOWIRE_FIELDS(Inner,
                   PROTOWIRE_FIELD(1, name),
                   PROTOWIRE_FIELD(2, value))
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
    PROTOWIRE_FIELDS(Big,
                     PROTOWIRE_FIELD(1, a),
                     PROTOWIRE_FIELD(2, b),
                     PROTOWIRE_FIELD(3, c))
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

}  // namespace

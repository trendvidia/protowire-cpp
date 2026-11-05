// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// The struct-binding table (protowire#509): testdata/values/values.tsv lists
// values with the bytes protowire-go's encoding/pb writes for each. This test
// keeps a twin of every struct in the spec's values_table_test.go, marshals the
// row's value, and compares the bytes. The file is a copy of the spec's, and
// scripts/go_differential.sh fails when the two differ.

#include "protowire/pb.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pb_marshal.h"

namespace {

using protowire::pb::BigFloat;
using protowire::pb::BigInt;
using protowire::pb::Decimal;
using protowire::testing::MustMarshal;

// Twins of the spec's structs: same field numbers and types. A Go pointer
// that may be nil is a value here, whose zero is what nil writes: omitted as a
// field, an empty message as a list element or map value.
struct Ints {
  int64_t a = 0, b = 0;
  int32_t c = 0;
  std::map<std::string, int64_t> m;
  std::map<std::string, int32_t> mi;
  PROTOWIRE_FIELDS(Ints,
                   PROTOWIRE_FIELD(1, a),
                   PROTOWIRE_FIELD(2, b),
                   PROTOWIRE_FIELD(3, c),
                   PROTOWIRE_FIELD(4, m),
                   PROTOWIRE_FIELD(5, mi))
};
struct Age {
  int32_t age = 0;
  PROTOWIRE_FIELDS(Age, PROTOWIRE_FIELD(1, age))
};
struct MinLong {
  int64_t v = 0;
  PROTOWIRE_FIELDS(MinLong, PROTOWIRE_FIELD(1, v))
};
struct Zigzag {
  int64_t d = 0;
  int32_t e = 0;
  PROTOWIRE_FIELDS(Zigzag, PROTOWIRE_ZIGZAG(1, d), PROTOWIRE_ZIGZAG(2, e))
};
struct Packed {
  std::vector<int32_t> xs;
  std::vector<int64_t> ys;
  std::vector<bool> bs;
  std::vector<double> ds;
  std::vector<float> fs;
  PROTOWIRE_FIELDS(Packed,
                   PROTOWIRE_FIELD(1, xs),
                   PROTOWIRE_FIELD(2, ys),
                   PROTOWIRE_FIELD(3, bs),
                   PROTOWIRE_FIELD(4, ds),
                   PROTOWIRE_FIELD(5, fs))
};
struct ZeroMapEntry {
  std::map<std::string, std::string> m;
  std::map<std::string, int32_t> z;
  std::map<int32_t, std::string> k;
  PROTOWIRE_FIELDS(ZeroMapEntry,
                   PROTOWIRE_FIELD(1, m),
                   PROTOWIRE_FIELD(2, z),
                   PROTOWIRE_FIELD(3, k))
};
struct DecimalT {
  Decimal d;
  PROTOWIRE_FIELDS(DecimalT, PROTOWIRE_FIELD(1, d))
};
struct Decimals {
  std::vector<Decimal> ds;
  PROTOWIRE_FIELDS(Decimals, PROTOWIRE_FIELD(1, ds))
};
struct Leaf {
  std::string name;
  int32_t n = 0;
  PROTOWIRE_FIELDS(Leaf, PROTOWIRE_FIELD(1, name), PROTOWIRE_FIELD(2, n))
};
struct Nested {
  std::string s;
  std::vector<Leaf> ls;
  std::vector<std::string> ss;
  PROTOWIRE_FIELDS(Nested, PROTOWIRE_FIELD(1, s), PROTOWIRE_FIELD(2, ls), PROTOWIRE_FIELD(3, ss))
};
struct Zeros {
  int32_t i = 0;
  std::string s;
  bool b = false;
  double d = 0;
  std::vector<uint8_t> bs;
  uint64_t u = 0;
  PROTOWIRE_FIELDS(Zeros,
                   PROTOWIRE_FIELD(1, i),
                   PROTOWIRE_FIELD(2, s),
                   PROTOWIRE_FIELD(3, b),
                   PROTOWIRE_FIELD(4, d),
                   PROTOWIRE_FIELD(5, bs),
                   PROTOWIRE_FIELD(6, u))
};
struct BigNums {
  BigInt i;
  Decimal d;
  BigFloat f;
  PROTOWIRE_FIELDS(BigNums, PROTOWIRE_FIELD(1, i), PROTOWIRE_FIELD(2, d), PROTOWIRE_FIELD(3, f))
};
struct BigInts {
  std::vector<BigInt> is;
  PROTOWIRE_FIELDS(BigInts, PROTOWIRE_FIELD(1, is))
};
struct BigFloatMap {
  std::map<std::string, BigFloat> m;
  PROTOWIRE_FIELDS(BigFloatMap, PROTOWIRE_FIELD(1, m))
};
struct BigFloatT {
  BigFloat f;
  PROTOWIRE_FIELDS(BigFloatT, PROTOWIRE_FIELD(1, f))
};

Decimal Dec(const char* s) {
  Decimal d;
  EXPECT_TRUE(protowire::pb::ParseDecimal(s, d)) << s;
  return d;
}

BigInt Int(const char* s) {
  BigInt i;
  EXPECT_TRUE(protowire::pb::ParseBigInt(s, i)) << s;
  return i;
}

template <class T>
std::vector<uint8_t> Bytes(const T& v) {
  return MustMarshal(v);
}

// Each row's value, built from the words in the table's third column.
std::map<std::string, std::function<std::vector<uint8_t>()>> Rows() {
  std::map<std::string, std::function<std::vector<uint8_t>()>> r;
  r["Ints"] = [] { return Bytes(Ints{-1, -1, -1, {{"k", -1}}, {{"k", -1}}}); };
  r["Age"] = [] { return Bytes(Age{30}); };
  r["MinLong"] = [] { return Bytes(MinLong{INT64_MIN}); };
  r["Zigzag"] = [] { return Bytes(Zigzag{-1, -2}); };
  r["Packed"] = [] { return Bytes(Packed{{1, 0, -1}, {300}, {true, false}, {1.5}, {-2.5f}}); };
  r["ZeroMapEntry"] = [] { return Bytes(ZeroMapEntry{{{"", ""}}, {{"zero", 0}}, {{0, "v"}}}); };
  r["Decimal"] = [] { return Bytes(DecimalT{Dec("3.1415")}); };
  r["DecimalOne"] = [] { return Bytes(DecimalT{Dec("1.00")}); };
  // 5E+2: unscaled 5 at scale -2.
  r["DecimalFiveE2"] = [] { return Bytes(DecimalT{Decimal{{5}, -2, false}}); };
  r["DecimalFifteen"] = [] { return Bytes(DecimalT{Dec("15.00")}); };
  r["DecimalZero"] = [] { return Bytes(DecimalT{Dec("0.00")}); };
  r["DecimalNegative"] = [] { return Bytes(DecimalT{Dec("-1.50")}); };
  r["Decimals"] = [] {
    return Bytes(Decimals{{Dec("0.00"), Dec("1.00"), Decimal{{5}, -2, false}}});
  };
  r["Nested"] = [] { return Bytes(Nested{"x", {Leaf{}, Leaf{"l", -3}}, {"", "s"}}); };
  r["Zeros"] = [] { return Bytes(Zeros{}); };
  r["BigNumsNull"] = [] { return Bytes(BigNums{}); };
  r["BigNumsZero"] = [] { return Bytes(BigNums{Int("0"), Dec("0"), BigFloat{}}); };
  // -1.5 at prec 53: mantissa 3, exponent -1.
  r["BigNumsNegative"] = [] {
    return Bytes(BigNums{Int("-12345678901234567890"), Dec("-0.5"), BigFloat{{3}, -1, 53, true}});
  };
  r["BigIntsZeroElement"] = [] { return Bytes(BigInts{{Int("0"), Int("1")}}); };
  r["BigIntsNullElement"] = [] { return Bytes(BigInts{{Int("1"), BigInt{}}}); };
  r["DecimalsNullElement"] = [] { return Bytes(Decimals{{Dec("1"), Decimal{}}}); };
  r["BigFloatMapNullValue"] = [] { return Bytes(BigFloatMap{{{"k", BigFloat{}}}}); };
  r["BigFloatPrec64"] = [] { return Bytes(BigFloatT{BigFloat{{3}, -1, 64, false}}); };
  return r;
}

std::string Hex(const std::vector<uint8_t>& b) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (uint8_t c : b) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}

TEST(PbValuesTable, EveryRowWritesGosBytes) {
  std::ifstream in(std::string(TESTDATA_DIR) + "/values/values.tsv");
  ASSERT_TRUE(in) << "testdata/values/values.tsv";
  const auto rows = Rows();
  std::set<std::string> seen;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string name, hex;
    ASSERT_TRUE(std::getline(fields, name, '\t')) << line;
    ASSERT_TRUE(std::getline(fields, hex, '\t')) << line;
    auto it = rows.find(name);
    if (it == rows.end()) {
      ADD_FAILURE() << "no twin for row " << name << ": add it to Rows()";
      continue;
    }
    seen.insert(name);
    EXPECT_EQ(Hex(it->second()), hex) << name;
  }
  // A twin with no row means the table dropped a case.
  for (const auto& [name, unused] : rows) EXPECT_EQ(seen.count(name), 1u) << name;
  EXPECT_EQ(seen.size(), 23u);
}

}  // namespace

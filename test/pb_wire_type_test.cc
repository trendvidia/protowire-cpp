// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// A tag's wire type, as protobuf reads it (#179, protowire#614).
//
// A PB decoder rejects wire type 6 or 7 on any field, and handles a known field
// whose wire type does not match its type as an unknown field: skipped by its
// wire type, the field left as it was. Unmarshal used to read a known field by
// its C++ type whatever the tag said, so `0a01` on an int64 read the length
// byte as the value 1, and `0a0101` was refused as corrupt.
//
// Each row is an input, protowire-go's error text, and what protowire-go's
// Marshal writes for the struct the input decoded to, so the values are
// compared as well as the verdict. They were made by running protowire-go at
// a0a78c2 on Go structs with these members, names and field numbers.
// pb_wire_type_oracle_test.cc compares the verdicts with protobuf's.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "protowire/pb.h"

namespace {

namespace pb = protowire::pb;
using Bytes = std::vector<uint8_t>;

Bytes FromHex(const std::string& h) {
  Bytes out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string ToHex(const Bytes& b) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (uint8_t c : b) {
    out += digits[c >> 4];
    out += digits[c & 0xf];
  }
  return out;
}

struct WtInner {
  std::string Name;
  int64_t N = 0;
  PROTOWIRE_FIELDS(WtInner, PROTOWIRE_FIELD(1, Name), PROTOWIRE_FIELD(2, N))
};

// Member names are the Go struct's, since an error names the member.
struct WtMsg {
  std::string S;
  int64_t I64 = 0;
  std::unique_ptr<int64_t> P;
  std::vector<std::string> L;
  std::vector<uint32_t> Nums;
  std::map<std::string, int64_t> M;
  float F32 = 0;
  double F64 = 0;
  std::optional<bool> B;
  std::unique_ptr<WtInner> Inner;
  int32_t Z = 0;
  Bytes Buf;
  PROTOWIRE_FIELDS(WtMsg,
                   PROTOWIRE_FIELD(1, S),
                   PROTOWIRE_FIELD(2, I64),
                   PROTOWIRE_FIELD(3, P),
                   PROTOWIRE_FIELD(4, L),
                   PROTOWIRE_FIELD(5, Nums),
                   PROTOWIRE_FIELD(6, M),
                   PROTOWIRE_FIELD(7, F32),
                   PROTOWIRE_FIELD(8, F64),
                   PROTOWIRE_FIELD(9, B),
                   PROTOWIRE_FIELD(10, Inner),
                   PROTOWIRE_ZIGZAG(11, Z),
                   PROTOWIRE_FIELD(12, Buf))
};

struct BigMsg {
  pb::BigInt I;
  pb::Decimal R;
  pb::BigFloat F;
  PROTOWIRE_FIELDS(BigMsg, PROTOWIRE_FIELD(1, I), PROTOWIRE_FIELD(2, R), PROTOWIRE_FIELD(3, F))
};

struct Row {
  const char* in;
  const char* status;   // "ok", or the error
  const char* written;  // Marshal of what an accepted input decoded to
};

template <class T>
void CheckRows(const std::vector<Row>& rows) {
  int accepted = 0;
  int rejected = 0;
  for (const Row& row : rows) {
    T m;
    protowire::Status st = pb::Unmarshal(FromHex(row.in), m);
    if (std::string(row.status) != "ok") {
      ++rejected;
      EXPECT_EQ(st.message(), row.status) << row.in;
      continue;
    }
    ++accepted;
    ASSERT_TRUE(st.ok()) << row.in << ": " << st.message();
    auto out = pb::Marshal(m);
    ASSERT_TRUE(out.ok()) << row.in << ": " << out.status().message();
    EXPECT_EQ(ToHex(*out), row.written) << row.in;
  }
  // A positive marker that both verdicts were exercised.
  EXPECT_GT(accepted, 3);
  EXPECT_GT(rejected, 3);
}

TEST(PbWireType, MatchesProtowireGo) {
  CheckRows<WtMsg>({
      // Wire types 6 and 7 on a known field of each kind.
      {"0e01", "corrupt field 1: proto: cannot parse reserved wire type", ""},
      {"0f0161", "corrupt field 1: proto: cannot parse reserved wire type", ""},
      {"1601", "corrupt field 2: proto: cannot parse reserved wire type", ""},
      {"1f01", "corrupt field 3: proto: cannot parse reserved wire type", ""},
      {"3e01", "corrupt field 7: proto: cannot parse reserved wire type", ""},
      {"5701", "corrupt field 10: proto: cannot parse reserved wire type", ""},
      // Another wire type, well formed: skipped, and the field left as it was.
      {"0801", "ok", ""},
      {"0d01000000", "ok", ""},
      {"090100000000000000", "ok", ""},
      {"120101", "ok", ""},
      {"1501000000", "ok", ""},
      {"1b1c", "ok", ""},
      {"1a0101", "ok", ""},
      {"2001", "ok", ""},
      {"2501000000", "ok", ""},
      {"2a0101", "ok", "2a0101"},  // packed
      {"3d00000000", "ok", ""},
      {"3801", "ok", ""},
      {"4001", "ok", ""},
      {"4a0101", "ok", ""},
      {"4801", "ok", "4801"},
      {"5001", "ok", ""},
      {"5a0101", "ok", ""},
      {"6001", "ok", ""},
      // Another wire type, cut short.
      {"1201", "corrupt field 2: unexpected EOF", ""},
      {"1d01", "corrupt field 3: unexpected EOF", ""},
      {"1901", "corrupt field 3: unexpected EOF", ""},
      {"0d01", "corrupt field 1: unexpected EOF", ""},
      {"1b", "corrupt field 3: unexpected EOF", ""},
      {"1c", "corrupt field 3: proto: mismatching end group marker", ""},
      // A match after a mismatch, and a mismatch after a match.
      {"0a01610801", "ok", "0a0161"},
      {"10071501000000", "ok", "1007"},
      {"2a01012801", "ok", "2a020101"},
      {"28012d01000000", "ok", "2a0101"},
      {"18012a0161", "ok", "18012a0161"},
      {"2201612001", "ok", "220161"},
      // A map entry's key or value of another wire type is skipped.
      {"32080a016b1501000000", "ok", "32050a016b1000"},
      {"320408011001", "ok", "32040a001001"},
      {"32050a016b1001", "ok", "32050a016b1001"},
      {"32060a016b150100", "field M: corrupt field 2 in map entry: unexpected EOF", ""},
      {"32031f0101",
       "field M: corrupt field 3 in map entry: proto: cannot parse reserved wire type",
       ""},
      {"32040a016b1e",
       "field M: corrupt field 3 in map entry: proto: cannot parse reserved wire type",
       ""},
      // So is a nested message's field.
      {"520408011001", "ok", "52021001"},
      {"52030e0100", "field Inner: corrupt field 1: proto: cannot parse reserved wire type", ""},
  });
}

// The big-number messages are read by the same rule.
TEST(PbWireType, BigNumbersMatchProtowireGo) {
  CheckRows<BigMsg>({
      {"0a060a0105120101", "ok", "0a030a0105"},  // BigInt.negative as bytes: skipped
      {"0a020805", "ok", ""},                    // BigInt.abs as a varint: skipped
      {"0a040a01051d", "field I: corrupt field 3 in BigInt: unexpected EOF", ""},
      {"12080a010f1501000000", "ok", "12030a010f"},  // Decimal.scale as fixed32
      {"12050a010f1a01", "field R: corrupt field 3 in Decimal: unexpected EOF", ""},
      // BigFloat.exponent as bytes: the value is 1 at prec 53.
      {"1a080a01011201011835", "ok", "1a160a071000000000000010ccffffffffffffffff011835"},
      {"1a070a010118352201", "field F: corrupt field 4 in BigFloat: unexpected EOF", ""},
      {"0a020e01",
       "field I: corrupt field 1 in BigInt: proto: cannot parse reserved wire type",
       ""},
      {"12031f0101",
       "field R: corrupt field 3 in Decimal: proto: cannot parse reserved wire type",
       ""},
      {"1a022601",
       "field F: corrupt field 4 in BigFloat: proto: cannot parse reserved wire type",
       ""},
      {"1a021d01", "field F: corrupt field 3 in BigFloat: unexpected EOF", ""},
      {"1a030a0101", "ok", ""},
      {"0a020d01", "field I: corrupt field 1 in BigInt: unexpected EOF", ""},
  });
}

// A skipped field is not touched: a pointer stays null, an optional unset, and
// a list gets no element.
TEST(PbWireType, MismatchLeavesFieldAlone) {
  WtMsg m;
  ASSERT_TRUE(pb::Unmarshal(FromHex("1d01000000"  // P as fixed32
                                    "2001"        // L as a varint
                                    "4d01000000"  // B as fixed32
                                    "5001"        // Inner as a varint
                                    "0a0161"),    // S, well formed
                            m)
                  .ok());
  EXPECT_EQ(m.P, nullptr);
  EXPECT_TRUE(m.L.empty());
  EXPECT_FALSE(m.B.has_value());
  EXPECT_EQ(m.Inner, nullptr);
  EXPECT_EQ(m.S, "a");
}

}  // namespace

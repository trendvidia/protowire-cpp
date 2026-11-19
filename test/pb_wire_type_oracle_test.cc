// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// A tag's wire type, against protobuf itself (#179, protowire#614): what
// protowire-go's encoding/pb/wire_type_test.go does against protobuf-go.
//
// Every field of ConfAll is sent one record of every wire type, well formed
// and cut short, at the top level and inside each message and map-entry field.
// Each input must get libprotobuf's verdict, and on accept the same values:
// libprotobuf's message, read without its unknown fields and re-encoded, must
// decode here to what the input decoded to. The values are compared through
// Marshal, which writes a struct's every set field.
//
// This lives apart from pb_wire_type_test.cc because protowire_pb does not link
// protobuf; this target does, as the oracle.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/compiler/parser.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "protowire/detail/wire.h"
#include "protowire/pb.h"

namespace {

namespace gpb = google::protobuf;
namespace pb = protowire::pb;
namespace wire = protowire::wire;
using Bytes = std::vector<uint8_t>;

constexpr const char* kConfProto = R"(
syntax = "proto3";
package conf;

message Inner {
  string name = 1;
  int64 n = 2;
}

message All {
  string s = 1;
  int64 i64 = 2;
  int32 i32 = 3;
  uint64 u64 = 4;
  uint32 u32 = 5;
  sint64 z64 = 6;
  sint32 z32 = 7;
  bool b = 8;
  double f64 = 9;
  float f32 = 10;
  bytes buf = 11;
  Inner inner = 12;
  repeated Inner list = 13;
  repeated uint32 nums = 14;
  repeated sint64 zigs = 15;
  repeated float reals = 16;
  repeated string strs = 17;
  map<string, int64> m = 18;
  map<int32, Inner> mm = 19;
  oneof choice {
    string a = 20;
    int64 c = 21;
    Inner d = 22;
  }
  repeated bool flags = 23;
  repeated int64 negs = 24;
  map<bool, bool> bm = 25;
}
)";

struct ConfInner {
  std::string name;
  int64_t n = 0;
  PROTOWIRE_FIELDS(ConfInner, PROTOWIRE_FIELD(1, name), PROTOWIRE_FIELD(2, n))
};

// The oneof's members are plain fields here: each input sets at most one.
struct ConfAll {
  std::string s;
  int64_t i64 = 0;
  int32_t i32 = 0;
  uint64_t u64 = 0;
  uint32_t u32 = 0;
  int64_t z64 = 0;
  int32_t z32 = 0;
  bool b = false;
  double f64 = 0;
  float f32 = 0;
  Bytes buf;
  std::unique_ptr<ConfInner> inner;
  std::vector<ConfInner> list;
  std::vector<uint32_t> nums;
  std::vector<int64_t> zigs;
  std::vector<float> reals;
  std::vector<std::string> strs;
  std::map<std::string, int64_t> m;
  std::map<int32_t, ConfInner> mm;
  std::string a;
  int64_t c = 0;
  std::unique_ptr<ConfInner> d;
  std::vector<bool> flags;
  std::vector<int64_t> negs;
  std::map<bool, bool> bm;
  PROTOWIRE_FIELDS(ConfAll,
                   PROTOWIRE_FIELD(1, s),
                   PROTOWIRE_FIELD(2, i64),
                   PROTOWIRE_FIELD(3, i32),
                   PROTOWIRE_FIELD(4, u64),
                   PROTOWIRE_FIELD(5, u32),
                   PROTOWIRE_ZIGZAG(6, z64),
                   PROTOWIRE_ZIGZAG(7, z32),
                   PROTOWIRE_FIELD(8, b),
                   PROTOWIRE_FIELD(9, f64),
                   PROTOWIRE_FIELD(10, f32),
                   PROTOWIRE_FIELD(11, buf),
                   PROTOWIRE_FIELD(12, inner),
                   PROTOWIRE_FIELD(13, list),
                   PROTOWIRE_FIELD(14, nums),
                   PROTOWIRE_ZIGZAG(15, zigs),
                   PROTOWIRE_FIELD(16, reals),
                   PROTOWIRE_FIELD(17, strs),
                   PROTOWIRE_FIELD(18, m),
                   PROTOWIRE_FIELD(19, mm),
                   PROTOWIRE_FIELD(20, a),
                   PROTOWIRE_FIELD(21, c),
                   PROTOWIRE_FIELD(22, d),
                   PROTOWIRE_FIELD(23, flags),
                   PROTOWIRE_FIELD(24, negs),
                   PROTOWIRE_FIELD(25, bm))
};

Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

Bytes Tag(uint32_t num, int type) {
  Bytes out;
  wire::AppendVarint(out, (static_cast<uint64_t>(num) << 3) | static_cast<uint64_t>(type));
  return out;
}

// Single records for field num: every wire type, each with a well-formed value
// and a value cut short.
std::vector<Bytes> WireRecords(uint32_t num) {
  return {
      Cat({Tag(num, wire::kVarint), {0x01}}),
      Cat({Tag(num, wire::kVarint), {0x80}}),  // cut short
      Cat({Tag(num, wire::kFixed64), {1, 0, 0, 0, 0, 0, 0, 0}}),
      Cat({Tag(num, wire::kFixed64), {1, 0, 0}}),  // cut short
      Cat({Tag(num, wire::kBytes), {0x00}}),
      Cat({Tag(num, wire::kBytes), {0x01, 0x01}}),
      Cat({Tag(num, wire::kBytes), {0x01}}),  // cut short
      Cat({Tag(num, wire::kStartGroup), Tag(num, wire::kEndGroup)}),
      Tag(num, wire::kStartGroup),  // never ended
      Tag(num, wire::kEndGroup),    // ends nothing
      Cat({Tag(num, wire::kFixed32), {1, 0, 0, 0}}),
      Cat({Tag(num, wire::kFixed32), {1, 0}}),  // cut short
      Cat({Tag(num, 6), {0x01}}),
      Cat({Tag(num, 7), {0x01}}),
  };
}

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

// Every record for every field of ConfAll, alone and carried inside each
// message field (12, 13, 22: Inner's fields 1, 2 and an unknown 3) and map
// field (18, 19, 25: key 1, value 2, unknown 3).
std::vector<Bytes> WireTypeInputs() {
  std::vector<Bytes> out;
  for (uint32_t num = 1; num <= 25; ++num) {
    for (Bytes& rec : WireRecords(num)) out.push_back(std::move(rec));
  }
  for (uint32_t outer : {12, 13, 18, 19, 22, 25}) {
    for (uint32_t inner = 1; inner <= 3; ++inner) {
      for (const Bytes& rec : WireRecords(inner)) {
        Bytes b = Tag(outer, wire::kBytes);
        wire::AppendBytes(b, rec);
        out.push_back(std::move(b));
      }
    }
  }
  // A matching record after a mismatched one: the mismatch leaves the field as
  // it was, and the match still sets it.
  for (const char* h : {"0a0161"
                        "0801",  // s: "a", then s as a varint
                        "1007"
                        "1501000000",  // i64: 7, then i64 as fixed32
                        "720101"
                        "7001",  // nums packed [1], then nums as a varint
                        "7001"
                        "7501000000"}) {  // nums [1], then nums as fixed32
    out.push_back(FromHex(h));
  }
  return out;
}

TEST(PbWireType, MatchesProtobuf) {
  gpb::FileDescriptorProto fdp;
  {
    gpb::io::ArrayInputStream in(kConfProto, static_cast<int>(std::strlen(kConfProto)));
    gpb::io::Tokenizer tokenizer(&in, nullptr);
    gpb::compiler::Parser parser;
    ASSERT_TRUE(parser.Parse(&tokenizer, &fdp));
  }
  fdp.set_name("conf.proto");
  gpb::DescriptorPool pool;
  ASSERT_NE(pool.BuildFile(fdp), nullptr);
  const gpb::Descriptor* desc = pool.FindMessageTypeByName("conf.All");
  ASSERT_NE(desc, nullptr);
  gpb::DynamicMessageFactory factory(&pool);

  const std::vector<Bytes> inputs = WireTypeInputs();
  int accepted = 0;
  int rejected = 0;
  for (const Bytes& in : inputs) {
    std::unique_ptr<gpb::Message> oracle(factory.GetPrototype(desc)->New());
    const bool oracle_ok = oracle->ParseFromArray(in.data(), static_cast<int>(in.size()));
    ConfAll got;
    protowire::Status st = pb::Unmarshal(in, got);
    if (!oracle_ok) {
      ++rejected;
      EXPECT_FALSE(st.ok()) << ToHex(in) << ": libprotobuf rejects it";
      continue;
    }
    ++accepted;
    if (!st.ok()) {
      ADD_FAILURE() << ToHex(in) << ": libprotobuf accepts it, Unmarshal says " << st.message();
      continue;
    }
    oracle->DiscardUnknownFields();
    std::string known;
    ASSERT_TRUE(oracle->SerializeToString(&known));
    ConfAll want;
    protowire::Status wst = pb::Unmarshal(Bytes(known.begin(), known.end()), want);
    ASSERT_TRUE(wst.ok()) << ToHex(Bytes(known.begin(), known.end())) << ": " << wst.message();
    auto got_bytes = pb::Marshal(got);
    auto want_bytes = pb::Marshal(want);
    ASSERT_TRUE(got_bytes.ok() && want_bytes.ok()) << ToHex(in);
    EXPECT_EQ(ToHex(*got_bytes), ToHex(*want_bytes))
        << ToHex(in) << ": values differ from libprotobuf's";
  }
  // A positive marker that both verdicts were exercised, and not only one.
  std::cout << inputs.size() << " inputs: libprotobuf accepts " << accepted << ", rejects "
            << rejected << "\n";
  EXPECT_GT(accepted, 100);
  EXPECT_GT(rejected, 100);
}

}  // namespace

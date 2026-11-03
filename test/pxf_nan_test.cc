// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Draft -01 § Numeric Literals: a decoder reading `nan` stores the canonical
// quiet NaN, 0x7FF8000000000000 for a double and 0x7FC00000 for a float
// (protowire#512, protowire-java#161). Mirrors nan_canonical_test.go in
// protowire-go (protowire-go#241).

#include "protowire/pxf.h"
#include "protowire/pxf/go_wire.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;

constexpr uint64_t kCanonicalNaN64 = 0x7FF8000000000000ULL;
constexpr uint32_t kCanonicalNaN32 = 0x7FC00000U;

constexpr const char* kSchema = R"src(
syntax = "proto3";
package nan.v1;
import "google/protobuf/wrappers.proto";
import "pxf/annotations.proto";
message M {
  double d = 1;
  float f = 2;
  repeated double ds = 3;
  repeated float fs = 4;
  google.protobuf.DoubleValue dw = 5;
  google.protobuf.FloatValue fw = 6;
  optional double d_default = 7 [(pxf.default) = "nan"];
  optional float f_default = 8 [(pxf.default) = "nan"];
}
)src";

class Errors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
           ": " + std::string(msg);
  }
  std::string last;
};

uint64_t Bits(double v) {
  uint64_t b;
  std::memcpy(&b, &v, sizeof(b));
  return b;
}

uint32_t Bits(float v) {
  uint32_t b;
  std::memcpy(&b, &v, sizeof(b));
  return b;
}

class PxfNaN : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream(protowire_test::FixtureDir() + "/nan.proto", std::ios::binary) << kSchema;
    tree_.MapPath("", protowire_test::FixtureDir());
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    const pb::FileDescriptor* file = importer_->Import("nan.proto");
    ASSERT_NE(file, nullptr) << errors_.last;
    desc_ = file->FindMessageTypeByName("M");
    ASSERT_NE(desc_, nullptr);
  }

  std::unique_ptr<pb::Message> New() {
    return std::unique_ptr<pb::Message>(factory_.GetPrototype(desc_)->New());
  }

  const pb::FieldDescriptor* Field(const char* name) { return desc_->FindFieldByName(name); }

  pb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::Descriptor* desc_ = nullptr;
  pb::DynamicMessageFactory factory_;
};

TEST_F(PxfNaN, DecodedCanonically) {
  auto msg = New();
  ASSERT_TRUE(pxf::UnmarshalFull(
                  "d = nan\nf = nan\nds = [nan, 1]\nfs = [nan]\ndw = nan\nfw = nan\n", msg.get())
                  .ok());
  const pb::Reflection* r = msg->GetReflection();
  EXPECT_EQ(Bits(r->GetDouble(*msg, Field("d"))), kCanonicalNaN64);
  EXPECT_EQ(Bits(r->GetFloat(*msg, Field("f"))), kCanonicalNaN32);
  EXPECT_EQ(Bits(r->GetRepeatedDouble(*msg, Field("ds"), 0)), kCanonicalNaN64);
  EXPECT_EQ(Bits(r->GetRepeatedFloat(*msg, Field("fs"), 0)), kCanonicalNaN32);
  const auto inner = [&](const char* name) -> const pb::Message& {
    return r->GetMessage(*msg, Field(name));
  };
  const pb::Message& dw = inner("dw");
  EXPECT_EQ(Bits(dw.GetReflection()->GetDouble(dw, dw.GetDescriptor()->FindFieldByName("value"))),
            kCanonicalNaN64);
  const pb::Message& fw = inner("fw");
  EXPECT_EQ(Bits(fw.GetReflection()->GetFloat(fw, fw.GetDescriptor()->FindFieldByName("value"))),
            kCanonicalNaN32);
  EXPECT_EQ(Bits(r->GetDouble(*msg, Field("d_default"))), kCanonicalNaN64);
  EXPECT_EQ(Bits(r->GetFloat(*msg, Field("f_default"))), kCanonicalNaN32);

  // And so the bytes: field 1, fixed64, 00 00 00 00 00 00 f8 7f.
  const std::string out = pxf::SerializeGoOrder(*msg);
  ASSERT_GE(out.size(), 9u);
  EXPECT_EQ(out.substr(0, 9), std::string("\x09\0\0\0\0\0\0\xf8\x7f", 9));
}

// A message's fields are written as they are: a NaN that arrives in PB with a
// payload keeps it. The rule above is about what a decoder chooses, never
// about bits it was given.
TEST_F(PxfNaN, PayloadFromWireIsKept) {
  auto in = New();
  const uint64_t payload = 0x7FF8000000000001ULL;
  double d;
  std::memcpy(&d, &payload, sizeof(d));
  in->GetReflection()->SetDouble(in.get(), Field("d"), d);
  const std::string wire = pxf::SerializeGoOrder(*in);
  auto back = New();
  ASSERT_TRUE(back->ParseFromString(wire));
  EXPECT_EQ(Bits(back->GetReflection()->GetDouble(*back, Field("d"))), payload);
  EXPECT_EQ(pxf::SerializeGoOrder(*back), wire);
}

}  // namespace

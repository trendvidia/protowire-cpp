// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Tests for pxf.BigInt / pxf.Decimal / pxf.BigFloat sugar.

#include "protowire/pxf.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pbuf = google::protobuf;

class CollectErrors : public pbuf::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

// Schema embedding pxf.BigInt / Decimal / BigFloat fields.
constexpr const char* kProto = R"(
syntax = "proto3";
package bignum.test;
import "pxf/bignum.proto";

message Wallet {
  pxf.BigInt    balance = 1;
  pxf.Decimal   price   = 2;
  pxf.BigFloat  rate    = 3;
}
)";

class PxfBigNum : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", PROTO_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    source_tree_.MapPath("", protowire_test::FixtureDir());
    std::string proto_path = protowire_test::FixtureDir() + "/wallet.proto";
    FILE* f = std::fopen(proto_path.c_str(), "w");
    std::fwrite(kProto, 1, std::strlen(kProto), f);
    std::fclose(f);

    importer_ = std::make_unique<pbuf::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("wallet.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("bignum.test.Wallet");
    ASSERT_NE(desc_, nullptr);
    factory_ = std::make_unique<pbuf::DynamicMessageFactory>(importer_->pool());
  }

  std::unique_ptr<pbuf::Message> NewWallet() {
    return std::unique_ptr<pbuf::Message>(factory_->GetPrototype(desc_)->New());
  }

  pbuf::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pbuf::compiler::Importer> importer_;
  const pbuf::FileDescriptor* file_ = nullptr;
  const pbuf::Descriptor* desc_ = nullptr;
  std::unique_ptr<pbuf::DynamicMessageFactory> factory_;
};

TEST_F(PxfBigNum, BigIntFromBareLiteral) {
  auto msg = NewWallet();
  ASSERT_TRUE(protowire::pxf::Unmarshal(R"(
balance = 12345678901234567890
)",
                                        msg.get())
                  .ok());
  // Re-encode and verify the literal round-trips.
  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(out.ok());
  EXPECT_NE(out->find("balance = 12345678901234567890"), std::string::npos);
}

TEST_F(PxfBigNum, BigIntNegative) {
  auto msg = NewWallet();
  ASSERT_TRUE(protowire::pxf::Unmarshal(R"(
balance = -42
)",
                                        msg.get())
                  .ok());
  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(out.ok());
  EXPECT_NE(out->find("balance = -42"), std::string::npos);
}

TEST_F(PxfBigNum, DecimalPreservesScale) {
  auto msg = NewWallet();
  ASSERT_TRUE(protowire::pxf::Unmarshal(R"(
price = 3.1415
)",
                                        msg.get())
                  .ok());
  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(out.ok());
  EXPECT_NE(out->find("price = 3.1415"), std::string::npos);
}

TEST_F(PxfBigNum, DecimalNegativeFractional) {
  auto msg = NewWallet();
  ASSERT_TRUE(protowire::pxf::Unmarshal(R"(
price = -0.05
)",
                                        msg.get())
                  .ok());
  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(out.ok());
  EXPECT_NE(out->find("price = -0.05"), std::string::npos);
}

// Sets Wallet.price from raw fields, as a protobuf runtime leaves it.
static void SetPrice(pbuf::Message* msg, std::string unscaled, int32_t scale) {
  const pbuf::Reflection* r = msg->GetReflection();
  pbuf::Message* price = r->MutableMessage(msg, msg->GetDescriptor()->FindFieldByName("price"));
  const pbuf::Reflection* pr = price->GetReflection();
  const pbuf::Descriptor* pd = price->GetDescriptor();
  pr->SetString(price, pd->FindFieldByName("unscaled"), std::move(unscaled));
  pr->SetInt32(price, pd->FindFieldByName("scale"), scale);
}

// A producer of PXF text fails on a value past a bound (HARDENING.md §
// Arbitrary-precision magnitudes), on both signs, before doing the work: the
// scale is four bytes and was a hang at INT32_MAX.
TEST_F(PxfBigNum, DecimalScalePastTheBoundIsRefused) {
  for (int32_t scale : {4097, -4097, INT32_MAX, INT32_MIN}) {
    auto msg = NewWallet();
    SetPrice(msg.get(), "\x19", scale);
    auto out = protowire::pxf::Marshal(*msg);
    ASSERT_FALSE(out.ok()) << "scale " << scale;
    EXPECT_NE(out.status().message().find("exceeds MaxNumericLiteralDigits"), std::string::npos)
        << out.status().message();
  }
}

TEST_F(PxfBigNum, DecimalNegativeScaleIsTrailingZeros) {
  auto msg = NewWallet();
  SetPrice(msg.get(), "\x05", -2);
  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_NE(out->find("price = 500"), std::string::npos) << *out;
}

// Scale 4096 is accepted, and leading zeros don't count toward the cap.
TEST_F(PxfBigNum, DecimalAtTheBoundRenders) {
  auto msg = NewWallet();
  SetPrice(msg.get(), "\x05", 4096);
  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_NE(out->find("price = 0." + std::string(4095, '0') + "5"), std::string::npos);
}

// Trailing zeros are significant, so these have no literal.
TEST_F(PxfBigNum, DecimalLiteralPastTheCapIsRefused) {
  auto msg = NewWallet();
  SetPrice(msg.get(), "\x0a", -4096);  // 4098 digits
  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_FALSE(out.ok());
  EXPECT_NE(out.status().message().find("renders to 4098 digits"), std::string::npos)
      << out.status().message();
}

TEST_F(PxfBigNum, BigFloatRoundTripsApprox) {
  // BigFloat parses through double — round-trip preserves value to within
  // double precision. Pick a clean dyadic fraction so the textual form
  // matches exactly after %.17g formatting.
  auto msg = NewWallet();
  ASSERT_TRUE(protowire::pxf::Unmarshal(R"(
rate = 0.5
)",
                                        msg.get())
                  .ok());
  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(out.ok());
  EXPECT_NE(out->find("rate = 0.5"), std::string::npos);
}

}  // namespace

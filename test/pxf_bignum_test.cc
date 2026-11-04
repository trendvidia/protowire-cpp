// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Tests for pxf.BigInt / pxf.Decimal / pxf.BigFloat sugar.

#include "protowire/pxf.h"

#include "protowire/limits.h"

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

// big.Float holds at most 2^31-1 mantissa bits, so a non-zero literal with a
// wider suffix is refused, in protowire-go's words (protowire-go#231). Only a
// digit cap raised past 646 million admits such a suffix. A zero has no
// mantissa and is accepted.
TEST_F(PxfBigNum, BigFloatWiderThanBigFloatIsRefused) {
  protowire::pxf::UnmarshalOptions opts;
  opts.max_numeric_literal_digits = 2000000000;
  const std::pair<const char*, const char*> refused[] = {
      {"rate = 1p2147483648\n",
       "big float literal 1: precision p2147483648 exceeds the widest mantissa big.Float can hold, "
       "2147483647 bits"},
      {"rate = -1.5p4294967295\n",
       "big float literal -1.5: precision p4294967295 exceeds the widest mantissa big.Float can "
       "hold, 2147483647 bits"},
  };
  for (const auto& [doc, want] : refused) {
    auto msg = NewWallet();
    auto st = protowire::pxf::Unmarshal(doc, msg.get(), opts);
    ASSERT_FALSE(st.ok()) << doc;
    EXPECT_NE(st.message().find(want), std::string::npos) << st.ToString();
  }
  auto msg = NewWallet();
  EXPECT_TRUE(protowire::pxf::Unmarshal("rate = 0p2147483648\n", msg.get(), opts).ok());
}

// protowire-go's TestMarshalBigFloatExponentBoundEdges. The bound is on the
// value after rounding to prec (protowire-go#261): 2^54-1 at exponent 16330
// is at the bound on the wire, and at prec 3 it rounds to 2^54, binary
// exponent 16385, whose literal no reader takes. What Marshal writes, the
// decoder takes.
TEST_F(PxfBigNum, BigFloatMarshalExponentBoundEdges) {
  constexpr int32_t kBound = protowire::kMaxBigFloatExponent;
  struct Case {
    const char* name;
    std::string mantissa;
    int32_t exponent;
    uint32_t prec;
    const char* want_err;  // "" for a literal
  };
  const Case cases[] = {
      {"prec 0", "\x01", INT32_MIN, 0, ""},
      {"empty mantissa", "", INT32_MIN, 64, ""},
      {"zero bytes", std::string(2, '\0'), INT32_MAX, 64, ""},
      {"leading zeros, at the bound", std::string("\0\0\x03", 3), kBound - 2, 64, ""},
      {"leading zeros, past the bound",
       std::string("\0\0\x03", 3),
       kBound - 1,
       64,
       "binary exponent 16385 exceeds MaxBigFloatExponent=16384"},
      {"rounds past the bound",
       "\x3f\xff\xff\xff\xff\xff\xff",
       16330,
       3,
       "binary exponent 16385 after rounding to prec 3 exceeds MaxBigFloatExponent=16384"},
      // The same mantissa rounding down stays at the bound: 0b101 followed by
      // 51 zeros at prec 3 is exact.
      {"rounds within the bound", std::string("\x28\0\0\0\0\0\0", 7), 16330, 3, ""},
      // Not in Go's table: a zero is bounded by its digits as Go's
      // formatBigFloat bounds it, rather than written as 0p<prec>.
      {"zero past the digit bound",
       "",
       0,
       4000000,
       "pxf.BigFloat prec 4000000 bits renders to 1204121 digits; MaxNumericLiteralDigits=4096"},
  };
  const auto* rate_fd = desc_->FindFieldByName("rate");
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    auto msg = NewWallet();
    pbuf::Message* rate = msg->GetReflection()->MutableMessage(msg.get(), rate_fd);
    const pbuf::Descriptor* d = rate->GetDescriptor();
    const pbuf::Reflection* r = rate->GetReflection();
    r->SetString(rate, d->FindFieldByName("mantissa"), c.mantissa);
    r->SetInt32(rate, d->FindFieldByName("exponent"), c.exponent);
    r->SetUInt32(rate, d->FindFieldByName("prec"), c.prec);
    auto out = protowire::pxf::Marshal(*msg);
    if (*c.want_err != '\0') {
      ASSERT_FALSE(out.ok()) << *out;
      EXPECT_NE(out.status().message().find(c.want_err), std::string::npos)
          << out.status().ToString();
      continue;
    }
    ASSERT_TRUE(out.ok()) << out.status().ToString();
    auto back = NewWallet();
    EXPECT_TRUE(protowire::pxf::Unmarshal(*out, back.get()).ok())
        << "the decoder takes what the encoder wrote: " << *out;
  }
}

TEST_F(PxfBigNum, BigFloatRoundTrips) {
  // An unsuffixed literal reads at 256 bits, and the suffix written back
  // says so.
  auto msg = NewWallet();
  ASSERT_TRUE(protowire::pxf::Unmarshal(R"(
rate = 0.5
)",
                                        msg.get())
                  .ok());
  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(out.ok());
  EXPECT_NE(out->find("rate = 0.5p256"), std::string::npos);
}

}  // namespace

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
#include <random>
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

// The wire carries a BigFloat's exponent less its prec as an int32
// (pxf/bignum.proto), so a small value at a wide prec has no encoding, and
// protowire-go refuses it in these words (protowire-go#231). 0.125 is
// 2^-3, binary exponent -2, and -2 - (2^31 - 1) is below INT32_MIN. The
// refusal comes before the value is widened to prec bits, 256 MiB here, so
// this test costs what any other does.
TEST_F(PxfBigNum, BigFloatBelowTheWiresExponentRangeIsRefused) {
  protowire::pxf::UnmarshalOptions opts;
  opts.max_numeric_literal_digits = 2000000000;
  for (const char* lit : {"0.125", "-0.125", "1e-300", "3e-4900"}) {
    SCOPED_TRACE(lit);
    auto msg = NewWallet();
    auto st =
        protowire::pxf::Unmarshal(std::string("rate = ") + lit + "p2147483647\n", msg.get(), opts);
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find(std::string("big float literal ") + lit +
                                " is below the wire's exponent range"),
              std::string::npos)
        << st.ToString();
    // No exponent is stored, wrapped or otherwise.
    const auto& rate = msg->GetReflection()->GetMessage(*msg, desc_->FindFieldByName("rate"));
    EXPECT_EQ(rate.ByteSizeLong(), 0u);
  }
  // protowire-go asks this before MaxBigFloatExponent, so a value past both
  // gets this refusal: 0.125 has binary exponent -2, past a bound of 1.
  opts.max_big_float_exponent = 1;
  auto msg = NewWallet();
  auto st = protowire::pxf::Unmarshal("rate = 0.125p2147483647\n", msg.get(), opts);
  EXPECT_NE(st.message().find("is below the wire's exponent range"), std::string::npos)
      << st.ToString();
}

// DecimalTimes multiplies a decimal digit string by a small number.
std::string DecimalTimes(const std::string& d, uint32_t m) {
  std::string out;
  uint64_t carry = 0;
  for (size_t i = d.size(); i > 0; --i) {
    const uint64_t v = uint64_t(d[i - 1] - '0') * m + carry;
    out.push_back(static_cast<char>('0' + v % 10));
    carry = v / 10;
  }
  for (; carry != 0; carry /= 10) out.push_back(static_cast<char>('0' + carry % 10));
  while (out.size() > 1 && out.back() == '0') out.pop_back();
  return std::string(out.rbegin(), out.rend());
}

// DyadicLiteral writes m 2^j exactly: m 5^-j e j for a negative j. A
// non-zero nudge moves it by one unit in a decimal place past its last
// digit, down for a negative nudge and up for a positive one. That is
// 10^(j-1) for a negative j and 0.1 otherwise, either way less than 2^(j+1),
// so a tie nudged this way stays nearer the tie than any other boundary.
std::string DyadicLiteral(uint64_t m, int j, int nudge) {
  std::string d = std::to_string(m);
  int exp10 = 0;
  for (int i = 0; i < (j > 0 ? j : -j); ++i) d = DecimalTimes(d, j > 0 ? 2 : 5);
  if (j < 0) exp10 = j;
  if (nudge != 0) {
    // d - 0.1 or d + 0.1 in the last place: one more digit.
    if (nudge < 0) {
      size_t i = d.size();
      while (d[i - 1] == '0') d[--i] = '9';
      d[i - 1] = static_cast<char>(d[i - 1] - 1);
      d += "9";
    } else {
      d += "1";
    }
    --exp10;
  }
  return d + "e" + std::to_string(exp10);
}

// The parser asks both exponent limits of the value rounded to prec before
// it divides, from the literal's bit lengths and one comparison
// (#125). Rounding can carry the exponent one higher, and these cases are
// built around that carry, so their expected exponent does not come from
// the arithmetic under test: (2^(p+1) - 1) 2^(e-p-1) is the tie between the
// largest p-bit value below 2^e and 2^e itself, and rounds up to 2^e,
// binary exponent e + 1; a hair below the tie it stays at e, and a hair
// above it carries. Each is read, its exponent compared, and the bound
// probed at that exponent, which is where the early arithmetic answers.
TEST_F(PxfBigNum, BigFloatRoundedExponentIsFoundBeforeDividing) {
  const auto* rate_fd = desc_->FindFieldByName("rate");
  const auto binary_exponent = [&](const pbuf::Message& wallet) {
    const pbuf::Message& rate = wallet.GetReflection()->GetMessage(wallet, rate_fd);
    const pbuf::Descriptor* d = rate.GetDescriptor();
    const std::string m = rate.GetReflection()->GetString(rate, d->FindFieldByName("mantissa"));
    int64_t bits = 0;
    if (!m.empty()) {
      bits = int64_t(m.size() - 1) * 8;
      for (auto top = static_cast<uint8_t>(m[0]); top != 0; top >>= 1) ++bits;
    }
    return bits + rate.GetReflection()->GetInt32(rate, d->FindFieldByName("exponent"));
  };
  const auto check = [&](const std::string& lit, uint32_t prec, int64_t want) {
    SCOPED_TRACE(lit + "p" + std::to_string(prec));
    const std::string doc = "rate = " + lit + "p" + std::to_string(prec) + "\n";
    auto msg = NewWallet();
    auto st = protowire::pxf::Unmarshal(doc, msg.get());
    ASSERT_TRUE(st.ok()) << st.ToString();
    const int64_t bin = binary_exponent(*msg);
    ASSERT_EQ(bin, want);
    protowire::pxf::UnmarshalOptions at;
    at.max_big_float_exponent = static_cast<int>(bin < 0 ? -bin : bin);
    auto ok = NewWallet();
    EXPECT_TRUE(protowire::pxf::Unmarshal(doc, ok.get(), at).ok());
    // A bound of 0 means the default, so the refusal is probed only above 1.
    if (at.max_big_float_exponent <= 1) return;
    --at.max_big_float_exponent;
    auto past = NewWallet();
    auto refused = protowire::pxf::Unmarshal(doc, past.get(), at);
    EXPECT_NE(refused.message().find("has binary exponent " + std::to_string(bin) + ","),
              std::string::npos)
        << refused.ToString();
  };

  std::mt19937_64 rng(125);
  int cases = 0;
  for (int i = 0; i < 400; ++i) {
    const uint32_t p = 1 + static_cast<uint32_t>(rng() % 62);
    const int e = static_cast<int>(rng() % 601) - 300;
    const uint64_t top = (uint64_t{1} << (p + 1)) - 1;  // the tie's mantissa
    const int j = e - static_cast<int>(p) - 1;
    check(DyadicLiteral(top, j, 0), p, e + 1);
    check(DyadicLiteral(top, j, -1), p, e);
    check(DyadicLiteral(top, j, +1), p, e + 1);
    check(DyadicLiteral(1, e, 0), p, e + 1);  // 2^e itself
    cases += 4;
  }
  // Hand-picked: ties to even on either side, and no carry.
  check("15.5", 4, 5);  // 1111.1: the tie rounds up to 10000
  check("14.5", 4, 4);  // 1110.1: the tie rounds down to 1110
  check("0.75", 1, 1);  // 0.11: the tie rounds up to 1
  check("1023.4", 10, 10);
  check("1023.6", 10, 11);
  check("-0.99999", 3, 1);  // past the tie at 0.9375: rounds to 1
  cases += 6;
  EXPECT_EQ(cases, 1606);
}

}  // namespace

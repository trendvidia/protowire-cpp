// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Tests for pxf.BigInt / pxf.Decimal / pxf.BigFloat sugar.

#include "protowire/pxf.h"

#include "protowire/limits.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
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
  map<int32, string> labels = 4;
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

// protowire-go's TestBigFloatLiteral_NearTieReadsExactly (protowire-go#260).
// A literal is rounded from its exact value to prec bits, once, ties to even.
// This one is 2.5 - 1.7e-68 times 2^-16383, just below the tie, so at 2 bits
// it is 2 times 2^-16383: mantissa 0b10 at exponent -16383. Rounding twice,
// as big.Float.Parse did, read it as 3.
TEST_F(PxfBigNum, BigFloatNearTieReadsExactly) {
  auto msg = NewWallet();
  auto st = protowire::pxf::Unmarshal(
      "rate = "
      "4.2026289288901168828283472716521907532475991810580890501360340372609e-4932p2\n",
      msg.get());
  ASSERT_TRUE(st.ok()) << st.ToString();
  const pbuf::Message& rate =
      msg->GetReflection()->GetMessage(*msg, desc_->FindFieldByName("rate"));
  const pbuf::Descriptor* d = rate.GetDescriptor();
  const pbuf::Reflection* r = rate.GetReflection();
  EXPECT_EQ(r->GetString(rate, d->FindFieldByName("mantissa")), "\x02");
  EXPECT_EQ(r->GetInt32(rate, d->FindFieldByName("exponent")), -16383);
  EXPECT_EQ(r->GetUInt32(rate, d->FindFieldByName("prec")), 2u);
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
// probed next to that exponent, which is where the early arithmetic answers.
// protowire-go asks the bound first of big.Float.Parse's exponent, which a
// hair from the tie can be one past the exact value's (#144), so at the
// exponent itself the verdict is Go's, and testdata/bigfloat-literals pins
// it; one past, the literal is taken, and one short it is refused.
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
    at.max_big_float_exponent = static_cast<int>(bin < 0 ? -bin : bin) + 1;
    auto ok = NewWallet();
    EXPECT_TRUE(protowire::pxf::Unmarshal(doc, ok.get(), at).ok());
    // A bound of 0 means the default, so the refusal is probed only above 1.
    at.max_big_float_exponent -= 2;
    if (at.max_big_float_exponent < 1) return;
    auto past = NewWallet();
    auto refused = protowire::pxf::Unmarshal(doc, past.get(), at);
    EXPECT_NE(refused.message().find("has binary exponent "), std::string::npos)
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

// A literal far past MaxBigFloatExponent is refused in protowire-go's words
// and in its order (#144): big.Float's range first, then the wire's exponent
// range, then the bound, with the exponent big.Float.Parse gives it. None of
// these builds 10^exp10.
TEST_F(PxfBigNum, BigFloatFarPastTheBoundIsRefusedAsGoRefusesIt) {
  protowire::pxf::UnmarshalOptions wide;
  wide.max_numeric_literal_digits = 2000000000;
  const struct {
    const char* lit;
    bool wide;
    const char* want;
  } cases[] = {
      {"1e-100000p64", false, "has binary exponent -332192, and MaxBigFloatExponent=16384 bounds"},
      {"1e20000p64", false, "has binary exponent 66439, and MaxBigFloatExponent=16384 bounds"},
      {"1e4940p64", false, "has binary exponent 16411, and MaxBigFloatExponent=16384 bounds"},
      {"1e-100000p2147483647", true, "is below the wire's exponent range"},
      {"1e646456993p64", false, "is above big.Float's range"},
      {"1e-646456993p64", false, "is below the wire's exponent range"},
      {"1e-646456994p64", false, "is below big.Float's range"},
      {"1e-999999999", false, "is below big.Float's range"},
      {"1e-999999999999", false, "invalid big float: 1e-999999999999"},
      // These are answered from an estimate, in protowire-go's words. Within
      // a few powers of two of big.Float's range the estimate cannot answer,
      // and the power of five is built, at no more than 16384 bits
      // (protowire-go#274).
      {"1e-999999999p2147483647", true, "is below big.Float's range"},
      {"1e646457000p4294967295", true, "is above big.Float's range"},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.lit);
    auto msg = NewWallet();
    auto st = protowire::pxf::Unmarshal(std::string("rate = ") + c.lit + "\n",
                                        msg.get(),
                                        c.wide ? wide : protowire::pxf::UnmarshalOptions{});
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find(c.want), std::string::npos) << st.ToString();
  }
}

// big.Float.Parse is followed at no more than 16384 bits, as protowire-go
// follows it (protowire-go#274, #151): its cost grows with the square of
// the precision, and a raised digit cap admits a suffix of 2^31 bits. These
// literals took protowire-go minutes each, and get the answers they got
// then. The texts are protowire-go's, from its TestBigFloatWidePrecisionIsQuick.
TEST_F(PxfBigNum, BigFloatWidePrecisionIsQuick) {
  protowire::pxf::UnmarshalOptions wide;
  wide.max_numeric_literal_digits = 1300000000;  // admits p4294967295
  const std::pair<const char*, const char*> cases[] = {
      {"1e-300p2147483647", "big float literal 1e-300 is below the wire's exponent range"},
      {"1e646456993p2147483647", "big float literal 1e646456993 is above big.Float's range"},
      {"1e646456993p4294967295", "big float literal 1e646456993 is above big.Float's range"},
      {"1e-646456993p2147483647",
       "big float literal 1e-646456993 is below the wire's exponent range"},
      {"1e-646457000p2147483647", "big float literal 1e-646457000 is below big.Float's range"},
      {"1.5p4294967295",
       "big float literal 1.5: precision p4294967295 exceeds the widest mantissa big.Float can "
       "hold, 2147483647 bits"},
      {"1e5000p2147483647",
       "big float literal 1e5000 has binary exponent 16610, and MaxBigFloatExponent=16384 bounds "
       "its magnitude"},
  };
  for (const auto& [lit, want] : cases) {
    SCOPED_TRACE(lit);
    auto msg = NewWallet();
    const auto start = std::chrono::steady_clock::now();
    auto st = protowire::pxf::Unmarshal(std::string("rate = ") + lit + "\n", msg.get(), wide);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
    EXPECT_EQ(st.message(), want);
  }
}

// A literal a wide precision accepts is rounded exactly at that precision,
// not at the one Parse is followed at: 0.1 at 50 million bits is 0.1's
// binary expansion, 0011 repeating, rounded once.
TEST_F(PxfBigNum, BigFloatWidePrecisionValue) {
  constexpr uint32_t kPrec = 50000000;
  protowire::pxf::UnmarshalOptions wide;
  wide.max_numeric_literal_digits = 20000000;
  auto msg = NewWallet();
  const auto start = std::chrono::steady_clock::now();
  auto st =
      protowire::pxf::Unmarshal("rate = 0.1p" + std::to_string(kPrec) + "\n", msg.get(), wide);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
  ASSERT_TRUE(st.ok()) << st.ToString();
  const pbuf::Message& rate =
      msg->GetReflection()->GetMessage(*msg, desc_->FindFieldByName("rate"));
  const pbuf::Descriptor* d = rate.GetDescriptor();
  const pbuf::Reflection* r = rate.GetReflection();
  EXPECT_EQ(r->GetUInt32(rate, d->FindFieldByName("prec")), kPrec);
  // 0.1 = 0.000110011… in binary: mantissa 1100 1100 … (prec bits) × 2^(-3 - prec).
  // The bits after the 50000000th are 1100…, more than half, so it rounds up.
  EXPECT_EQ(r->GetInt32(rate, d->FindFieldByName("exponent")), -3 - static_cast<int32_t>(kPrec));
  const std::string m = r->GetString(rate, d->FindFieldByName("mantissa"));
  ASSERT_EQ(m.size(), kPrec / 8);
  EXPECT_EQ(static_cast<uint8_t>(m.front()), 0xcc);
  EXPECT_EQ(static_cast<uint8_t>(m[m.size() / 2]), 0xcc);
  EXPECT_EQ(static_cast<uint8_t>(m.back()), 0xcd);
}

// A precision suffix quoted in an error is clipped as protowire-go clips
// it: 24 bytes and an ellipsis.
TEST_F(PxfBigNum, PrecisionSuffixIsClippedInErrors) {
  const std::pair<const char*, const char*> cases[] = {
      {"price = 1p1234567890123456789012345678901\n",
       "precision suffix p123456789012345678901234… is allowed only on a pxf.BigFloat, not "
       "on field \"price\""},
      {"price = 1p12345\n",
       "precision suffix p12345 is allowed only on a pxf.BigFloat, not on field \"price\""},
      {"labels = { 1p1234567890123456789012345678901: \"x\" }\n",
       "map field \"labels\": a key cannot carry a precision suffix "
       "(p123456789012345678901234…)"},
  };
  for (const auto& [doc, want] : cases) {
    auto msg = NewWallet();
    auto st = protowire::pxf::Unmarshal(doc, msg.get());
    EXPECT_EQ(st.message(), want) << doc;
  }
}

// testdata/bigfloat-literals/go_sweep.tsv is protowire-go's verdict on
// literals near powers of ten, near powers of two and at rounding carries,
// at precisions from p1 to p13603 and under several bounds: the error text,
// or the fields it stored. Each row is max_exp, max_digits, literal, result.
// The literals were built by a Go program from their construction, not from
// either port's arithmetic, by tools/bigfloat_sweep, whose header says how to
// regenerate the table.
TEST_F(PxfBigNum, BigFloatLiteralsMatchProtowireGo) {
  std::ifstream in(std::string(TESTDATA_DIR) + "/bigfloat-literals/go_sweep.tsv");
  ASSERT_TRUE(in) << "testdata/bigfloat-literals/go_sweep.tsv";
  const auto* rate_fd = desc_->FindFieldByName("rate");
  int rows = 0;
  int refused = 0;
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] == '#') continue;
    const size_t a = line.find('\t');
    const size_t b = line.find('\t', a + 1);
    const size_t c = line.find('\t', b + 1);
    ASSERT_NE(c, std::string::npos) << line;
    const std::string lit = line.substr(b + 1, c - b - 1);
    const std::string want = line.substr(c + 1);
    SCOPED_TRACE(lit);
    protowire::pxf::UnmarshalOptions opts;
    opts.max_big_float_exponent = std::stoi(line.substr(0, a));
    opts.max_numeric_literal_digits = std::stoi(line.substr(a + 1, b - a - 1));
    auto msg = NewWallet();
    auto st = protowire::pxf::Unmarshal("rate = " + lit + "\n", msg.get(), opts);
    std::string got;
    if (!st.ok()) {
      // Go's error text leads with the position, as check_decode prints it.
      got = "err ";
      if (const auto pos = st.position()) {
        got += std::to_string(pos->first) + ":" + std::to_string(pos->second) + ": ";
      }
      got += st.message();
      ++refused;
    } else {
      const pbuf::Message& rate = msg->GetReflection()->GetMessage(*msg, rate_fd);
      const pbuf::Descriptor* d = rate.GetDescriptor();
      const pbuf::Reflection* r = rate.GetReflection();
      static constexpr char kHex[] = "0123456789abcdef";
      const std::string mantissa = r->GetString(rate, d->FindFieldByName("mantissa"));
      std::string hex;
      if (mantissa.size() > 32) {
        // A wide mantissa is its length and FNV-1a 64 hash, to keep the
        // table small.
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char ch : mantissa) h = (h ^ ch) * 0x100000001b3ULL;
        char buf[64];
        std::snprintf(buf,
                      sizeof(buf),
                      "%zuB-fnv1a64-%016llx",
                      mantissa.size(),
                      static_cast<unsigned long long>(h));
        hex = buf;
      } else {
        for (unsigned char ch : mantissa) {
          hex.push_back(kHex[ch / 16]);
          hex.push_back(kHex[ch % 16]);
        }
      }
      got = "ok " + hex + " " + std::to_string(r->GetInt32(rate, d->FindFieldByName("exponent"))) +
            " " + std::to_string(r->GetUInt32(rate, d->FindFieldByName("prec"))) + " " +
            (r->GetBool(rate, d->FindFieldByName("negative")) ? "true" : "false");
    }
    EXPECT_EQ(got, want);
    ++rows;
  }
  // A table that did not load cannot pass.
  EXPECT_GT(rows, 3000);
  EXPECT_GT(refused, 1000);
}

}  // namespace

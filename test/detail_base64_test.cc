#include "protowire/detail/base64.h"

#include <gtest/gtest.h>

namespace {

using protowire::detail::Base64DecodeStd;
using protowire::detail::Base64EncodeStd;

TEST(Base64, RoundTripBasic) {
  std::vector<uint8_t> in = {'H', 'e', 'l', 'l', 'o'};
  std::string enc = Base64EncodeStd(in);
  EXPECT_EQ(enc, "SGVsbG8=");
  auto dec = Base64DecodeStd(enc);
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(*dec, in);
}

TEST(Base64, EmptyRoundTrip) {
  EXPECT_EQ(Base64EncodeStd(""), "");
  auto dec = Base64DecodeStd("");
  ASSERT_TRUE(dec.has_value());
  EXPECT_TRUE(dec->empty());
}

TEST(Base64, AcceptsRawStd) {
  // "Hello" without padding.
  auto dec = Base64DecodeStd("SGVsbG8");
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(std::string(dec->begin(), dec->end()), "Hello");
}

TEST(Base64, RejectsInvalidChar) {
  EXPECT_FALSE(Base64DecodeStd("###=").has_value());
}

TEST(Base64, AllByteValues) {
  std::vector<uint8_t> in;
  for (int i = 0; i < 256; ++i) in.push_back(static_cast<uint8_t>(i));
  std::string enc = Base64EncodeStd(in);
  auto dec = Base64DecodeStd(enc);
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(*dec, in);
}

}  // namespace

#include "protowire/detail/duration.h"

#include <gtest/gtest.h>

namespace {

using protowire::detail::FormatDuration;
using protowire::detail::ParseDuration;

TEST(Duration, ParseSingleUnit) {
  auto d = ParseDuration("30s");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->seconds, 30);
  EXPECT_EQ(d->nanos, 0);
}

TEST(Duration, ParseCompound) {
  auto d = ParseDuration("1h30m45s");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->total_nanos(), (3600LL + 30 * 60 + 45) * 1'000'000'000LL);
}

TEST(Duration, ParseFractional) {
  auto d = ParseDuration("1.5h");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->total_nanos(), 5400LL * 1'000'000'000LL);
}

TEST(Duration, ParseSubsecond) {
  auto d = ParseDuration("250ms");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->total_nanos(), 250'000'000);
}

TEST(Duration, ParseZero) {
  auto d = ParseDuration("0");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->total_nanos(), 0);
}

TEST(Duration, ParseNegative) {
  auto d = ParseDuration("-1h30m");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->total_nanos(), -5400LL * 1'000'000'000LL);
}

TEST(Duration, FormatRoundTrip) {
  EXPECT_EQ(FormatDuration(0, 0), "0s");
  EXPECT_EQ(FormatDuration(30, 0), "30s");
  EXPECT_EQ(FormatDuration(0, 250'000'000), "250ms");
  EXPECT_EQ(FormatDuration(3600 + 30 * 60 + 45, 0), "1h30m45s");
}

TEST(Duration, RejectsBadInputs) {
  EXPECT_FALSE(ParseDuration("").has_value());
  EXPECT_FALSE(ParseDuration("30").has_value());
  EXPECT_FALSE(ParseDuration("hello").has_value());
}

}  // namespace

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
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

// google.protobuf.Duration requires a non-zero nanos to carry the sign of
// the value; the parser used to normalise nanos into [0, 1e9) instead, so
// -1ns came out as seconds=-1, nanos=999999999 (#20).
TEST(Duration, ParseNegativeSplitsTowardZero) {
  auto d = protowire::detail::ParseDuration("-1.5s");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->seconds, -1);
  EXPECT_EQ(d->nanos, -500'000'000);
  d = protowire::detail::ParseDuration("-1ns");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->seconds, 0);
  EXPECT_EQ(d->nanos, -1);
  d = protowire::detail::ParseDuration("-312.5µs");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->seconds, 0);
  EXPECT_EQ(d->nanos, -312'500);
  EXPECT_EQ(protowire::detail::FormatDuration(d->seconds, d->nanos), "-312.5µs");
}

// The int64 edges format as time.Duration.String() does; INT64_MIN used
// to come out as a placeholder because its negation overflows (#20).
TEST(Duration, FormatInt64Edges) {
  EXPECT_EQ(FormatDuration(INT64_MAX / 1'000'000'000LL,
                           static_cast<int32_t>(INT64_MAX % 1'000'000'000LL)),
            "2562047h47m16.854775807s");
  EXPECT_EQ(FormatDuration(INT64_MIN / 1'000'000'000LL,
                           static_cast<int32_t>(INT64_MIN % 1'000'000'000LL)),
            "-2562047h47m16.854775808s");
}

// Both int64 edges read back, and one nanosecond past either does not.
TEST(Duration, ParseInt64Edges) {
  auto d = ParseDuration("2562047h47m16.854775807s");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->total_nanos(), INT64_MAX);
  d = ParseDuration("-2562047h47m16.854775808s");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->seconds, INT64_MIN / 1'000'000'000LL);
  EXPECT_EQ(d->nanos, static_cast<int32_t>(INT64_MIN % 1'000'000'000LL));
  EXPECT_FALSE(ParseDuration("2562047h47m16.854775808s").has_value());
  EXPECT_FALSE(ParseDuration("-2562047h47m16.854775809s").has_value());
  EXPECT_FALSE(ParseDuration("9223372036854775808ns").has_value());
}

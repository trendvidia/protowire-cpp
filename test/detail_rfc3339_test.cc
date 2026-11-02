// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/detail/rfc3339.h"

#include <gtest/gtest.h>

namespace {

using protowire::detail::FormatRFC3339Nano;
using protowire::detail::ParseRFC3339;

TEST(RFC3339, ParseUTC) {
  auto t = ParseRFC3339("2024-01-15T10:30:00Z");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->seconds, 1705314600);
  EXPECT_EQ(t->nanos, 0);
}

TEST(RFC3339, ParseFractional) {
  auto t = ParseRFC3339("2024-01-15T10:30:00.123456789Z");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->seconds, 1705314600);
  EXPECT_EQ(t->nanos, 123456789);
}

TEST(RFC3339, ParseOffset) {
  auto t = ParseRFC3339("2024-01-15T12:30:00+02:00");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->seconds, 1705314600);  // same instant as 10:30 UTC
}

TEST(RFC3339, FormatRoundTrip) {
  EXPECT_EQ(FormatRFC3339Nano(1705314600, 0), "2024-01-15T10:30:00Z");
  EXPECT_EQ(FormatRFC3339Nano(1705314600, 123000000), "2024-01-15T10:30:00.123Z");
  EXPECT_EQ(FormatRFC3339Nano(1705314600, 123456789), "2024-01-15T10:30:00.123456789Z");
  EXPECT_EQ(FormatRFC3339Nano(1705314600, 500000000), "2024-01-15T10:30:00.5Z");
  EXPECT_EQ(FormatRFC3339Nano(1705314600, 120000), "2024-01-15T10:30:00.00012Z");
}

TEST(RFC3339, RejectsBadInputs) {
  EXPECT_FALSE(ParseRFC3339("").has_value());
  EXPECT_FALSE(ParseRFC3339("not-a-date").has_value());
  EXPECT_FALSE(ParseRFC3339("2024-01-15").has_value());
}

}  // namespace

// time.Parse is what these follow, and it differs from the RFC.
TEST(RFC3339, MatchesGoTimeParse) {
  using protowire::detail::ParseRFC3339;
  EXPECT_EQ(ParseRFC3339("2024-01-02T3:04:05Z")->seconds, 1704164645);
  EXPECT_EQ(ParseRFC3339("2024-01-02T3:04:05.5Z")->nanos, 500000000);
  EXPECT_EQ(ParseRFC3339("2024-01-02T03:04:05,5Z")->nanos, 500000000);
  EXPECT_EQ(ParseRFC3339("2024-01-02T03:04:05.1234567890123Z")->nanos, 123456789);
  EXPECT_EQ(ParseRFC3339("2024-01-02T03:04:05+24:00")->seconds, 1704078245);
  EXPECT_EQ(ParseRFC3339("2024-01-02T03:04:05+23:60")->seconds, 1704078245);
  for (const char* bad : {"2024-01-02t03:04:05Z",
                          "2024-01-02T03:04:05z",
                          "2024-01-02T03:4:05Z",
                          "2024-01-02T03:04:5Z",
                          "2024-01-02T03:04:05.Z",
                          "2024-01-02T03:04:05+25:00",
                          "2024-01-02T03:04:05+23:61",
                          "2024-01-02T03:04:05+0:00"}) {
    EXPECT_FALSE(ParseRFC3339(bad).has_value()) << bad;
  }
}

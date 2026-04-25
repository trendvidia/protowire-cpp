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
  EXPECT_EQ(FormatRFC3339Nano(1705314600, 123000000),
            "2024-01-15T10:30:00.123Z");
  EXPECT_EQ(FormatRFC3339Nano(1705314600, 123456789),
            "2024-01-15T10:30:00.123456789Z");
}

TEST(RFC3339, RejectsBadInputs) {
  EXPECT_FALSE(ParseRFC3339("").has_value());
  EXPECT_FALSE(ParseRFC3339("not-a-date").has_value());
  EXPECT_FALSE(ParseRFC3339("2024-01-15").has_value());
}

}  // namespace

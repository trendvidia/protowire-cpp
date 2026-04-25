#include "protowire/envelope.h"

#include <gtest/gtest.h>

namespace {

using protowire::envelope::AppError;
using protowire::envelope::Envelope;
using protowire::envelope::FieldError;

TEST(Envelope, OKBuilder) {
  auto e = Envelope::OK(200, {0x01, 0x02});
  EXPECT_TRUE(e.IsOK());
  EXPECT_FALSE(e.IsTransportError());
  EXPECT_FALSE(e.IsAppError());
  EXPECT_EQ(e.status, 200);
  EXPECT_EQ(e.data.size(), 2u);
  EXPECT_EQ(e.ErrorCode(), "");
}

TEST(Envelope, ErrBuilderAndChaining) {
  auto e = Envelope::Err(400, "INVALID", "bad input", {"name"});
  EXPECT_FALSE(e.IsOK());
  EXPECT_TRUE(e.IsAppError());
  ASSERT_NE(e.error, nullptr);
  EXPECT_EQ(e.ErrorCode(), "INVALID");

  e.error->WithField("name", "REQUIRED", "name is required")
      .WithMeta("trace_id", "abc123");
  EXPECT_EQ(e.error->details.size(), 1u);
  EXPECT_EQ(e.error->metadata.at("trace_id"), "abc123");

  auto m = e.FieldErrors();
  ASSERT_TRUE(m.contains("name"));
  EXPECT_EQ(m["name"]->code, "REQUIRED");
}

TEST(Envelope, TransportErr) {
  auto e = Envelope::TransportErr("connection refused");
  EXPECT_TRUE(e.IsTransportError());
  EXPECT_FALSE(e.IsOK());
  EXPECT_FALSE(e.IsAppError());
}

TEST(Envelope, RoundTripStatusAndData) {
  // Round-trip the basic fields (status, transport_error, data) — the wire
  // layout matches the Go envelope for those three. AppError is exercised
  // separately as a standalone struct.
  Envelope orig = Envelope::OK(201, {0xAA, 0xBB, 0xCC});
  auto bytes = protowire::pb::Marshal(orig);

  Envelope got;
  ASSERT_TRUE(protowire::pb::Unmarshal(bytes, got).ok());
  EXPECT_EQ(got.status, 201);
  EXPECT_EQ(got.data, orig.data);
}

TEST(Envelope, AppErrorRoundTrip) {
  AppError ae{"PERMISSION_DENIED", "no access", {"role"}, {}, {}};
  ae.WithField("role", "FORBIDDEN", "role mismatch", {"admin"});

  auto bytes = protowire::pb::Marshal(ae);
  AppError got;
  ASSERT_TRUE(protowire::pb::Unmarshal(bytes, got).ok());
  EXPECT_EQ(got.code, "PERMISSION_DENIED");
  EXPECT_EQ(got.message, "no access");
  ASSERT_EQ(got.details.size(), 1u);
  EXPECT_EQ(got.details[0].field, "role");
  EXPECT_EQ(got.details[0].code, "FORBIDDEN");
}

}  // namespace

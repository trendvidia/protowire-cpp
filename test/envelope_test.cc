// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
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

  e.error->WithField("name", "REQUIRED", "name is required").WithMeta("trace_id", "abc123");
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
  Envelope orig = Envelope::OK(201, {0xAA, 0xBB, 0xCC});
  auto bytes = protowire::pb::Marshal(orig);

  Envelope got;
  ASSERT_TRUE(protowire::pb::Unmarshal(bytes, got).ok());
  EXPECT_EQ(got.status, 201);
  EXPECT_EQ(got.data, orig.data);
  EXPECT_TRUE(got.IsOK());
}

TEST(Envelope, AppErrorStandaloneRoundTrip) {
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

TEST(Envelope, AppErrorMetadataRoundTrip) {
  // Exercises pb's map<string,string> support — previously dropped silently.
  AppError ae{"VALIDATION", "fields invalid", {}, {}, {}};
  ae.WithMeta("region", "us-east").WithMeta("retry_after", "30");

  auto bytes = protowire::pb::Marshal(ae);
  AppError got;
  ASSERT_TRUE(protowire::pb::Unmarshal(bytes, got).ok());
  EXPECT_EQ(got.code, "VALIDATION");
  ASSERT_EQ(got.metadata.size(), 2u);
  EXPECT_EQ(got.metadata.at("region"), "us-east");
  EXPECT_EQ(got.metadata.at("retry_after"), "30");
}

TEST(Envelope, FullEnvelopeRoundTripWithAppErrorAndMetadata) {
  // Exercises both gaps that previously caused C++ to silently drop fields:
  // unique_ptr<AppError> in Envelope and map<string,string> in AppError.
  Envelope orig = Envelope::Err(402, "INSUFFICIENT_FUNDS", "balance too low", {"$3.50", "$10.00"});
  orig.error->WithField("amount", "MIN_VALUE", "below minimum", {"10.00"});
  orig.error->WithField("currency", "INVALID", "unsupported currency");
  orig.error->WithMeta("request_id", "req-123");
  orig.error->WithMeta("retry_after", "30");

  auto bytes = protowire::pb::Marshal(orig);

  Envelope got;
  ASSERT_TRUE(protowire::pb::Unmarshal(bytes, got).ok());

  EXPECT_EQ(got.status, 402);
  ASSERT_NE(got.error, nullptr);
  EXPECT_EQ(got.error->code, "INSUFFICIENT_FUNDS");
  EXPECT_EQ(got.error->message, "balance too low");
  EXPECT_EQ(got.error->args, std::vector<std::string>({"$3.50", "$10.00"}));
  ASSERT_EQ(got.error->details.size(), 2u);
  EXPECT_EQ(got.error->details[0].field, "amount");
  EXPECT_EQ(got.error->details[0].code, "MIN_VALUE");
  EXPECT_EQ(got.error->details[0].args, std::vector<std::string>({"10.00"}));
  EXPECT_EQ(got.error->details[1].field, "currency");
  EXPECT_EQ(got.error->details[1].code, "INVALID");
  ASSERT_EQ(got.error->metadata.size(), 2u);
  EXPECT_EQ(got.error->metadata.at("request_id"), "req-123");
  EXPECT_EQ(got.error->metadata.at("retry_after"), "30");
  EXPECT_TRUE(got.IsAppError());
}

TEST(Envelope, ZeroEnvelopeRoundTrip) {
  Envelope orig;
  auto bytes = protowire::pb::Marshal(orig);
  EXPECT_EQ(bytes.size(), 0u);

  Envelope got;
  ASSERT_TRUE(protowire::pb::Unmarshal(bytes, got).ok());
  EXPECT_TRUE(got.IsOK());
  EXPECT_EQ(got.status, 0);
  EXPECT_TRUE(got.transport_error.empty());
  EXPECT_TRUE(got.data.empty());
  EXPECT_EQ(got.error, nullptr);
}

TEST(Envelope, TransportErrRoundTrip) {
  Envelope orig = Envelope::TransportErr("connection refused");
  auto bytes = protowire::pb::Marshal(orig);

  Envelope got;
  ASSERT_TRUE(protowire::pb::Unmarshal(bytes, got).ok());
  EXPECT_TRUE(got.IsTransportError());
  EXPECT_EQ(got.transport_error, "connection refused");
  EXPECT_EQ(got.error, nullptr);
}

}  // namespace

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Tests for (pxf.required) and (pxf.default) annotation enforcement in
// UnmarshalFull. Mirrors protowire-go's null/required/default behavior.

#include "protowire/pxf.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

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

constexpr const char* kProto = R"(
syntax = "proto3";
package anntest.v1;
import "pxf/annotations.proto";
import "pxf/bignum.proto";
import "google/protobuf/timestamp.proto";
import "google/protobuf/duration.proto";
import "google/protobuf/wrappers.proto";

message Cfg {
  string name     = 1 [(pxf.required) = true];
  string role     = 2 [(pxf.default) = "viewer"];
  int32  priority = 3 [(pxf.default) = "5"];
  bool   enabled  = 4 [(pxf.default) = "true"];

  // Well-known message-typed defaults.
  google.protobuf.Timestamp   created_at = 5
      [(pxf.default) = "2024-01-15T10:30:00Z"];
  google.protobuf.Duration    timeout    = 6
      [(pxf.default) = "1h30m"];
  google.protobuf.StringValue label      = 7
      [(pxf.default) = "default"];
  google.protobuf.Int32Value  shards     = 8
      [(pxf.default) = "3"];
  google.protobuf.BoolValue   active     = 9
      [(pxf.default) = "true"];

  // pxf.* big-number defaults.
  pxf.BigInt   big_count = 10
      [(pxf.default) = "12345678901234567890"];
  pxf.Decimal  price     = 11
      [(pxf.default) = "10.50"];
  pxf.BigFloat ratio     = 12
      [(pxf.default) = "1.5"];
}
)";

class PxfAnnotations : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", WKT_PROTO_DIR);
    source_tree_.MapPath("", PROTO_DIR);
    source_tree_.MapPath("", testing::TempDir());
    std::string proto_path = std::string(testing::TempDir()) + "/cfg.proto";
    FILE* f = std::fopen(proto_path.c_str(), "w");
    std::fwrite(kProto, 1, std::strlen(kProto), f);
    std::fclose(f);

    importer_ = std::make_unique<pbuf::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("cfg.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("anntest.v1.Cfg");
    ASSERT_NE(desc_, nullptr);
    factory_ = std::make_unique<pbuf::DynamicMessageFactory>(importer_->pool());
  }

  std::unique_ptr<pbuf::Message> NewCfg() {
    return std::unique_ptr<pbuf::Message>(factory_->GetPrototype(desc_)->New());
  }

  pbuf::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pbuf::compiler::Importer> importer_;
  const pbuf::FileDescriptor* file_ = nullptr;
  const pbuf::Descriptor* desc_ = nullptr;
  std::unique_ptr<pbuf::DynamicMessageFactory> factory_;
};

TEST_F(PxfAnnotations, RequiredMissingErrors) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(role = "admin")", msg.get());
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("required"), std::string::npos)
      << "expected 'required' in error, got: " << r.status().message();
  EXPECT_NE(r.status().message().find("name"), std::string::npos);
}

TEST_F(PxfAnnotations, RequiredNullCountsAsPresent) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(name = null)", msg.get());
  EXPECT_TRUE(r.ok()) << r.status().message();
}

TEST_F(PxfAnnotations, DefaultsAppliedWhenAbsent) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(name = "Alice")", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();

  const pbuf::Reflection* refl = msg->GetReflection();
  EXPECT_EQ(refl->GetString(*msg, desc_->FindFieldByName("role")), "viewer");
  EXPECT_EQ(refl->GetInt32(*msg, desc_->FindFieldByName("priority")), 5);
  EXPECT_EQ(refl->GetBool(*msg, desc_->FindFieldByName("enabled")), true);
}

TEST_F(PxfAnnotations, DefaultNotAppliedWhenNull) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(
name = "Alice"
role = null
)",
                                         msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();

  const pbuf::Reflection* refl = msg->GetReflection();
  // role is null → default NOT applied; field stays at proto3 zero value.
  EXPECT_EQ(refl->GetString(*msg, desc_->FindFieldByName("role")), "");
  // priority and enabled are absent → defaults DO apply.
  EXPECT_EQ(refl->GetInt32(*msg, desc_->FindFieldByName("priority")), 5);
  EXPECT_EQ(refl->GetBool(*msg, desc_->FindFieldByName("enabled")), true);
}

TEST_F(PxfAnnotations, DefaultNotAppliedWhenSet) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(
name = "Alice"
role = "admin"
priority = 10
)",
                                         msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();

  const pbuf::Reflection* refl = msg->GetReflection();
  EXPECT_EQ(refl->GetString(*msg, desc_->FindFieldByName("role")), "admin");
  EXPECT_EQ(refl->GetInt32(*msg, desc_->FindFieldByName("priority")), 10);
}

TEST_F(PxfAnnotations, TimestampDefaultApplied) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(name = "Alice")", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();

  const pbuf::Reflection* refl = msg->GetReflection();
  const auto* fd = desc_->FindFieldByName("created_at");
  ASSERT_TRUE(refl->HasField(*msg, fd));
  const auto& ts = refl->GetMessage(*msg, fd);
  const auto* ts_desc = ts.GetDescriptor();
  // 2024-01-15T10:30:00Z = 1705314600 unix seconds, 0 nanos.
  EXPECT_EQ(ts.GetReflection()->GetInt64(ts, ts_desc->FindFieldByName("seconds")), 1705314600);
  EXPECT_EQ(ts.GetReflection()->GetInt32(ts, ts_desc->FindFieldByName("nanos")), 0);
}

TEST_F(PxfAnnotations, DurationDefaultApplied) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(name = "Alice")", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();

  const pbuf::Reflection* refl = msg->GetReflection();
  const auto* fd = desc_->FindFieldByName("timeout");
  ASSERT_TRUE(refl->HasField(*msg, fd));
  const auto& dur = refl->GetMessage(*msg, fd);
  const auto* dur_desc = dur.GetDescriptor();
  // 1h30m = 5400 seconds.
  EXPECT_EQ(dur.GetReflection()->GetInt64(dur, dur_desc->FindFieldByName("seconds")), 5400);
}

TEST_F(PxfAnnotations, WrapperDefaultsApplied) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(name = "Alice")", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();

  const pbuf::Reflection* refl = msg->GetReflection();

  const auto* label_fd = desc_->FindFieldByName("label");
  ASSERT_TRUE(refl->HasField(*msg, label_fd));
  const auto& label = refl->GetMessage(*msg, label_fd);
  EXPECT_EQ(
      label.GetReflection()->GetString(label, label.GetDescriptor()->FindFieldByName("value")),
      "default");

  const auto* shards_fd = desc_->FindFieldByName("shards");
  ASSERT_TRUE(refl->HasField(*msg, shards_fd));
  const auto& shards = refl->GetMessage(*msg, shards_fd);
  EXPECT_EQ(
      shards.GetReflection()->GetInt32(shards, shards.GetDescriptor()->FindFieldByName("value")),
      3);

  const auto* active_fd = desc_->FindFieldByName("active");
  ASSERT_TRUE(refl->HasField(*msg, active_fd));
  const auto& active = refl->GetMessage(*msg, active_fd);
  EXPECT_EQ(
      active.GetReflection()->GetBool(active, active.GetDescriptor()->FindFieldByName("value")),
      true);
}

TEST_F(PxfAnnotations, BigNumberDefaultsApplied) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(name = "Alice")", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();

  const pbuf::Reflection* refl = msg->GetReflection();

  // Just assert the message was populated; the exact byte representation is
  // covered by pxf_bignum_test.
  for (const char* fname : {"big_count", "price", "ratio"}) {
    const auto* fd = desc_->FindFieldByName(fname);
    ASSERT_NE(fd, nullptr) << fname;
    EXPECT_TRUE(refl->HasField(*msg, fd)) << fname;
  }
}

TEST_F(PxfAnnotations, MessageDefaultNotAppliedWhenNull) {
  // null on a Timestamp field counts as present → default must NOT apply.
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(
name = "Alice"
created_at = null
)",
                                         msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();

  const pbuf::Reflection* refl = msg->GetReflection();
  const auto* fd = desc_->FindFieldByName("created_at");
  EXPECT_FALSE(refl->HasField(*msg, fd));
}

}  // namespace

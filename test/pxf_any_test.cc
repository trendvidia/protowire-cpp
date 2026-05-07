// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Tests for google.protobuf.Any sugar (block syntax with @type = "...").

#include "protowire/pxf.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <google/protobuf/any.pb.h>
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

// Schema with one Any field plus an inner detail message.
constexpr const char* kProto = R"(
syntax = "proto3";
package any.test;
import "google/protobuf/any.proto";

message ErrorDetail {
  int32  code   = 1;
  string reason = 2;
}

message Event {
  string id = 1;
  google.protobuf.Any payload = 2;
}
)";

// Resolver that knows about ErrorDetail by full name (any URL prefix is fine).
class FixedResolver : public protowire::pxf::TypeResolver {
 public:
  FixedResolver(const pbuf::DescriptorPool* pool) : pool_(pool) {}
  const pbuf::Descriptor* FindMessageByURL(std::string_view url) override {
    // Strip optional prefix up to last '/' — type URLs commonly look like
    // "type.googleapis.com/pkg.Type" but we also accept bare names.
    auto pos = url.find_last_of('/');
    std::string_view name = pos == std::string_view::npos ? url : url.substr(pos + 1);
    return pool_->FindMessageTypeByName(std::string(name));
  }

 private:
  const pbuf::DescriptorPool* pool_;
};

class PxfAny : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", WKT_PROTO_DIR);
    source_tree_.MapPath("", testing::TempDir());
    std::string proto_path = std::string(testing::TempDir()) + "/event.proto";
    FILE* f = std::fopen(proto_path.c_str(), "w");
    std::fwrite(kProto, 1, std::strlen(kProto), f);
    std::fclose(f);

    importer_ = std::make_unique<pbuf::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("event.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("any.test.Event");
    detail_desc_ = importer_->pool()->FindMessageTypeByName("any.test.ErrorDetail");
    ASSERT_NE(desc_, nullptr);
    ASSERT_NE(detail_desc_, nullptr);
    factory_ = std::make_unique<pbuf::DynamicMessageFactory>(importer_->pool());
    resolver_ = std::make_unique<FixedResolver>(importer_->pool());
  }

  std::unique_ptr<pbuf::Message> NewEvent() {
    return std::unique_ptr<pbuf::Message>(factory_->GetPrototype(desc_)->New());
  }

  pbuf::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pbuf::compiler::Importer> importer_;
  const pbuf::FileDescriptor* file_ = nullptr;
  const pbuf::Descriptor* desc_ = nullptr;
  const pbuf::Descriptor* detail_desc_ = nullptr;
  std::unique_ptr<pbuf::DynamicMessageFactory> factory_;
  std::unique_ptr<FixedResolver> resolver_;
};

TEST_F(PxfAny, DecodeAnySugar) {
  auto msg = NewEvent();
  protowire::pxf::UnmarshalOptions opts;
  opts.type_resolver = resolver_.get();
  ASSERT_TRUE(protowire::pxf::Unmarshal(R"(
id = "evt-1"
payload {
  @type = "any.test.ErrorDetail"
  code = 42
  reason = "not found"
}
)",
                                        msg.get(),
                                        opts)
                  .ok());

  // Verify the Any was packed correctly.
  const pbuf::Reflection* r = msg->GetReflection();
  const pbuf::Message& any = r->GetMessage(*msg, desc_->FindFieldByName("payload"));
  std::string scratch;
  EXPECT_EQ(any.GetReflection()->GetStringReference(
                any, any.GetDescriptor()->FindFieldByName("type_url"), &scratch),
            "any.test.ErrorDetail");
  // Decode the inner bytes back through the schema.
  const std::string& packed = any.GetReflection()->GetStringReference(
      any, any.GetDescriptor()->FindFieldByName("value"), &scratch);
  std::unique_ptr<pbuf::Message> inner(factory_->GetPrototype(detail_desc_)->New());
  ASSERT_TRUE(inner->ParseFromString(packed));
  EXPECT_EQ(inner->GetReflection()->GetInt32(*inner, detail_desc_->FindFieldByName("code")), 42);
  EXPECT_EQ(inner->GetReflection()->GetStringReference(
                *inner, detail_desc_->FindFieldByName("reason"), &scratch),
            "not found");
}

TEST_F(PxfAny, EncodeAnySugar) {
  auto msg = NewEvent();
  const pbuf::Reflection* r = msg->GetReflection();
  r->SetString(msg.get(), desc_->FindFieldByName("id"), "evt-1");

  // Build the inner ErrorDetail and pack into Any.
  std::unique_ptr<pbuf::Message> inner(factory_->GetPrototype(detail_desc_)->New());
  inner->GetReflection()->SetInt32(inner.get(), detail_desc_->FindFieldByName("code"), 7);
  inner->GetReflection()->SetString(inner.get(), detail_desc_->FindFieldByName("reason"), "boom");
  std::string packed;
  ASSERT_TRUE(inner->SerializeToString(&packed));

  pbuf::Message* any = r->MutableMessage(msg.get(), desc_->FindFieldByName("payload"));
  any->GetReflection()->SetString(
      any, any->GetDescriptor()->FindFieldByName("type_url"), "any.test.ErrorDetail");
  any->GetReflection()->SetString(any, any->GetDescriptor()->FindFieldByName("value"), packed);

  protowire::pxf::MarshalOptions mopts;
  mopts.type_resolver = resolver_.get();
  auto out = protowire::pxf::Marshal(*msg, mopts);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_NE(out->find("@type = \"any.test.ErrorDetail\""), std::string::npos);
  EXPECT_NE(out->find("code = 7"), std::string::npos);
  EXPECT_NE(out->find("reason = \"boom\""), std::string::npos);
}

TEST_F(PxfAny, RoundTrip) {
  protowire::pxf::UnmarshalOptions uopts;
  uopts.type_resolver = resolver_.get();
  protowire::pxf::MarshalOptions mopts;
  mopts.type_resolver = resolver_.get();

  auto msg1 = NewEvent();
  ASSERT_TRUE(protowire::pxf::Unmarshal(R"(
id = "evt-2"
payload {
  @type = "any.test.ErrorDetail"
  code = 99
  reason = "rt"
}
)",
                                        msg1.get(),
                                        uopts)
                  .ok());
  auto encoded = protowire::pxf::Marshal(*msg1, mopts);
  ASSERT_TRUE(encoded.ok());

  auto msg2 = NewEvent();
  ASSERT_TRUE(protowire::pxf::Unmarshal(*encoded, msg2.get(), uopts).ok());

  std::string s1, s2;
  msg1->SerializeToString(&s1);
  msg2->SerializeToString(&s2);
  EXPECT_EQ(s1, s2);
}

}  // namespace

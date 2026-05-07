// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// PXF integration tests using a runtime-compiled schema.
//
// We compile testdata/test.proto via libprotobuf's Importer, look up the
// AllTypes message descriptor, and exercise round-tripping through PXF.

#include "protowire/pxf.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <memory>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;

class SilentErrorCollector : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

class PxfDecode : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", TESTDATA_DIR);
    // Well-known types ship next to protoc — needed because test.proto
    // imports timestamp.proto, duration.proto, and wrappers.proto.
    source_tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("test.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
    desc_ = importer_->pool()->FindMessageTypeByName("test.v1.AllTypes");
    ASSERT_NE(desc_, nullptr);
    nested_desc_ = importer_->pool()->FindMessageTypeByName("test.v1.Nested");
    ASSERT_NE(nested_desc_, nullptr);
  }

  std::unique_ptr<pb::Message> NewAllTypes() {
    return std::unique_ptr<pb::Message>(factory_->GetPrototype(desc_)->New());
  }

  pb::compiler::DiskSourceTree source_tree_;
  SilentErrorCollector errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  const pb::Descriptor* desc_ = nullptr;
  const pb::Descriptor* nested_desc_ = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
};

TEST_F(PxfDecode, ScalarFields) {
  std::string src = R"(
string_field = "hello"
int32_field = -42
uint32_field = 7
int64_field = 1234567890
uint64_field = 18000000000
float_field = 1.5
double_field = 2.71828
bool_field = true
enum_field = STATUS_ACTIVE
)";
  auto msg = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(src, msg.get()).ok());

  const pb::Reflection* r = msg->GetReflection();
  std::string scratch;
  EXPECT_EQ(r->GetStringReference(*msg, desc_->FindFieldByName("string_field"), &scratch), "hello");
  EXPECT_EQ(r->GetInt32(*msg, desc_->FindFieldByName("int32_field")), -42);
  EXPECT_EQ(r->GetUInt32(*msg, desc_->FindFieldByName("uint32_field")), 7u);
  EXPECT_EQ(r->GetBool(*msg, desc_->FindFieldByName("bool_field")), true);
}

TEST_F(PxfDecode, NestedMessageBlock) {
  std::string src = R"(
nested_field {
  name = "inner"
  value = 99
}
)";
  auto msg = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(src, msg.get()).ok());

  const pb::Reflection* r = msg->GetReflection();
  const pb::Message& sub = r->GetMessage(*msg, desc_->FindFieldByName("nested_field"));
  std::string scratch;
  EXPECT_EQ(sub.GetReflection()->GetStringReference(
                sub, sub.GetDescriptor()->FindFieldByName("name"), &scratch),
            "inner");
  EXPECT_EQ(sub.GetReflection()->GetInt32(sub, sub.GetDescriptor()->FindFieldByName("value")), 99);
}

TEST_F(PxfDecode, RepeatedScalar) {
  std::string src = R"(
repeated_string = ["a", "b", "c"]
)";
  auto msg = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(src, msg.get()).ok());
  const pb::Reflection* r = msg->GetReflection();
  EXPECT_EQ(r->FieldSize(*msg, desc_->FindFieldByName("repeated_string")), 3);
}

TEST_F(PxfDecode, RepeatedMessage) {
  std::string src = R"(
repeated_nested = [
  { name = "x" value = 1 }
  { name = "y" value = 2 }
]
)";
  auto msg = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(src, msg.get()).ok());
  const pb::Reflection* r = msg->GetReflection();
  EXPECT_EQ(r->FieldSize(*msg, desc_->FindFieldByName("repeated_nested")), 2);
}

TEST_F(PxfDecode, StringMap) {
  std::string src = R"(
string_map = {
  env: "prod"
  team: "platform"
}
)";
  auto msg = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(src, msg.get()).ok());
  const pb::Reflection* r = msg->GetReflection();
  EXPECT_EQ(r->FieldSize(*msg, desc_->FindFieldByName("string_map")), 2);
}

TEST_F(PxfDecode, TimestampAndDuration) {
  std::string src = R"(
ts_field = 2024-01-15T10:30:00Z
dur_field = 1h30m
)";
  auto msg = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(src, msg.get()).ok());
}

TEST_F(PxfDecode, WrapperSugar) {
  std::string src = R"(
nullable_string = "present"
nullable_int = 42
nullable_bool = true
)";
  auto msg = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(src, msg.get()).ok());

  const pb::Reflection* r = msg->GetReflection();
  const pb::Message& ws = r->GetMessage(*msg, desc_->FindFieldByName("nullable_string"));
  std::string scratch;
  EXPECT_EQ(ws.GetReflection()->GetStringReference(
                ws, ws.GetDescriptor()->FindFieldByName("value"), &scratch),
            "present");
}

TEST_F(PxfDecode, RoundTrip) {
  std::string src =
      "string_field = \"hello\"\n"
      "int32_field = -42\n"
      "bool_field = true\n"
      "nested_field {\n"
      "  name = \"inner\"\n"
      "  value = 99\n"
      "}\n";
  auto msg = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(src, msg.get()).ok());
  auto encoded = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(encoded.ok()) << encoded.status().ToString();

  // Re-parse and verify equality of representative fields.
  auto msg2 = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(*encoded, msg2.get()).ok());
  std::string s1, s2;
  msg->SerializeToString(&s1);
  msg2->SerializeToString(&s2);
  EXPECT_EQ(s1, s2);
}

TEST_F(PxfDecode, UnmarshalFullTracksNullAndAbsent) {
  std::string src = R"(
string_field = "alice"
int32_field = null
)";
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(src, msg.get());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->IsSet("string_field"));
  EXPECT_TRUE(r->IsNull("int32_field"));
  EXPECT_TRUE(r->IsAbsent("bool_field"));
}

}  // namespace

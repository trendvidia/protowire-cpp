// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Tests aimed at the fused fast decoder's distinctive behaviors:
// discard_unknown, oneof conflict detection, and a quick round-trip
// benchmark-style test using the testdata schema.

#include "protowire/pxf.h"

#include <gtest/gtest.h>

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
  void RecordError(absl::string_view filename,
                   int line,
                   int column,
                   absl::string_view msg) override {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

class PxfFast : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", TESTDATA_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pbuf::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("test.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("test.v1.AllTypes");
    factory_ = std::make_unique<pbuf::DynamicMessageFactory>(importer_->pool());
  }
  std::unique_ptr<pbuf::Message> NewAllTypes() {
    return std::unique_ptr<pbuf::Message>(factory_->GetPrototype(desc_)->New());
  }
  pbuf::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pbuf::compiler::Importer> importer_;
  const pbuf::FileDescriptor* file_ = nullptr;
  const pbuf::Descriptor* desc_ = nullptr;
  std::unique_ptr<pbuf::DynamicMessageFactory> factory_;
};

TEST_F(PxfFast, UnknownFieldRejectsByDefault) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(
string_field = "x"
not_a_real_field = 42
)",
                                      msg.get());
  EXPECT_FALSE(st.ok());
}

TEST_F(PxfFast, UnknownFieldDiscarded) {
  auto msg = NewAllTypes();
  protowire::pxf::UnmarshalOptions opts;
  opts.discard_unknown = true;
  auto st = protowire::pxf::Unmarshal(R"(
string_field = "x"
not_a_real_field = 42
nested_field {
  name = "ok"
  value = 1
}
also_unknown { foo = "bar" }
also_list = [1, 2, 3]
)",
                                      msg.get(),
                                      opts);
  ASSERT_TRUE(st.ok()) << st.ToString();
  std::string scratch;
  EXPECT_EQ(msg->GetReflection()->GetStringReference(
                *msg, desc_->FindFieldByName("string_field"), &scratch),
            "x");
}

TEST_F(PxfFast, OneofConflictDetected) {
  // The AllTypes schema has `oneof choice { string text_choice; int32 number_choice; }`
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(
text_choice = "hi"
number_choice = 5
)",
                                      msg.get());
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("oneof"), std::string::npos);
}

TEST_F(PxfFast, OneofSingleFieldOK) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(text_choice = "hi")", msg.get());
  ASSERT_TRUE(st.ok());
}

TEST_F(PxfFast, FastPathRoundTripHeavy) {
  // Reasonably broad input — make sure the fast path covers all the
  // primitive types and structural cases at once.
  std::string src = R"(
string_field = "hello"
int32_field = -42
int64_field = 1234567890
uint32_field = 7
uint64_field = 18000000000
float_field = 1.5
double_field = 2.71828
bool_field = true
enum_field = STATUS_ACTIVE
nested_field {
  name = "n"
  value = 99
}
repeated_string = ["a", "b"]
repeated_nested = [
  { name = "x" value = 1 }
  { name = "y" value = 2 }
]
string_map = { env: "prod" team: "platform" }
ts_field = 2024-01-15T10:30:00Z
dur_field = 1h30m
nullable_string = "present"
nullable_int = 42
nullable_bool = true
)";
  auto msg = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(src, msg.get()).ok());
  // Re-encode and re-decode — proto-level equality is the parity check.
  auto encoded = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(encoded.ok());
  auto msg2 = NewAllTypes();
  ASSERT_TRUE(protowire::pxf::Unmarshal(*encoded, msg2.get()).ok());
  std::string a, b;
  ASSERT_TRUE(msg->SerializeToString(&a));
  ASSERT_TRUE(msg2->SerializeToString(&b));
  EXPECT_EQ(a, b);
}

}  // namespace

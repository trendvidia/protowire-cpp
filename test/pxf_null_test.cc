// Tests for _null FieldMask round-trip support.
//
// Schema declares a `_null` google.protobuf.FieldMask field. Decoded null
// fields should land in `_null.paths`; encoder reads that mask and emits
// `field = null` for each path.

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
  void RecordError(absl::string_view filename, int line, int column,
                   absl::string_view msg) override {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" +
            std::to_string(column) + ": " + std::string(msg);
  }
  std::string last_;
};

// Schema with a _null FieldMask field at field number 15.
constexpr const char* kProto = R"(
syntax = "proto3";
package nulltest.v1;
import "google/protobuf/field_mask.proto";

message Cfg {
  string name  = 1;
  string email = 2;
  string role  = 3;
  google.protobuf.FieldMask _null = 15;
}
)";

class PxfNull : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", WKT_PROTO_DIR);
    source_tree_.MapPath("", testing::TempDir());
    std::string proto_path = std::string(testing::TempDir()) + "/cfg.proto";
    FILE* f = std::fopen(proto_path.c_str(), "w");
    std::fwrite(kProto, 1, std::strlen(kProto), f);
    std::fclose(f);

    importer_ = std::make_unique<pbuf::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("cfg.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("nulltest.v1.Cfg");
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

TEST_F(PxfNull, UnmarshalFullPopulatesNullMask) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(
name = "Alice"
email = null
role = null
)",
                                          msg.get());
  ASSERT_TRUE(r.ok());

  // Verify that the _null FieldMask got populated.
  const pbuf::FieldDescriptor* null_fd = desc_->FindFieldByName("_null");
  ASSERT_NE(null_fd, nullptr);
  const pbuf::Reflection* refl = msg->GetReflection();
  ASSERT_TRUE(refl->HasField(*msg, null_fd));
  const pbuf::Message& fm = refl->GetMessage(*msg, null_fd);
  const pbuf::FieldDescriptor* paths = fm.GetDescriptor()->FindFieldByName("paths");
  EXPECT_EQ(fm.GetReflection()->FieldSize(fm, paths), 2);
}

TEST_F(PxfNull, MarshalEmitsNullsFromMask) {
  // Build a message with the FieldMask pre-populated.
  auto msg = NewCfg();
  const pbuf::Reflection* r = msg->GetReflection();
  r->SetString(msg.get(), desc_->FindFieldByName("name"), "Alice");
  pbuf::Message* fm = r->MutableMessage(msg.get(), desc_->FindFieldByName("_null"));
  const pbuf::FieldDescriptor* paths = fm->GetDescriptor()->FindFieldByName("paths");
  fm->GetReflection()->AddString(fm, paths, "email");

  auto out = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  // The _null field itself must NOT appear in the output.
  EXPECT_EQ(out->find("_null"), std::string::npos);
  // The email field should be emitted as null.
  EXPECT_NE(out->find("email = null"), std::string::npos);
  EXPECT_NE(out->find("name = \"Alice\""), std::string::npos);
}

TEST_F(PxfNull, RoundTripPreservesNulls) {
  auto msg = NewCfg();
  auto r = protowire::pxf::UnmarshalFull(R"(
name = "Bob"
email = null
)",
                                          msg.get());
  ASSERT_TRUE(r.ok());

  // Re-encode → re-decode → re-encode and confirm null survives.
  auto pxf1 = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(pxf1.ok());
  EXPECT_NE(pxf1->find("email = null"), std::string::npos);

  auto msg2 = NewCfg();
  ASSERT_TRUE(protowire::pxf::Unmarshal(*pxf1, msg2.get()).ok());
}

}  // namespace

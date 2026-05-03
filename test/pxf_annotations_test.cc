// Tests for (pxf.required) and (pxf.default) annotation enforcement in
// UnmarshalFull. Mirrors protowire-go's null/required/default behavior.

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

constexpr const char* kProto = R"(
syntax = "proto3";
package anntest.v1;
import "pxf/annotations.proto";

message Cfg {
  string name     = 1 [(pxf.required) = true];
  string role     = 2 [(pxf.default) = "viewer"];
  int32  priority = 3 [(pxf.default) = "5"];
  bool   enabled  = 4 [(pxf.default) = "true"];
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

    importer_ =
        std::make_unique<pbuf::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("cfg.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("anntest.v1.Cfg");
    ASSERT_NE(desc_, nullptr);
    factory_ =
        std::make_unique<pbuf::DynamicMessageFactory>(importer_->pool());
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

}  // namespace

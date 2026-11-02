// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// pxf.Secret sugar: a scalar binds to `value`, list elements and map values
// take either form, and Marshal writes the scalar form when `hint` and
// `fingerprint` are empty. Mirrors secret_test.go in protowire-go. Map values
// take every other literal too, which is pinned here because they share the
// code path.

#include "protowire/pxf.h"
#include "protowire/pxf/dataset_reader.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/text_format.h>

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;

constexpr const char* kSchema = R"src(
syntax = "proto3";
package secret_test.v1;

import "google/protobuf/timestamp.proto";
import "google/protobuf/wrappers.proto";
import "pxf/bignum.proto";
import "pxf/secret.proto";

message SecretDemo {
  string base_url = 1;
  pxf.Secret db_password = 2;
  pxf.Secret api_token = 3;
  repeated pxf.Secret backup_keys = 4;
  map<string, pxf.Secret> tenant_keys = 5;
  map<string, google.protobuf.Timestamp> seen = 6;
  map<string, google.protobuf.Int32Value> counts = 7;
  map<string, pxf.BigInt> weights = 8;
  map<string, pxf.BigFloat> ratios = 9;
  map<string, Plain> plain = 10;
  Plain inner = 11;
}

message Plain { string s = 1; }
)src";

class Errors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
           ": " + std::string(msg);
  }
  std::string last;
};

class PxfSecret : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string path = protowire_test::FixtureDir() + "/secret_demo.proto";
    std::ofstream(path, std::ios::binary) << kSchema;
    tree_.MapPath("", protowire_test::FixtureDir());
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    const pb::FileDescriptor* fd = importer_->Import("secret_demo.proto");
    ASSERT_NE(fd, nullptr) << errors_.last;
    desc_ = fd->FindMessageTypeByName("SecretDemo");
    msg_.reset(factory_.GetPrototype(desc_)->New());
  }

  std::string Field(const pb::Message& m, const char* name) const {
    return m.GetReflection()->GetString(m, m.GetDescriptor()->FindFieldByName(name));
  }
  const pb::Message& Sub(const char* name) const {
    return msg_->GetReflection()->GetMessage(*msg_, desc_->FindFieldByName(name));
  }
  const pb::Message& Elem(const char* name, int i) const {
    return msg_->GetReflection()->GetRepeatedMessage(*msg_, desc_->FindFieldByName(name), i);
  }
  // MapValue returns the value message of the map entry with this key.
  const pb::Message& MapValue(const char* name, const std::string& key) const {
    const pb::FieldDescriptor* fd = desc_->FindFieldByName(name);
    const pb::Reflection* r = msg_->GetReflection();
    for (int i = 0; i < r->FieldSize(*msg_, fd); ++i) {
      const pb::Message& e = r->GetRepeatedMessage(*msg_, fd, i);
      if (Field(e, "key") == key) {
        return e.GetReflection()->GetMessage(e, e.GetDescriptor()->FindFieldByName("value"));
      }
    }
    ADD_FAILURE() << "no key " << key;
    return *msg_;
  }
  std::string Encoded() const {
    auto out = pxf::Marshal(*msg_);
    EXPECT_TRUE(out.ok()) << out.status().ToString();
    return out.ok() ? *out : "";
  }

  pb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  pb::DynamicMessageFactory factory_;
  const pb::Descriptor* desc_ = nullptr;
  std::unique_ptr<pb::Message> msg_;
};

bool Has(const std::string& s, const std::string& sub) {
  return s.find(sub) != std::string::npos;
}

TEST_F(PxfSecret, ScalarBindsToValue) {
  ASSERT_TRUE(pxf::Unmarshal("db_password = \"supersecret\"\n", msg_.get()).ok());
  EXPECT_EQ(Field(Sub("db_password"), "value"), "supersecret");
  EXPECT_EQ(Field(Sub("db_password"), "hint"), "");
}

TEST_F(PxfSecret, BlockFormStillWorks) {
  ASSERT_TRUE(pxf::Unmarshal("db_password {\n value = \"s\"\n hint = \"h\"\n"
                             " fingerprint = \"fp\"\n}\napi_token = \"t\"\n",
                             msg_.get())
                  .ok());
  EXPECT_EQ(Field(Sub("db_password"), "value"), "s");
  EXPECT_EQ(Field(Sub("db_password"), "hint"), "h");
  EXPECT_EQ(Field(Sub("db_password"), "fingerprint"), "fp");
  EXPECT_EQ(Field(Sub("api_token"), "value"), "t");
}

TEST_F(PxfSecret, ListElementsTakeEitherForm) {
  auto st = pxf::Unmarshal(
      "backup_keys = [\n \"one\",\n {\n value = \"two\"\n hint = \"h\"\n }\n \"three\"\n]\n",
      msg_.get());
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(Field(Elem("backup_keys", 0), "value"), "one");
  EXPECT_EQ(Field(Elem("backup_keys", 1), "value"), "two");
  EXPECT_EQ(Field(Elem("backup_keys", 1), "hint"), "h");
  EXPECT_EQ(Field(Elem("backup_keys", 2), "value"), "three");
}

TEST_F(PxfSecret, MapValuesTakeEitherForm) {
  auto st = pxf::Unmarshal(
      "tenant_keys = {\n \"acme\": \"key-a\"\n \"globex\": {\n value = \"key-g\"\n hint = \"h\"\n "
      "}\n}\n",
      msg_.get());
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(Field(MapValue("tenant_keys", "acme"), "value"), "key-a");
  EXPECT_EQ(Field(MapValue("tenant_keys", "globex"), "value"), "key-g");
  EXPECT_EQ(Field(MapValue("tenant_keys", "globex"), "hint"), "h");
}

TEST_F(PxfSecret, EncodesScalarWithoutMetadata) {
  ASSERT_TRUE(pxf::Unmarshal("db_password = \"x\"\n", msg_.get()).ok());
  const std::string out = Encoded();
  EXPECT_TRUE(Has(out, "db_password = \"x\"")) << out;
  EXPECT_FALSE(Has(out, "{")) << out;
}

TEST_F(PxfSecret, EncodesBlockWithEitherMetadataField) {
  ASSERT_TRUE(pxf::Unmarshal("db_password {\n value = \"x\"\n hint = \"h\"\n}\n"
                             "api_token {\n value = \"y\"\n fingerprint = \"fp\"\n}\n",
                             msg_.get())
                  .ok());
  const std::string out = Encoded();
  EXPECT_TRUE(Has(out, "db_password {")) << out;
  EXPECT_TRUE(Has(out, "hint = \"h\"")) << out;
  EXPECT_TRUE(Has(out, "api_token {")) << out;
  EXPECT_TRUE(Has(out, "fingerprint = \"fp\"")) << out;
}

TEST_F(PxfSecret, RepeatedAndMapRoundTrip) {
  const std::string doc =
      "backup_keys = [\n \"a\"\n {\n value = \"b\"\n hint = \"h\"\n }\n]\n"
      "tenant_keys = {\n \"acme\": \"x\"\n \"globex\": {\n value = \"y\"\n hint = \"h\"\n }\n}\n";
  ASSERT_TRUE(pxf::Unmarshal(doc, msg_.get()).ok());
  const std::string out = Encoded();
  EXPECT_TRUE(Has(out, "\"a\"")) << out;
  EXPECT_TRUE(Has(out, "acme: \"x\"")) << out;
  EXPECT_TRUE(Has(out, "globex: {")) << out;

  std::unique_ptr<pb::Message> back(factory_.GetPrototype(desc_)->New());
  auto st = pxf::Unmarshal(out, back.get());
  ASSERT_TRUE(st.ok()) << st.ToString() << "\n" << out;
  std::string a, b;
  ASSERT_TRUE(pb::TextFormat::PrintToString(*msg_, &a));
  ASSERT_TRUE(pb::TextFormat::PrintToString(*back, &b));
  EXPECT_EQ(a, b);
}

TEST_F(PxfSecret, ScalarMarksValuePresent) {
  auto r = pxf::UnmarshalFull("db_password = \"x\"\n", msg_.get());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->IsSet("db_password"));
  EXPECT_TRUE(r->IsSet("db_password.value"));
}

TEST_F(PxfSecret, ScalarIsCheckedAsAStringField) {
  auto st = pxf::Unmarshal("db_password = \"\\xff\"\n", msg_.get());
  ASSERT_FALSE(st.ok());
  EXPECT_TRUE(Has(st.message(), "invalid UTF-8")) << st.ToString();
}

TEST_F(PxfSecret, OtherMessagesStillRefuseAScalar) {
  auto st = pxf::Unmarshal("inner = \"x\"\n", msg_.get());
  ASSERT_FALSE(st.ok());
  EXPECT_TRUE(Has(st.message(), "expected '{'")) << st.ToString();
  auto mapped = pxf::Unmarshal("plain = { \"k\": \"x\" }\n", msg_.get());
  ASSERT_FALSE(mapped.ok());
  EXPECT_TRUE(Has(mapped.message(), "expected '{'")) << mapped.ToString();
}

TEST_F(PxfSecret, DatasetCellTakesTheScalar) {
  std::istringstream in(
      "@dataset secret_test.v1.SecretDemo (base_url, db_password)\n"
      "(\"u\", \"pw\")\n");
  auto reader = pxf::DatasetReader::Create(&in);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  auto st = (*reader)->Scan(msg_.get());
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(Field(Sub("db_password"), "value"), "pw");
}

// Marshal writes these as literals, so a map of them has to read them back.
TEST_F(PxfSecret, MapValuesTakeEveryLiteral) {
  auto st = pxf::Unmarshal(
      "seen = { a: 2026-01-02T03:04:05Z }\n"
      "counts = { a: 7 }\n"
      "weights = { a: 42 }\n"
      "ratios = { a: 1.5p53 }\n",
      msg_.get());
  ASSERT_TRUE(st.ok()) << st.ToString();
  const std::string out = Encoded();
  EXPECT_TRUE(Has(out, "a: 2026-01-02T03:04:05Z")) << out;
  EXPECT_TRUE(Has(out, "a: 7")) << out;
  EXPECT_TRUE(Has(out, "a: 42")) << out;
  EXPECT_TRUE(Has(out, "a: 1.5p53")) << out;

  std::unique_ptr<pb::Message> back(factory_.GetPrototype(desc_)->New());
  auto again = pxf::Unmarshal(out, back.get());
  EXPECT_TRUE(again.ok()) << again.ToString() << "\n" << out;
}

TEST_F(PxfSecret, MapValueLiteralsKeepTheirChecks) {
  auto prec = pxf::Unmarshal("weights = { a: 42p53 }\n", msg_.get());
  ASSERT_FALSE(prec.ok());
  EXPECT_TRUE(Has(prec.message(), "precision suffix")) << prec.ToString();

  pxf::UnmarshalOptions opts;
  opts.max_numeric_literal_digits = 3;
  std::unique_ptr<pb::Message> m(factory_.GetPrototype(desc_)->New());
  auto digits = pxf::Unmarshal("weights = { a: 12345 }\n", m.get(), opts);
  ASSERT_FALSE(digits.ok());
  EXPECT_TRUE(Has(digits.message(), "MaxNumericLiteralDigits")) << digits.ToString();
}

}  // namespace

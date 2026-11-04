// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Marshal and an unset annotated field (#129, protowire-go#259). A reader that
// applies (pxf.default) to an absent field and checks (pxf.required) would not
// read a left-out annotated field back as unset, so Marshal writes such a field
// whenever text can say what it holds. The draft requires
// decode_PXF(encode_PXF(v)) == v (Common Schema Layer). Mirrors
// marshal_unset_annotated_test.go in protowire-go, with the same expected text.

#include "protowire/pxf.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/message_differencer.h>

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;
using pb::util::MessageDifferencer;

constexpr const char* kSchema = R"src(
syntax = "proto3";
package unsetann.v1;

import "google/protobuf/field_mask.proto";
import "google/protobuf/wrappers.proto";
import "google/protobuf/timestamp.proto";
import "pxf/annotations.proto";

message M {
  int32 n = 1 [(pxf.default) = "5"];
  string s = 2 [(pxf.default) = "\"dflt\""];
  bool b = 3 [(pxf.default) = "true"];
  int32 req = 4 [(pxf.required) = true];
  repeated int32 rl = 5 [(pxf.required) = true];
  map<string, int32> rm = 6 [(pxf.required) = true];
  google.protobuf.Int32Value w = 7 [(pxf.default) = "7"];
  google.protobuf.Timestamp ts = 8 [(pxf.default) = "2024-01-01T00:00:00Z"];
  oneof o {
    int32 a = 9 [(pxf.default) = "8"];
    string other = 10;
  }
  int32 plain = 11;
  Sub sub = 12;
}

message Sub {
  int32 k = 1 [(pxf.default) = "3"];
  int32 plain = 2;
}

message Opt {
  optional int32 o = 1 [(pxf.default) = "6"];
}

message ReqOpt {
  optional int32 o = 1 [(pxf.required) = true];
  int32 plain = 2;
}

message N {
  google.protobuf.Int32Value w = 1 [(pxf.default) = "7"];
  int32 plain = 2;
  google.protobuf.FieldMask _null = 3;
}
)src";

class Errors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
           ": " + std::string(msg);
  }
  std::string last;
};

class MarshalUnsetAnnotated : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream(protowire_test::FixtureDir() + "/unset_annotated.proto", std::ios::binary)
        << kSchema;
    tree_.MapPath("", protowire_test::FixtureDir());
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    file_ = importer_->Import("unset_annotated.proto");
    ASSERT_NE(file_, nullptr) << errors_.last;
  }

  const pb::Descriptor* Desc(const char* name) { return file_->FindMessageTypeByName(name); }

  std::unique_ptr<pb::Message> New(const char* name) {
    return std::unique_ptr<pb::Message>(factory_.GetPrototype(Desc(name))->New());
  }

  // FullReadBack marshals m and reads the text back with defaults applied and
  // required fields checked, returning the text and what it read.
  std::unique_ptr<pb::Message> FullReadBack(const pb::Message& m, std::string* text) {
    auto out = pxf::Marshal(m);
    EXPECT_TRUE(out.ok()) << out.status().ToString();
    if (!out.ok()) return nullptr;
    *text = *out;
    std::unique_ptr<pb::Message> back(m.New());
    auto r = pxf::UnmarshalFull(*out, back.get());
    EXPECT_TRUE(r.ok()) << r.status().ToString() << "\n" << *out;
    return back;
  }

  pb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  pb::DynamicMessageFactory factory_;
};

const pb::FieldDescriptor* Field(const pb::Message& m, const char* name) {
  return m.GetDescriptor()->FindFieldByName(name);
}

void SetInt32(pb::Message* m, const char* name, int32_t v) {
  m->GetReflection()->SetInt32(m, Field(*m, name), v);
}

int32_t GetInt32(const pb::Message& m, const char* name) {
  return m.GetReflection()->GetInt32(m, Field(m, name));
}

// The issue's case: a zero proto3 scalar with a default reads back zero.
TEST_F(MarshalUnsetAnnotated, ZeroDefaultedScalarReadsBack) {
  auto m = New("Sub");
  SetInt32(m.get(), "plain", 1);
  std::string out;
  auto back = FullReadBack(*m, &out);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(out, "k = 0\nplain = 1\n");
  EXPECT_TRUE(MessageDifferencer::Equals(*m, *back)) << out;
}

// Every unset annotated shape at once, top level and one level down, with
// nothing but plain fields set.
TEST_F(MarshalUnsetAnnotated, UnsetAnnotatedFieldsReadBack) {
  auto m = New("M");
  SetInt32(m.get(), "plain", 1);
  pb::Message* sub = m->GetReflection()->MutableMessage(m.get(), Field(*m, "sub"));
  SetInt32(sub, "plain", 2);

  std::string out;
  auto back = FullReadBack(*m, &out);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(out,
            "n = 0\n"
            "s = \"\"\n"
            "b = false\n"
            "req = 0\n"
            "rl = []\n"
            "rm = {}\n"
            "w = null\n"
            "ts = null\n"
            "plain = 1\n"
            "sub {\n"
            "  k = 0\n"
            "  plain = 2\n"
            "}\n");

  // The oneof member with a default is the one field that reads back as its
  // default: Marshal leaves it out rather than write null (protowire-go#267,
  // the owner's choice).
  EXPECT_EQ(GetInt32(*back, "a"), 8);
  back->GetReflection()->ClearField(back.get(), Field(*back, "a"));
  EXPECT_TRUE(MessageDifferencer::Equals(*m, *back)) << out;
}

// A set oneof member keeps its sibling's default off, so the sibling is not
// written.
TEST_F(MarshalUnsetAnnotated, OneofSiblingKeepsDefaultOff) {
  auto m = New("M");
  m->GetReflection()->SetString(m.get(), Field(*m, "other"), "x");
  std::string out;
  auto back = FullReadBack(*m, &out);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(out.find("a = "), std::string::npos) << out;
  EXPECT_TRUE(MessageDifferencer::Equals(*m, *back)) << out;
}

// An unset `optional` scalar with a default is left out and reads back as its
// default: the owner chose this in protowire-go#267, and draft -01 § Common
// Schema Layer names it as an exception to the round trip. `o = null` would
// read back unset (protowire#339 option 3).
TEST_F(MarshalUnsetAnnotated, UnsetOptionalDefaultedReadsBackDefault) {
  auto m = New("Opt");
  std::string out;
  auto back = FullReadBack(*m, &out);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(out, "");
  EXPECT_EQ(GetInt32(*back, "o"), 6);

  auto nulled = New("Opt");
  auto r = pxf::UnmarshalFull("o = null\n", nulled.get());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_FALSE(nulled->GetReflection()->HasField(*nulled, Field(*nulled, "o")))
      << "null is the text that keeps it unset";
}

// Pinned limit (protowire-go#267): in a message with a _null mask, every null
// read is recorded in the mask, so an unset defaulted field has no text that
// reads back as "unset and not null". Written `null`, as Marshal writes a
// message-typed one, it reads back with the mask naming it; left out, it reads
// back as its default. Neither equals the message. No writer can avoid this.
TEST_F(MarshalUnsetAnnotated, UnsetDefaultedInNullMaskMessageHasNoText) {
  auto m = New("N");
  SetInt32(m.get(), "plain", 1);
  std::string out;
  auto back = FullReadBack(*m, &out);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(out, "w = null\nplain = 1\n");
  const pb::Reflection* r = back->GetReflection();
  EXPECT_FALSE(r->HasField(*back, Field(*back, "w")));
  const pb::Message& mask = r->GetMessage(*back, Field(*back, "_null"));
  const auto* paths = mask.GetDescriptor()->FindFieldByName("paths");
  ASSERT_EQ(mask.GetReflection()->FieldSize(mask, paths), 1);
  EXPECT_EQ(mask.GetReflection()->GetRepeatedString(mask, paths, 0), "w");
  EXPECT_FALSE(MessageDifferencer::Equals(*m, *back)) << "the mask names w";

  auto left_out = New("N");
  auto res = pxf::UnmarshalFull("plain = 1\n", left_out.get());
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  const pb::Message& w = left_out->GetReflection()->GetMessage(*left_out, Field(*left_out, "w"));
  EXPECT_EQ(GetInt32(w, "value"), 7);
  EXPECT_FALSE(left_out->GetReflection()->HasField(*left_out, Field(*left_out, "_null")));
}

// An unset required field with presence has no text a reader accepts: absent
// is rejected, and so is null (protowire-go#268). So Marshal refuses it. With
// emit_defaults it is written as zero, which a reader accepts, as before.
TEST_F(MarshalUnsetAnnotated, UnsetRequiredOptionalRefused) {
  auto m = New("ReqOpt");
  SetInt32(m.get(), "plain", 1);

  auto refused = pxf::Marshal(*m);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.status().message(),
            "pxf: unsetann.v1.ReqOpt.o is unset, and the field is (pxf.required)");

  pxf::MarshalOptions opts;
  opts.emit_defaults = true;
  auto out = pxf::Marshal(*m, opts);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_EQ(*out, "o = 0\nplain = 1\n");

  SetInt32(m.get(), "o", 0);
  std::string text;
  auto back = FullReadBack(*m, &text);
  ASSERT_NE(back, nullptr);
  EXPECT_TRUE(MessageDifferencer::Equals(*m, *back)) << text;
}

}  // namespace

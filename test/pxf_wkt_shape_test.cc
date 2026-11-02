// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// A schema may declare a well-known type itself, and draft -01 §wkt-shapes
// says what such a declaration must have. Mirrors wkt_shape_test.go in
// protowire-go. scripts/go_differential.sh compares the text with Go's over
// testdata/differential/wkt-shape.

#include "protowire/pxf.h"
#include "protowire/pxf/dataset_writer.h"
#include "protowire/pxf/schema.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;
using protowire::Status;

struct Redefinition {
  const char* pkg;
  const char* name;
  const char* body;
  const char* value;  // one value, as written after `pw = `
  const char* detail;
};

// A wrong shape for every type matched by name.
const Redefinition kRedefinitions[] = {
    {"pxf", "Secret", "string other = 1;", "\"x\"", "no field \"value\""},
    {"pxf", "Secret", "int32 value = 1;", "\"x\"", "field \"value\" is int32"},
    {"pxf", "Secret", "repeated string value = 1;", "\"x\"", "\"value\" has the wrong cardinality"},
    {"pxf", "Secret", "string value = 1; int32 hint = 2;", "\"x\"", "field \"hint\" is int32"},
    {"pxf", "BigInt", "string other = 1;", "5", "no field \"abs\""},
    {"pxf", "BigInt", "int32 abs = 1; bool negative = 2;", "5", "field \"abs\" is int32"},
    {"pxf", "Decimal", "string other = 1;", "1.5", "no field \"unscaled\""},
    {"pxf", "BigFloat", "string other = 1;", "1.5", "no field \"mantissa\""},
    {"google.protobuf",
     "Timestamp",
     "string other = 1;",
     "2026-01-01T00:00:00Z",
     "no field \"seconds\""},
    {"google.protobuf", "Duration", "string other = 1;", "5s", "no field \"seconds\""},
    {"google.protobuf", "StringValue", "string other = 1;", "\"x\"", "no field \"value\""},
    {"google.protobuf", "StringValue", "int32 value = 1;", "\"x\"", "field \"value\" is int32"},
    {"google.protobuf", "Int32Value", "string value = 1;", "5", "field \"value\" is string"},
    {"google.protobuf",
     "BytesValue",
     "string value = 1;",
     "b\"aGk=\"",
     "field \"value\" is string"},
    {"google.protobuf", "Any", "string other = 1;", "{ }", "no field \"type_url\""},
};

class Errors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
           ": " + std::string(msg);
  }
  std::string last;
};

// Compiled holds a schema with a message `name` and a holder H that uses it
// as a singular field, a list and a map value.
class Compiled {
 public:
  Compiled(const std::string& pkg, const std::string& name, const std::string& body) {
    Load("syntax = \"proto3\";\npackage " + pkg + ";\nmessage " + name + " { " + body +
             " }\nmessage H { " + name + " pw = 1; repeated " + name + " l = 2; map<string, " +
             name + "> m = 3; }\n",
         pkg + ".H");
  }
  explicit Compiled(const std::string& source, const std::string& holder = "google.protobuf.H") {
    Load(source, holder);
  }

  const pb::Descriptor* desc() const { return desc_; }
  std::unique_ptr<pb::Message> New() {
    return std::unique_ptr<pb::Message>(factory_->GetPrototype(desc_)->New());
  }

 private:
  void Load(const std::string& source, const std::string& holder) {
    std::ofstream(protowire_test::FixtureDir() + "/wkt_shape.proto", std::ios::binary) << source;
    tree_.MapPath("", protowire_test::FixtureDir());
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    const pb::FileDescriptor* fd = importer_->Import("wkt_shape.proto");
    ASSERT_NE(fd, nullptr) << errors_.last;
    desc_ = importer_->pool()->FindMessageTypeByName(holder);
    ASSERT_NE(desc_, nullptr);
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
  }

  pb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
  const pb::Descriptor* desc_ = nullptr;
};

TEST(PxfWktShape, RefusedAtBind) {
  for (const Redefinition& tc : kRedefinitions) {
    SCOPED_TRACE(std::string(tc.name) + "/" + tc.body);
    Compiled s(tc.pkg, tc.name, tc.body);
    const auto vs = pxf::ValidateDescriptor(s.desc());
    ASSERT_EQ(vs.size(), 1u);
    EXPECT_EQ(vs[0].kind, pxf::ViolationKind::kWellKnownShape);
    EXPECT_EQ(vs[0].element, std::string(tc.pkg) + "." + tc.name);
    const std::string text = vs[0].ToString();
    EXPECT_NE(text.find(tc.detail), std::string::npos) << text;
    EXPECT_NE(text.find("§wkt-shapes"), std::string::npos) << text;
    auto m = s.New();
    const Status st = pxf::Unmarshal(std::string("pw = ") + tc.value, m.get());
    EXPECT_NE(st.message().find("redefines a well-known type"), std::string::npos) << st.message();
  }
}

// Where validation does not run the message is ordinary: every form in every
// position is an error or a block, and nothing crashes.
TEST(PxfWktShape, OrdinaryWithoutValidation) {
  int runs = 0;
  for (const Redefinition& tc : kRedefinitions) {
    SCOPED_TRACE(std::string(tc.name) + "/" + tc.body);
    Compiled s(tc.pkg, tc.name, tc.body);
    const bool block = std::string(tc.name) == "Any";
    const std::string value = tc.value;
    pxf::UnmarshalOptions opts;
    opts.skip_validate = true;
    pxf::UnmarshalOptions hooked = opts;
    hooked.on_secret_field = [](std::string_view, std::string_view) {
      ADD_FAILURE() << "on_secret_field fired for a message of the wrong shape";
      return Status::OK();
    };
    for (const std::string& doc :
         {"pw = " + value, "l = [ " + value + " ]", "m = { \"k\": " + value + " }"}) {
      ++runs;
      for (const pxf::UnmarshalOptions* o : {&opts, &hooked}) {
        auto m = s.New();
        EXPECT_EQ(pxf::Unmarshal(doc, m.get(), *o).ok(), block) << doc;
        auto f = s.New();
        EXPECT_EQ(pxf::UnmarshalFull(doc, f.get(), *o).ok(), block) << doc;
      }
    }

    auto m = s.New();
    const pb::Reflection* r = m->GetReflection();
    r->MutableMessage(m.get(), s.desc()->FindFieldByName("pw"));
    r->AddMessage(m.get(), s.desc()->FindFieldByName("l"));
    r->AddMessage(m.get(), s.desc()->FindFieldByName("m"));
    auto out = pxf::Marshal(*m);
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_NE(out->find("pw {"), std::string::npos) << *out;

    std::string row;
    const Status st = pxf::AppendDatasetRow(&row, *m, {"pw"});
    EXPECT_NE(st.message().find("redefines a well-known type"), std::string::npos)
        << st.message() << " row " << row;
  }
  EXPECT_EQ(runs, static_cast<int>(std::size(kRedefinitions)) * 3);
}

// Only the shape counts: a copy of a canonical type, a smaller Secret and
// extra fields are all still the well-known type.
TEST(PxfWktShape, Accepted) {
  struct Case {
    const char* pkg;
    const char* name;
    const char* body;
    const char* doc;
  };
  for (const Case& tc : std::vector<Case>{
           {"pxf", "Secret", "string value = 1;", "pw = \"x\""},
           {"pxf",
            "Secret",
            "string value = 1; string hint = 2; string fingerprint = 3;",
            "pw = \"x\""},
           {"pxf", "Secret", "string value = 7; string note = 8;", "pw = \"x\""},
           {"pxf", "Secret", "optional string value = 1;", "pw = \"x\""},
           {"pxf", "BigInt", "bytes abs = 1; bool negative = 2;", "pw = 5"},
           {"pxf",
            "Decimal",
            "bytes unscaled = 1; int32 scale = 2; bool negative = 3;",
            "pw = 1.5"},
           {"pxf",
            "BigFloat",
            "bytes mantissa = 1; int32 exponent = 2; uint32 prec = 3; bool negative = 4;",
            "pw = 1.5"},
           {"google.protobuf",
            "Timestamp",
            "int64 seconds = 1; int32 nanos = 2;",
            "pw = 2026-01-01T00:00:00Z"},
           {"google.protobuf", "Int32Value", "int32 value = 1;", "pw = 5"},
       }) {
    SCOPED_TRACE(std::string(tc.name) + "/" + tc.body);
    Compiled s(tc.pkg, tc.name, tc.body);
    EXPECT_TRUE(pxf::ValidateDescriptor(s.desc()).empty());
    auto m = s.New();
    const Status st = pxf::Unmarshal(tc.doc, m.get());
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(m->GetReflection()->HasField(*m, s.desc()->FindFieldByName("pw")));
  }
}

// A Secret may leave hint out, and a block naming it is then an error rather
// than a null field in the hook's path.
TEST(PxfWktShape, SecretHookWithoutDeclaredHint) {
  Compiled s("pxf", "Secret", "string value = 1;");
  pxf::UnmarshalOptions opts;
  opts.on_secret_field = [](std::string_view, std::string_view) { return Status::OK(); };
  for (const char* doc : {"pw { hint = \"h\" }", "pw { fingerprint = \"f\" }"}) {
    auto m = s.New();
    const Status st = pxf::Unmarshal(doc, m.get(), opts);
    EXPECT_NE(st.message().find("this schema does not declare it"), std::string::npos)
        << st.message();
  }
}

// A null mask needs the real FieldMask: this one has no paths to read.
TEST(PxfWktShape, NullMaskRequiresFieldMaskShape) {
  Compiled s(
      "syntax = \"proto3\";\npackage google.protobuf;\nmessage FieldMask { string other = 1; }\n"
      "message H { FieldMask _null = 1; string s = 2; }\n");
  EXPECT_EQ(pxf::ValidateDescriptor(s.desc()).size(), 1u);
  auto m = s.New();
  m->GetReflection()->MutableMessage(m.get(), s.desc()->FindFieldByName("_null"));
  EXPECT_TRUE(pxf::Marshal(*m).ok());
}

// A redefinition is checked wherever it is declared, nested included, since
// the codec matches on the full name.
TEST(PxfWktShape, NestedMessageWithTheFullName) {
  Compiled s(
      "syntax = \"proto3\";\npackage google;\nmessage protobuf { message Timestamp { "
      "string other = 1; } }\nmessage H { protobuf.Timestamp pw = 1; }\n",
      "google.H");
  const auto vs = pxf::ValidateDescriptor(s.desc());
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].element, "google.protobuf.Timestamp");
}

// The closure walk skips only descriptor.proto: a schema can put its own file
// at a google/protobuf path, the likeliest place for a redefinition.
TEST(PxfWktShape, CheckedAtAGoogleProtobufPath) {
  pb::FileDescriptorProto ts;
  ts.set_name("google/protobuf/timestamp.proto");
  ts.set_package("google.protobuf");
  ts.set_syntax("proto3");
  pb::DescriptorProto* msg = ts.add_message_type();
  msg->set_name("Timestamp");
  pb::FieldDescriptorProto* other = msg->add_field();
  other->set_name("other");
  other->set_number(1);
  other->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
  other->set_type(pb::FieldDescriptorProto::TYPE_STRING);

  pb::FileDescriptorProto user;
  user.set_name("user.proto");
  user.set_package("user.v1");
  user.set_syntax("proto3");
  user.add_dependency(ts.name());
  pb::DescriptorProto* holder = user.add_message_type();
  holder->set_name("H");
  pb::FieldDescriptorProto* pw = holder->add_field();
  pw->set_name("pw");
  pw->set_number(1);
  pw->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
  pw->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
  pw->set_type_name(".google.protobuf.Timestamp");

  pb::DescriptorPool pool;
  ASSERT_NE(pool.BuildFile(ts), nullptr);
  const pb::FileDescriptor* fd = pool.BuildFile(user);
  ASSERT_NE(fd, nullptr);
  const auto vs = pxf::ValidateFile(fd);
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].kind, pxf::ViolationKind::kWellKnownShape);
  EXPECT_EQ(vs[0].file, "google/protobuf/timestamp.proto");
}

}  // namespace

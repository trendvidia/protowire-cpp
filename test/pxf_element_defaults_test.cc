// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// (pxf.default) and (pxf.required) apply in every message body a document
// writes: list elements, map values, keyed entries and Any bodies, as well as
// singular message fields. Mirrors element_defaults_test.go in protowire-go.

#include "protowire/pxf.h"
#include "protowire/pxf/dataset_reader.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;

constexpr const char* kSchema = R"src(
syntax = "proto3";
package elemdefaults.v1;

import "google/protobuf/any.proto";
import "google/protobuf/field_mask.proto";
import "google/protobuf/wrappers.proto";
import "pxf/annotations.proto";

message P {
  repeated Item items = 1;
  map<string, Item> by_key = 2;
  repeated Item keyed = 3 [(pxf.key) = "id"];
  google.protobuf.Any payload = 4;
  Item one = 5;
  map<int32, Item> by_num = 6;
  string label = 7;
  repeated KeyedDefault kd = 8 [(pxf.key) = "name"];
  google.protobuf.FieldMask _null = 15;
}

message Item {
  string id = 1;
  string label = 2 [(pxf.default) = "item-dflt"];
  Deep deep = 3;
  repeated Leaf leaves = 4;
  google.protobuf.Any inner = 5;
  oneof pick {
    string pick_s = 6 [(pxf.default) = "pick-dflt"];
    int32 pick_i = 7;
  }
  google.protobuf.StringValue w = 8 [(pxf.default) = "w-dflt"];
}

message Deep {
  int32 n = 1 [(pxf.required) = true];
  optional int32 r = 2;
}

message Leaf {
  string tag = 1 [(pxf.default) = "leaf-dflt"];
}

message KeyedDefault {
  string name = 1 [(pxf.default) = "must-not-replace-the-entry-name"];
}

// Required at the root, with an element holding a field of the same name.
message Parent {
  string x = 1 [(pxf.required) = true];
  string y = 2 [(pxf.default) = "parent-dflt"];
  repeated Child kids = 3;
  map<string, Child> kid_map = 4;
}

message Child {
  string x = 1;
  string y = 2;
}

// No annotation anywhere below it.
message Bare {
  repeated Child kids = 1;
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

class Resolver : public pxf::TypeResolver {
 public:
  explicit Resolver(const pb::DescriptorPool* pool) : pool_(pool) {}
  const pb::Descriptor* FindMessageByURL(std::string_view url) override {
    return pool_->FindMessageTypeByName(std::string(url));
  }

 private:
  const pb::DescriptorPool* pool_;
};

class ElementDefaults : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream(protowire_test::FixtureDir() + "/element_defaults.proto", std::ios::binary)
        << kSchema;
    tree_.MapPath("", protowire_test::FixtureDir());
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    file_ = importer_->Import("element_defaults.proto");
    ASSERT_NE(file_, nullptr) << errors_.last;
    resolver_ = std::make_unique<Resolver>(importer_->pool());
    opts_.type_resolver = resolver_.get();
  }

  std::unique_ptr<pb::Message> New(const char* type = "P") {
    return std::unique_ptr<pb::Message>(
        factory_.GetPrototype(file_->FindMessageTypeByName(type))->New());
  }

  // Unpack decodes an Any's value as its own type.
  std::unique_ptr<pb::Message> Unpack(const pb::Message& any, const char* type) {
    auto inner = New(type);
    const auto* d = any.GetDescriptor();
    EXPECT_TRUE(
        inner->ParseFromString(any.GetReflection()->GetString(any, d->FindFieldByName("value"))));
    return inner;
  }

  pb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  pb::DynamicMessageFactory factory_;
  std::unique_ptr<Resolver> resolver_;
  pxf::UnmarshalOptions opts_;
};

const pb::Message& Elem(const pb::Message& m, const char* field, int i) {
  const auto* fd = m.GetDescriptor()->FindFieldByName(field);
  return m.GetReflection()->GetRepeatedMessage(m, fd, i);
}
const pb::Message& Sub(const pb::Message& m, const char* field) {
  return m.GetReflection()->GetMessage(m, m.GetDescriptor()->FindFieldByName(field));
}
std::string Str(const pb::Message& m, const char* field) {
  return m.GetReflection()->GetString(m, m.GetDescriptor()->FindFieldByName(field));
}
bool Set(const pb::Message& m, const char* field) {
  return m.GetReflection()->HasField(m, m.GetDescriptor()->FindFieldByName(field));
}
// OnlyMapValue is the value of a map holding one entry.
const pb::Message& OnlyMapValue(const pb::Message& m, const char* field) {
  return Sub(Elem(m, field, 0), "value");
}

TEST_F(ElementDefaults, AppliedInListElements) {
  auto m = New();
  auto r = pxf::UnmarshalFull("items = [{ label = \"x\" }, {}]\n", m.get(), opts_);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(Str(Elem(*m, "items", 0), "label"), "x");
  EXPECT_EQ(Str(Elem(*m, "items", 1), "label"), "item-dflt");
  EXPECT_EQ(Str(Sub(Elem(*m, "items", 1), "w"), "value"), "w-dflt");
}

TEST_F(ElementDefaults, AppliedInMapValuesAndKeyedEntries) {
  auto m = New();
  auto r = pxf::UnmarshalFull("by_key = { \"k\": {} }\nkeyed { a {} }\n", m.get(), opts_);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(Str(OnlyMapValue(*m, "by_key"), "label"), "item-dflt");
  EXPECT_EQ(Str(Elem(*m, "keyed", 0), "label"), "item-dflt");
  EXPECT_EQ(Str(Elem(*m, "keyed", 0), "id"), "a");
}

// The default reaches the serialized message, not a copy of the map entry.
TEST_F(ElementDefaults, MapValueDefaultSurvivesSerialization) {
  auto m = New();
  ASSERT_TRUE(pxf::UnmarshalFull("by_key = { \"k\": {} }\n", m.get(), opts_).ok());
  auto back = New();
  ASSERT_TRUE(back->ParseFromString(m->SerializeAsString()));
  EXPECT_EQ(Str(OnlyMapValue(*back, "by_key"), "label"), "item-dflt");
}

TEST_F(ElementDefaults, AppliedAtEveryDepth) {
  auto m = New();
  auto r = pxf::UnmarshalFull(
      "items = [{ leaves = [{}, { tag = \"t\" }] }]\n"
      "by_key = { \"k\": { leaves = [{}] } }\n"
      "one { leaves = [{}] }\n",
      m.get(),
      opts_);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(Str(Elem(Elem(*m, "items", 0), "leaves", 0), "tag"), "leaf-dflt");
  EXPECT_EQ(Str(Elem(Elem(*m, "items", 0), "leaves", 1), "tag"), "t");
  EXPECT_EQ(Str(Elem(OnlyMapValue(*m, "by_key"), "leaves", 0), "tag"), "leaf-dflt");
  EXPECT_EQ(Str(Elem(Sub(*m, "one"), "leaves", 0), "tag"), "leaf-dflt");
}

// Setting a oneof member clears the rest, so a default on one arm would
// destroy the arm the document chose.
TEST_F(ElementDefaults, ChosenOneofArmKeepsTheDefaultOff) {
  auto m = New();
  auto r = pxf::UnmarshalFull("items = [{ pick_i = 3 }, {}]\none { pick_i = 4 }\n", m.get(), opts_);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_FALSE(Set(Elem(*m, "items", 0), "pick_s"));
  EXPECT_TRUE(Set(Elem(*m, "items", 0), "pick_i"));
  EXPECT_EQ(Str(Elem(*m, "items", 1), "pick_s"), "pick-dflt");
  EXPECT_FALSE(Set(Sub(*m, "one"), "pick_s"));
  EXPECT_TRUE(Set(Sub(*m, "one"), "pick_i"));
}

TEST_F(ElementDefaults, AppliedInAnyBodies) {
  auto m = New();
  auto r = pxf::UnmarshalFull(
      "payload { @type = \"elemdefaults.v1.Item\" leaves = [{}] }\n", m.get(), opts_);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto inner = Unpack(Sub(*m, "payload"), "Item");
  EXPECT_EQ(Str(*inner, "label"), "item-dflt");
  EXPECT_EQ(Str(Elem(*inner, "leaves", 0), "tag"), "leaf-dflt");

  auto in_elem = New();
  r = pxf::UnmarshalFull(
      "items = [{ inner = { @type = \"elemdefaults.v1.Leaf\" } }]\n", in_elem.get(), opts_);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(Str(*Unpack(Sub(Elem(*in_elem, "items", 0), "inner"), "Leaf"), "tag"), "leaf-dflt");
}

TEST_F(ElementDefaults, RequiredCheckedWithTheBodysPath) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"items = [{ deep { n = 1 } }, { deep {} }]\n", "items[1].deep.n"},
      {"by_key = { \"k\": { deep {} } }\n", "by_key[\"k\"].deep.n"},
      {"by_num = { 7: { deep {} } }\n", "by_num[7].deep.n"},
      {"keyed { a { deep {} } }\n", "keyed[0].deep.n"},
      {"payload { @type = \"elemdefaults.v1.Item\" deep {} }\n", "payload.deep.n"},
      {"items = [{ inner = { @type = \"elemdefaults.v1.Deep\" } }]\n", "items[0].inner.n"},
  };
  for (const auto& [doc, path] : cases) {
    SCOPED_TRACE(doc);
    auto m = New();
    auto r = pxf::UnmarshalFull(doc, m.get(), opts_);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("required field \"" + path + "\" is absent"),
              std::string::npos)
        << r.status().ToString();
  }
}

// Map values are checked in key order, whatever the document's order, so the
// first error is the same on every run.
TEST_F(ElementDefaults, MapErrorsAreInKeyOrder) {
  auto m = New();
  auto r = pxf::UnmarshalFull("by_key = { b: { deep {} }\n a: { deep {} } }\n", m.get(), opts_);
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("by_key[\"a\"]"), std::string::npos) << r.status().ToString();
}

TEST_F(ElementDefaults, NullSatisfiesRequiredAndItsBodyIsNotEntered) {
  auto m = New();
  auto r = pxf::UnmarshalFull("items = [{ deep = null }]\n", m.get(), opts_);
  EXPECT_TRUE(r.ok()) << r.status().ToString();
}

// A null in an element is presence in that element. The _null mask names
// singular paths from the root, and an element has none.
TEST_F(ElementDefaults, NullInAnElementStaysOutOfTheMask) {
  auto m = New();
  auto r = pxf::UnmarshalFull("items = [{ w = null }]\none { w = null }\n", m.get(), opts_);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->IsNull("items[0].w"));
  EXPECT_TRUE(r->IsNull("one.w"));
  EXPECT_FALSE(Set(Elem(*m, "items", 0), "w"));  // null keeps the default off
  const auto& mask = Sub(*m, "_null");
  const auto* paths = mask.GetDescriptor()->FindFieldByName("paths");
  ASSERT_EQ(mask.GetReflection()->FieldSize(mask, paths), 1);
  EXPECT_EQ(mask.GetReflection()->GetRepeatedString(mask, paths, 0), "one.w");
}

// The entry name wrote the key field, so its default does not apply.
TEST_F(ElementDefaults, KeyedEntryNameCountsAsTheKeyField) {
  auto m = New();
  auto r = pxf::UnmarshalFull("kd { alpha {} }\n", m.get(), opts_);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(Str(Elem(*m, "kd", 0), "name"), "alpha");
}

TEST_F(ElementDefaults, ElementFieldsDoNotMarkTheParent) {
  auto m = New("Parent");
  auto r = pxf::UnmarshalFull("kids = [{ x = \"1\" y = \"2\" }]\n", m.get());
  ASSERT_FALSE(r.ok()) << "a field in an element satisfied the parent's required field";
  EXPECT_NE(r.status().message().find("required field \"x\" is absent"), std::string::npos)
      << r.status().ToString();

  auto mapped = New("Parent");
  r = pxf::UnmarshalFull("kid_map = { a: { x = \"1\" } }\n", mapped.get());
  ASSERT_FALSE(r.ok()) << "a field in a map value satisfied the parent's required field";

  auto kept = New("Parent");
  auto ok = pxf::UnmarshalFull("x = \"root\"\nkids = [{ y = \"2\" }]\n", kept.get());
  ASSERT_TRUE(ok.ok()) << ok.status().ToString();
  EXPECT_EQ(Str(*kept, "y"), "parent-dflt");  // the element's y did not keep it off
  EXPECT_TRUE(ok->IsSet("kids[0].y"));
  EXPECT_TRUE(ok->IsAbsent("y"));
}

TEST_F(ElementDefaults, ListsThatAppendKeepCountingIndexes) {
  auto m = New();
  auto r = pxf::UnmarshalFull("items = [{ id = \"a\" }]\nitems = [{ id = \"b\" }]\n", m.get());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->IsSet("items[0].id"));
  EXPECT_TRUE(r->IsSet("items[1].id"));
}

TEST_F(ElementDefaults, MapKeyInAPathIsQuoted) {
  auto m = New();
  auto r = pxf::UnmarshalFull(
      "by_key = { \"a\\\"b\": { id = \"x\" } }\nby_num = { 7: { id = \"y\" } }\n", m.get());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->IsSet("by_key[\"a\\\"b\"].id"));
  EXPECT_TRUE(r->IsSet("by_num[7].id"));
}

TEST_F(ElementDefaults, AnAnyBodyIsRecordedUnderItsOwnPath) {
  auto m = New();
  auto r = pxf::UnmarshalFull(
      "payload { @type = \"elemdefaults.v1.Item\" label = \"x\" }\n", m.get(), opts_);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->IsSet("payload.label"));
  EXPECT_TRUE(r->IsAbsent("label"));
}

TEST_F(ElementDefaults, UnmarshalAppliesNothing) {
  auto m = New();
  auto st = pxf::Unmarshal(
      "items = [{ deep {} }]\npayload { @type = \"elemdefaults.v1.Item\" }\n", m.get(), opts_);
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(Str(Elem(*m, "items", 0), "label"), "");
  EXPECT_EQ(Str(*Unpack(Sub(*m, "payload"), "Item"), "label"), "");
}

TEST_F(ElementDefaults, ATypeWithNoAnnotationsDecodesAsBefore) {
  auto m = New("Bare");
  auto r = pxf::UnmarshalFull("kids = [{ x = \"1\" }]\n", m.get());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->IsSet("kids[0].x"));
}

TEST_F(ElementDefaults, ADatasetCellGetsThePass) {
  std::istringstream in(
      "@dataset elemdefaults.v1.P (items, by_key)\n"
      "([{ pick_i = 1 }], { \"k\": { leaves = [{}] } })\n");
  auto reader = pxf::DatasetReader::Create(&in, opts_);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  auto m = New();
  auto st = (*reader)->Scan(m.get());
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(Str(Elem(*m, "items", 0), "label"), "item-dflt");
  EXPECT_EQ(Str(Elem(OnlyMapValue(*m, "by_key"), "leaves", 0), "tag"), "leaf-dflt");
}

TEST_F(ElementDefaults, ADatasetCellIsCheckedForRequired) {
  std::istringstream in("@dataset elemdefaults.v1.P (items)\n([{ deep {} }])\n");
  auto reader = pxf::DatasetReader::Create(&in, opts_);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  auto m = New();
  auto st = (*reader)->Scan(m.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("required field \"items[0].deep.n\" is absent"), std::string::npos)
      << st.ToString();
}

}  // namespace

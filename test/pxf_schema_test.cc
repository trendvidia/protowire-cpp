// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// PXF schema reserved-name validator (draft §3.13). Runtime-compiles
// small .proto fixtures via libprotoc's Importer, then exercises
// ValidateDescriptor / ValidateFile and the Unmarshal-time gate.

#include "protowire/pxf.h"
#include "protowire/pxf/annotations.h"
#include "protowire/pxf/schema.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

// Note on raw-string delimiters: clang-format treats `R"pb(...)pb"` /
// `R"proto(...)proto"` as embedded text-proto and reformats the inner
// content, which breaks our literal `syntax = "proto3";` line. Use the
// opaque `R"src(...)src"` form everywhere below.

namespace {

namespace pb = google::protobuf;

using protowire::pxf::UnmarshalOptions;
using protowire::pxf::ValidateDescriptor;
using protowire::pxf::ValidateFile;
using protowire::pxf::Violation;
using protowire::pxf::ViolationKind;

class CollectErrors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

// Compile a .proto string into a temp directory and import it; returns
// the first message descriptor in the file, or nullptr on failure (the
// failure message lands in `errors_.last_` for the caller to inspect).
class PxfSchema : public ::testing::Test {
 protected:
  void TearDown() override {
    if (!proto_path_.empty()) std::remove(proto_path_.c_str());
    for (const auto& p : extra_paths_) std::remove(p.c_str());
  }

  const pb::FileDescriptor* CompileFromString(const std::string& proto_src,
                                              const std::string& file_basename) {
    proto_path_ = std::string(::testing::TempDir()) + "/" + file_basename;
    {
      std::ofstream out(proto_path_, std::ios::binary);
      out << proto_src;
    }
    source_tree_.MapPath("", ::testing::TempDir());
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    return importer_->Import(file_basename);
  }

  // Compile several .proto sources written to the temp directory and
  // import `main_basename`; the closure tests need a schema that imports
  // another file.
  const pb::FileDescriptor* CompileFiles(
      const std::vector<std::pair<std::string, std::string>>& files,
      const std::string& main_basename) {
    for (const auto& [name, src] : files) {
      std::string path = std::string(::testing::TempDir()) + "/" + name;
      std::ofstream out(path, std::ios::binary);
      out << src;
      extra_paths_.push_back(path);
    }
    source_tree_.MapPath("", ::testing::TempDir());
    source_tree_.MapPath("", PROTO_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    return importer_->Import(main_basename);
  }

  pb::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  std::string proto_path_;
  std::vector<std::string> extra_paths_;
};

TEST_F(PxfSchema, ConformantSchemaProducesNoViolations) {
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package conformant.v1;
message Trade {
  int64 price = 1;
  int64 qty = 2;
}
)src",
                                                   "conformant.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  EXPECT_TRUE(ValidateFile(fd).empty());
  const pb::Descriptor* d = fd->message_type(0);
  EXPECT_TRUE(ValidateDescriptor(d).empty());
}

TEST_F(PxfSchema, FieldNamedNullCaught) {
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
message Row {
  int64 a = 1;
  string null = 2;
}
)src",
                                                   "bad_field.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  auto vs = ValidateFile(fd);
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].name, "null");
  EXPECT_EQ(vs[0].kind, ViolationKind::kField);
  EXPECT_EQ(vs[0].element, "bad.v1.Row.null");
  EXPECT_NE(vs[0].ToString().find("PXF-reserved name \"null\""), std::string::npos);
}

TEST_F(PxfSchema, OneofNamedTrueCaught) {
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
message Row {
  oneof true {
    string s = 1;
    int64 n = 2;
  }
}
)src",
                                                   "bad_oneof.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  auto vs = ValidateFile(fd);
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].name, "true");
  EXPECT_EQ(vs[0].kind, ViolationKind::kOneof);
}

TEST_F(PxfSchema, EnumValueNamedFalseCaught) {
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
enum Side {
  SIDE_UNSPECIFIED = 0;
  false = 1;
}
)src",
                                                   "bad_enum.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  auto vs = ValidateFile(fd);
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].name, "false");
  EXPECT_EQ(vs[0].kind, ViolationKind::kEnumValue);
}

TEST_F(PxfSchema, NestedEnumValueCaught) {
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
message Row {
  enum Side {
    SIDE_UNSPECIFIED = 0;
    null = 1;
  }
  Side s = 1;
}
)src",
                                                   "bad_nested_enum.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  auto vs = ValidateFile(fd);
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].kind, ViolationKind::kEnumValue);
  // proto3 scopes enum value names at the enum's parent, not under the
  // enum name — so a `null` value inside `enum Side {}` nested in
  // `message Row {}` resolves to "bad.v1.Row.null".
  EXPECT_EQ(vs[0].element, "bad.v1.Row.null");
}

TEST_F(PxfSchema, NestedMessageFieldCaught) {
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
message Outer {
  message Inner {
    int64 true = 1;
  }
  Inner i = 1;
}
)src",
                                                   "bad_nested_msg.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  auto vs = ValidateFile(fd);
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].kind, ViolationKind::kField);
  EXPECT_EQ(vs[0].element, "bad.v1.Outer.Inner.true");
}

TEST_F(PxfSchema, CaseSensitiveCheck) {
  // Capitalized variants don't lex as PXF keywords and must pass.
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package ok.v1;
message Row {
  int64 NULL = 1;
  string True = 2;
}
)src",
                                                   "case.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  EXPECT_TRUE(ValidateFile(fd).empty());
}

TEST_F(PxfSchema, MultipleViolationsSortedByElement) {
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
message Row {
  string null = 1;
  int64 false = 2;
}
enum E {
  E_UNSPECIFIED = 0;
  true = 1;
}
)src",
                                                   "bad_multi.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  auto vs = ValidateFile(fd);
  ASSERT_EQ(vs.size(), 3u);
  // Enum value names live at the enum's parent (the file package), not
  // under the enum name itself — so the top-level `enum E { true = 1; }`
  // resolves to "bad.v1.true". Sorted FQN order:
  //   "bad.v1.Row.false" < "bad.v1.Row.null" < "bad.v1.true"
  EXPECT_EQ(vs[0].element, "bad.v1.Row.false");
  EXPECT_EQ(vs[1].element, "bad.v1.Row.null");
  EXPECT_EQ(vs[2].element, "bad.v1.true");
}

TEST_F(PxfSchema, SyntheticOneofIgnored) {
  // proto3 `optional` fields generate a synthetic oneof per field. The
  // synthetic oneof name shadows the field name — and `optional null`
  // would produce a field violation AND a oneof violation if we didn't
  // filter synthetics. We expect exactly one violation here (the
  // field), proving the synthetic-oneof filter works.
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
message Row {
  optional int64 null = 1;
}
)src",
                                                   "bad_synthetic.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  auto vs = ValidateFile(fd);
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].kind, ViolationKind::kField);
}

TEST_F(PxfSchema, NullFileDescriptorReturnsEmpty) {
  EXPECT_TRUE(ValidateFile(nullptr).empty());
  EXPECT_TRUE(ValidateDescriptor(nullptr).empty());
}

TEST_F(PxfSchema, ViolationKindNameRendersExpectedLabels) {
  using protowire::pxf::ViolationKindName;
  EXPECT_STREQ(ViolationKindName(ViolationKind::kField), "message field");
  EXPECT_STREQ(ViolationKindName(ViolationKind::kOneof), "oneof");
  EXPECT_STREQ(ViolationKindName(ViolationKind::kEnumValue), "enum value");
}

// ---- Unmarshal-time integration (the decode gate) ------------------------

TEST_F(PxfSchema, UnmarshalRejectsSchemaWithReservedField) {
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
message Row {
  int64 a = 1;
  string null = 2;
}
)src",
                                                   "bad_unmarshal.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  pb::DynamicMessageFactory factory(importer_->pool());
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(fd->message_type(0))->New());

  auto st = protowire::pxf::Unmarshal("a = 1\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("PXF schema reserved-name violations"),
            std::string::npos);
}

TEST_F(PxfSchema, UnmarshalSkipsValidateWhenOptionSet) {
  // With skip_validate, decode proceeds even though the schema is
  // non-conformant — caller has taken responsibility. The reserved
  // field name "null" still wouldn't be reachable from PXF syntax,
  // but the gate itself is skipped, so the call succeeds against a
  // body that doesn't reference it.
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
message Row {
  int64 a = 1;
  string null = 2;
}
)src",
                                                   "skip_validate.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  pb::DynamicMessageFactory factory(importer_->pool());
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(fd->message_type(0))->New());

  UnmarshalOptions opts;
  opts.skip_validate = true;
  auto st = protowire::pxf::Unmarshal("a = 1\n", msg.get(), opts);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfSchema, UnmarshalFullAlsoGated) {
  const pb::FileDescriptor* fd = CompileFromString(R"src(
syntax = "proto3";
package bad.v1;
message Row {
  int64 a = 1;
  string null = 2;
}
)src",
                                                   "bad_full.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  pb::DynamicMessageFactory factory(importer_->pool());
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(fd->message_type(0))->New());
  auto r = protowire::pxf::UnmarshalFull("a = 1\n", msg.get());
  ASSERT_FALSE(r.ok());
  EXPECT_NE(std::string(r.status().message()).find("PXF schema reserved-name violations"),
            std::string::npos);
}

// ---- import closure (draft -01 § Scope of Bind-Time Checks; #34) ---------

// A violation declared in an imported file is reported when the importing
// file is bound, attributed to the file that declares it — for the
// reserved-name rule and for (pxf.key) placement alike.
TEST_F(PxfSchema, ImportedFileViolationsReportedWithDeclaringFile) {
  const char* b = R"(
syntax = "proto3";
package closure.v1;
import "pxf/annotations.proto";
message Imported {
  string null = 1;
  repeated string tags = 2 [(pxf.key) = "name"];
}
)";
  const char* a = R"(
syntax = "proto3";
package closure.v1;
import "b_closure.proto";
message Root { Imported inner = 1; }
)";
  const pb::FileDescriptor* fd =
      CompileFiles({{"b_closure.proto", b}, {"a_closure.proto", a}}, "a_closure.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  auto vs = ValidateFile(fd);
  ASSERT_EQ(vs.size(), 2u);
  EXPECT_EQ(vs[0].file, "b_closure.proto");
  EXPECT_EQ(vs[0].element, "closure.v1.Imported.null");
  EXPECT_EQ(vs[0].kind, protowire::pxf::ViolationKind::kField);
  EXPECT_EQ(vs[1].file, "b_closure.proto");
  EXPECT_EQ(vs[1].element, "closure.v1.Imported.tags");
  EXPECT_EQ(vs[1].kind, protowire::pxf::ViolationKind::kKeyOption);
  // The same through the bound message, and the per-decode gate refuses
  // the schema even though the document never reaches the imported type.
  const pb::Descriptor* root = importer_->pool()->FindMessageTypeByName("closure.v1.Root");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(ValidateDescriptor(root).size(), 2u);
  pb::DynamicMessageFactory factory(importer_->pool());
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(root)->New());
  auto st = protowire::pxf::Unmarshal("", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("b_closure.proto"), std::string::npos) << st.ToString();
}

// The diamond — A imports B and C, both importing D — reports D's
// violation once, and the closure's violations sort by file, then element.
TEST_F(PxfSchema, DiamondImportReportedOnceAndSortedByFile) {
  const char* d = R"(
syntax = "proto3";
package closure.v1;
message D { string true = 1; }
)";
  const char* b = R"(
syntax = "proto3";
package closure.v1;
import "d_closure.proto";
message B { D d = 1; string false = 2; }
)";
  const char* c = R"(
syntax = "proto3";
package closure.v1;
import "d_closure.proto";
message C { D d = 1; }
)";
  const char* a = R"(
syntax = "proto3";
package closure.v1;
import "b_closure.proto";
import "c_closure.proto";
message A { B b = 1; C c = 2; string null = 3; }
)";
  const pb::FileDescriptor* fd = CompileFiles({{"d_closure.proto", d},
                                               {"b_closure.proto", b},
                                               {"c_closure.proto", c},
                                               {"a_closure.proto", a}},
                                              "a_closure.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  auto vs = ValidateFile(fd);
  ASSERT_EQ(vs.size(), 3u);
  EXPECT_EQ(vs[0].file, "a_closure.proto");
  EXPECT_EQ(vs[0].element, "closure.v1.A.null");
  EXPECT_EQ(vs[1].file, "b_closure.proto");
  EXPECT_EQ(vs[1].element, "closure.v1.B.false");
  EXPECT_EQ(vs[2].file, "d_closure.proto");
  EXPECT_EQ(vs[2].element, "closure.v1.D.true");
}

// The closure of a schema that uses the annotations includes
// pxf/annotations.proto and google/protobuf/descriptor.proto; both are
// conformant, so a clean schema stays clean.
TEST_F(PxfSchema, AnnotationsClosureIsConformant) {
  const char* a = R"(
syntax = "proto3";
package closure.v1;
import "pxf/annotations.proto";
message Elem { string name = 1; }
message Root { repeated Elem items = 1 [(pxf.key) = "name"]; }
)";
  const pb::FileDescriptor* fd = CompileFiles({{"a_closure.proto", a}}, "a_closure.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  EXPECT_TRUE(ValidateFile(fd).empty());
  EXPECT_GE(fd->dependency_count(), 1);
}

// The closure walk skips google/protobuf/* on the assumption that
// Google's files carry no PXF violation. This pins it: each file the
// annotations closure pulls in is validated directly.
TEST_F(PxfSchema, GoogleProtobufFilesAreConformant) {
  const char* a = R"(
syntax = "proto3";
package closure.v1;
import "pxf/annotations.proto";
import "google/protobuf/timestamp.proto";
import "google/protobuf/duration.proto";
import "google/protobuf/wrappers.proto";
import "google/protobuf/any.proto";
import "google/protobuf/field_mask.proto";
message Root { google.protobuf.Timestamp t = 1 [(pxf.required) = true]; }
)";
  const pb::FileDescriptor* fd = CompileFiles({{"g_closure.proto", a}}, "g_closure.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  std::vector<const pb::FileDescriptor*> stack{fd};
  std::vector<std::string> seen;
  int google_files = 0;
  while (!stack.empty()) {
    const pb::FileDescriptor* f = stack.back();
    stack.pop_back();
    if (std::find(seen.begin(), seen.end(), std::string(f->name())) != seen.end()) continue;
    seen.push_back(std::string(f->name()));
    for (int i = 0; i < f->dependency_count(); ++i) stack.push_back(f->dependency(i));
    if (std::string(f->name()).rfind("google/protobuf/", 0) != 0) continue;
    ++google_files;
    // Validate the file's own declarations the way the walk would have.
    std::vector<protowire::pxf::Violation> vs;
    for (int i = 0; i < f->message_type_count(); ++i) {
      const pb::Descriptor* md = f->message_type(i);
      for (int k = 0; k < md->field_count(); ++k) {
        const std::string name(md->field(k)->name());
        EXPECT_FALSE(name == "null" || name == "true" || name == "false") << f->name();
        EXPECT_FALSE(protowire::pxf::KeyFieldName(md->field(k)).has_value()) << f->name();
      }
    }
    for (int i = 0; i < f->enum_type_count(); ++i) {
      for (int k = 0; k < f->enum_type(i)->value_count(); ++k) {
        const std::string name(f->enum_type(i)->value(k)->name());
        EXPECT_FALSE(name == "null" || name == "true" || name == "false") << f->name();
      }
    }
  }
  EXPECT_GE(google_files, 6) << "descriptor.proto and five well-known types";
  EXPECT_TRUE(ValidateFile(fd).empty());
}

}  // namespace

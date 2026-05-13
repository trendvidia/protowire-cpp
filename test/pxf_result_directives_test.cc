// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Tests for Result::Directives() / Result::Datasets() — PR 3 of the
// v0.72-v0.75 cpp catch-up. The fast-path decoder now populates the
// directive vectors on Result during UnmarshalFull, so consumers
// (chameleon's @header reader, table binders, etc.) can read the
// document-root directives after a decode call.

#include "protowire/pxf.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <memory>
#include <string>

#include <google/protobuf/compiler/importer.h>
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

class PxfResultDirectives : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", TESTDATA_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("test.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
    desc_ = importer_->pool()->FindMessageTypeByName("test.v1.AllTypes");
    ASSERT_NE(desc_, nullptr);
  }
  std::unique_ptr<pb::Message> NewAllTypes() {
    return std::unique_ptr<pb::Message>(factory_->GetPrototype(desc_)->New());
  }
  pb::compiler::DiskSourceTree source_tree_;
  SilentErrorCollector errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  const pb::Descriptor* desc_ = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
};

TEST_F(PxfResultDirectives, EmptyDocumentHasEmptyAccessors) {
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull("string_field = \"x\"\n", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  EXPECT_TRUE(r->Directives().empty());
  EXPECT_TRUE(r->Datasets().empty());
}

TEST_F(PxfResultDirectives, BareDirectiveRecorded) {
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull("@frob\nstring_field = \"x\"\n", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Directives().size(), 1u);
  EXPECT_EQ(r->Directives()[0].name, "frob");
  EXPECT_TRUE(r->Directives()[0].prefixes.empty());
  EXPECT_FALSE(r->Directives()[0].has_body);
  EXPECT_TRUE(r->Directives()[0].type.empty());
}

TEST_F(PxfResultDirectives, SinglePrefixPopulatesLegacyTypeField) {
  // v0.72.0-era chameleon shape: `@header Type { ... }`.
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(
      R"(@header pkg.Hdr { id = "h" }
string_field = "x"
)",
      msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Directives().size(), 1u);
  const auto& dir = r->Directives()[0];
  EXPECT_EQ(dir.name, "header");
  ASSERT_EQ(dir.prefixes.size(), 1u);
  EXPECT_EQ(dir.prefixes[0], "pkg.Hdr");
  EXPECT_EQ(dir.type, "pkg.Hdr");
  EXPECT_TRUE(dir.has_body);
  EXPECT_NE(dir.body.find("id = \"h\""), std::string::npos);
}

TEST_F(PxfResultDirectives, TwoPrefixesLeaveLegacyTypeEmpty) {
  auto msg = NewAllTypes();
  auto r =
      protowire::pxf::UnmarshalFull("@entry label pkg.MsgType\nstring_field = \"x\"\n", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Directives().size(), 1u);
  const auto& dir = r->Directives()[0];
  EXPECT_EQ(dir.name, "entry");
  ASSERT_EQ(dir.prefixes.size(), 2u);
  EXPECT_EQ(dir.prefixes[0], "label");
  EXPECT_EQ(dir.prefixes[1], "pkg.MsgType");
  EXPECT_TRUE(dir.type.empty());
}

TEST_F(PxfResultDirectives, MultipleDirectivesInSourceOrder) {
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(
      R"(@header pkg.Hdr { id = "h" }
@frob alpha beta
@meta
string_field = "x"
)",
      msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Directives().size(), 3u);
  EXPECT_EQ(r->Directives()[0].name, "header");
  EXPECT_EQ(r->Directives()[1].name, "frob");
  EXPECT_EQ(r->Directives()[2].name, "meta");
  EXPECT_EQ(r->Directives()[1].prefixes.size(), 2u);
  EXPECT_TRUE(r->Directives()[2].prefixes.empty());
}

TEST_F(PxfResultDirectives, NestedBlockBodyPreserved) {
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(
      R"(@h T { inner { a = 1 nested { b = "x" } } }
string_field = "y"
)",
      msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Directives().size(), 1u);
  // Body is the raw text between `{` and `}` (exclusive); nested
  // braces are preserved verbatim.
  EXPECT_NE(r->Directives()[0].body.find("inner {"), std::string::npos);
  EXPECT_NE(r->Directives()[0].body.find("nested {"), std::string::npos);
  EXPECT_NE(r->Directives()[0].body.find("b = \"x\""), std::string::npos);
}

TEST_F(PxfResultDirectives, AtTypeNotRecordedAsDirective) {
  // @type is a distinct directive form; it populates the document's
  // type URL handling, not Result::Directives().
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(
      R"(@type test.v1.AllTypes
@frob alpha
string_field = "x"
)",
      msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Directives().size(), 1u);
  EXPECT_EQ(r->Directives()[0].name, "frob");
}

// ---- @dataset ---------------------------------------------------------------

TEST_F(PxfResultDirectives, TableRecordedWithColumnsAndRows) {
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(R"(@dataset trades.v1.Trade ( px, qty )
( 100, 5 )
( 101, 7 )
)",
                                         msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Datasets().size(), 1u);
  const auto& t = r->Datasets()[0];
  EXPECT_EQ(t.type, "trades.v1.Trade");
  ASSERT_EQ(t.columns.size(), 2u);
  EXPECT_EQ(t.columns[0], "px");
  EXPECT_EQ(t.columns[1], "qty");
  ASSERT_EQ(t.rows.size(), 2u);
  EXPECT_EQ(t.rows[0].cells.size(), 2u);
  EXPECT_EQ(t.rows[1].cells.size(), 2u);
  EXPECT_TRUE(r->Directives().empty());
}

TEST_F(PxfResultDirectives, TableCellsCarryActualValues) {
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(R"(@dataset x.Row ( a, b, c )
( 42, "hello", true )
)",
                                         msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Datasets().size(), 1u);
  const auto& row = r->Datasets()[0].rows[0];
  ASSERT_EQ(row.cells.size(), 3u);
  // Cell 0: IntVal with raw "42".
  ASSERT_TRUE(row.cells[0].has_value());
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<protowire::pxf::IntVal>>(*row.cells[0]));
  EXPECT_EQ(std::get<std::unique_ptr<protowire::pxf::IntVal>>(*row.cells[0])->raw, "42");
  // Cell 1: StringVal with value "hello".
  ASSERT_TRUE(row.cells[1].has_value());
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<protowire::pxf::StringVal>>(*row.cells[1]));
  EXPECT_EQ(std::get<std::unique_ptr<protowire::pxf::StringVal>>(*row.cells[1])->value, "hello");
  // Cell 2: BoolVal true.
  ASSERT_TRUE(row.cells[2].has_value());
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<protowire::pxf::BoolVal>>(*row.cells[2]));
  EXPECT_TRUE(std::get<std::unique_ptr<protowire::pxf::BoolVal>>(*row.cells[2])->value);
}

TEST_F(PxfResultDirectives, TableThreeStateCells) {
  // Empty cell = nullopt (absent); `null` literal = present-but-null
  // (cell holds a NullVal); value = present-with-value.
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(R"(@dataset x.Row ( a, b, c )
( 1, , null )
)",
                                         msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Datasets().size(), 1u);
  const auto& row = r->Datasets()[0].rows[0];
  ASSERT_EQ(row.cells.size(), 3u);
  EXPECT_TRUE(row.cells[0].has_value());   // present
  EXPECT_FALSE(row.cells[1].has_value());  // absent
  ASSERT_TRUE(row.cells[2].has_value());   // present-but-null
  EXPECT_TRUE(std::holds_alternative<std::unique_ptr<protowire::pxf::NullVal>>(*row.cells[2]));
}

TEST_F(PxfResultDirectives, MultipleTablesInOrder) {
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(R"(@dataset a.Row ( x )
( 1 )
@dataset b.Row ( y, z )
( "p", "q" )
)",
                                         msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Datasets().size(), 2u);
  EXPECT_EQ(r->Datasets()[0].type, "a.Row");
  EXPECT_EQ(r->Datasets()[1].type, "b.Row");
}

TEST_F(PxfResultDirectives, TableLeavesDirectivesEmpty) {
  // Cross-check that @dataset populates only Datasets(), not Directives().
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull("@dataset x.Row ( a )\n( 1 )\n", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  EXPECT_EQ(r->Datasets().size(), 1u);
  EXPECT_TRUE(r->Directives().empty());
}

TEST_F(PxfResultDirectives, DirectivesAndTablesCanCoexist) {
  // Note: a doc with @dataset can NOT have @type or body entries, but it
  // CAN carry generic @<directive>s before the @dataset.
  auto msg = NewAllTypes();
  auto r = protowire::pxf::UnmarshalFull(R"(@header pkg.Hdr { id = "h" }
@dataset x.Row ( a )
( 1 )
)",
                                         msg.get());
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->Directives().size(), 1u);
  ASSERT_EQ(r->Datasets().size(), 1u);
  EXPECT_EQ(r->Directives()[0].name, "header");
  EXPECT_EQ(r->Datasets()[0].type, "x.Row");
}

// ---- Discard path: Unmarshal (no Result) -------------------------------

TEST_F(PxfResultDirectives, UnmarshalWithoutResultStillSucceeds) {
  // Unmarshal (vs UnmarshalFull) passes nullptr for Result; the fast
  // path must still walk the directives correctly but not allocate
  // anywhere. This is an end-to-end check that the result_-null branch
  // didn't regress.
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(
      R"(@header pkg.Hdr { id = "h" }
@frob alpha beta
string_field = "x"
)",
      msg.get());
  EXPECT_TRUE(st.ok()) << st.message();
}

}  // namespace

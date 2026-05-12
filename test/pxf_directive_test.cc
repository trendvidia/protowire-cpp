// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Parser-tier tests for the v0.72-v0.75 directive grammar:
//   - @<name> *(<prefix>) [{ ... }]   (draft §3.4.2)
//   - @entry  *(<prefix>) [{ ... }]   (draft §3.4.3)
//   - @table  <type> ( cols ) row*    (draft §3.4.4)
//
// These exercise Parse(...) directly and assert on AST shape — they do
// NOT decode against a proto descriptor. Decode-tier wiring (Result
// accessors, TableReader, BindRow) arrives in later PRs of the
// v0.72-v0.75 cpp catch-up sequence.

#include "protowire/pxf.h"
#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <memory>
#include <string>
#include <string_view>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;

using protowire::pxf::Document;
using protowire::pxf::Parse;
using protowire::pxf::TableDirective;

class SilentErrorCollector : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

// Fixture for fast-path decode tests: runtime-compiles test.proto and
// exposes a fresh test.v1.AllTypes message per test. Mirrors the
// existing PxfFast / PxfDecode fixtures in this directory.
class PxfDirectiveFast : public ::testing::Test {
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

TEST(Directive, BareNameNoBodyNoPrefix) {
  std::string_view src = R"(@frob
name = "x"
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_EQ(doc->directives[0].name, "frob");
  EXPECT_TRUE(doc->directives[0].prefixes.empty());
  EXPECT_FALSE(doc->directives[0].has_body);
  EXPECT_TRUE(doc->directives[0].type.empty());
  ASSERT_EQ(doc->entries.size(), 1u);
}

TEST(Directive, SinglePrefixPopulatesLegacyType) {
  // v0.72.0-era chameleon shape: `@header Type { ... }`.
  std::string_view src = R"(@header chameleon.v1.LayerHeader { id = "x" }
body = "z"
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_EQ(doc->directives[0].name, "header");
  ASSERT_EQ(doc->directives[0].prefixes.size(), 1u);
  EXPECT_EQ(doc->directives[0].prefixes[0], "chameleon.v1.LayerHeader");
  EXPECT_EQ(doc->directives[0].type, "chameleon.v1.LayerHeader");
  EXPECT_TRUE(doc->directives[0].has_body);
  EXPECT_NE(doc->directives[0].body.find("id = \"x\""), std::string::npos);
  ASSERT_EQ(doc->entries.size(), 1u);
}

TEST(Directive, TwoPrefixesNoLegacyType) {
  // `@entry label dotted.Type { ... }` — two prefixes leave .type empty.
  std::string_view src = R"(@entry mylabel pkg.MsgType { x = 1 }
name = "z"
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_EQ(doc->directives[0].name, "entry");
  ASSERT_EQ(doc->directives[0].prefixes.size(), 2u);
  EXPECT_EQ(doc->directives[0].prefixes[0], "mylabel");
  EXPECT_EQ(doc->directives[0].prefixes[1], "pkg.MsgType");
  EXPECT_TRUE(doc->directives[0].type.empty());
}

TEST(Directive, PrefixLookaheadStopsAtBodyKey) {
  // `@foo BarType\nbody_key = ...`: BarType is a prefix, body_key is the
  // first body assignment. Lookahead disambiguates because body_key is
  // followed by `=`.
  std::string_view src = R"(@foo BarType
body_key = "x"
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_EQ(doc->directives[0].name, "foo");
  ASSERT_EQ(doc->directives[0].prefixes.size(), 1u);
  EXPECT_EQ(doc->directives[0].prefixes[0], "BarType");
  ASSERT_EQ(doc->entries.size(), 1u);
}

TEST(Directive, MultipleDirectivesAndType) {
  std::string_view src = R"(@type some.MsgType
@header pkg.Header { id = "h1" }
@frob alpha beta
name = "z"
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  EXPECT_EQ(doc->type_url, "some.MsgType");
  ASSERT_EQ(doc->directives.size(), 2u);
  EXPECT_EQ(doc->directives[0].name, "header");
  EXPECT_EQ(doc->directives[1].name, "frob");
  EXPECT_EQ(doc->directives[1].prefixes.size(), 2u);
  EXPECT_GT(doc->body_offset, 0);
}

TEST(Directive, BodyOffsetMatchesAfterDirective) {
  std::string_view src = "@frob alpha\nname = 1\n";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  // body_offset should land on or right after the "alpha" prefix (end
  // of the directive's last token); chameleon hashes from this point.
  // Last token "alpha" starts at offset 6 (after "@frob ") and has
  // length 5, so end = 11.
  EXPECT_EQ(doc->body_offset, 11);
}

TEST(Directive, BlockBodyRawBytesPreserved) {
  std::string_view src = "@hdr T { a = 1\n b = \"x\" }\nrest = 0\n";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_TRUE(doc->directives[0].has_body);
  // Body is the raw text between `{` and `}` (exclusive); whitespace
  // and newlines are preserved verbatim.
  EXPECT_NE(doc->directives[0].body.find("a = 1"), std::string::npos);
  EXPECT_NE(doc->directives[0].body.find("b = \"x\""), std::string::npos);
  EXPECT_EQ(doc->directives[0].body.find('}'), std::string::npos);
}

TEST(Directive, NestedBracesInBody) {
  std::string_view src = "@nested T { inner { a = 1 } }\n";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_TRUE(doc->directives[0].has_body);
  EXPECT_NE(doc->directives[0].body.find("inner { a = 1 }"), std::string::npos);
}

TEST(Directive, BracesInsideStringNotCounted) {
  std::string_view src = "@s T { a = \"}{\" }\n";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_TRUE(doc->directives[0].has_body);
}

TEST(Table, BasicTwoColumnsTwoRows) {
  std::string_view src = R"(@table trades.v1.Trade ( px, qty )
( 100, 5 )
( 101, 7 )
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->tables.size(), 1u);
  const TableDirective& t = doc->tables[0];
  EXPECT_EQ(t.type, "trades.v1.Trade");
  ASSERT_EQ(t.columns.size(), 2u);
  EXPECT_EQ(t.columns[0], "px");
  EXPECT_EQ(t.columns[1], "qty");
  ASSERT_EQ(t.rows.size(), 2u);
  EXPECT_EQ(t.rows[0].cells.size(), 2u);
  EXPECT_EQ(t.rows[1].cells.size(), 2u);
  EXPECT_TRUE(t.rows[0].cells[0].has_value());
  EXPECT_TRUE(t.rows[0].cells[1].has_value());
}

TEST(Table, EmptyCellMeansAbsentField) {
  // The middle cell is empty (no value between two commas) — distinct
  // from `null` (present-but-null) per the three-state cell grammar.
  std::string_view src = R"(@table x.Row ( a, b, c )
( 1, , 3 )
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->tables.size(), 1u);
  const auto& row = doc->tables[0].rows[0];
  ASSERT_EQ(row.cells.size(), 3u);
  EXPECT_TRUE(row.cells[0].has_value());
  EXPECT_FALSE(row.cells[1].has_value());  // absent
  EXPECT_TRUE(row.cells[2].has_value());
}

TEST(Table, NullCellMeansPresentNull) {
  std::string_view src = R"(@table x.Row ( a, b )
( 1, null )
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->tables.size(), 1u);
  const auto& row = doc->tables[0].rows[0];
  ASSERT_EQ(row.cells.size(), 2u);
  EXPECT_TRUE(row.cells[1].has_value());  // present-but-null is not nullopt
}

TEST(Table, ZeroRowsOk) {
  std::string_view src = "@table x.Row ( a, b )\n";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->tables.size(), 1u);
  EXPECT_EQ(doc->tables[0].rows.size(), 0u);
}

TEST(Table, ArityMismatchRejected) {
  std::string_view src = R"(@table x.Row ( a, b )
( 1, 2, 3 )
)";
  auto doc = Parse(src);
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("3 cells, expected 2"), std::string::npos);
}

TEST(Table, DottedColumnRejected) {
  std::string_view src = "@table x.Row ( a.b )\n";
  auto doc = Parse(src);
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("dotted column"), std::string::npos);
}

TEST(Table, ListCellRejected) {
  std::string_view src = R"(@table x.Row ( a )
( [1, 2] )
)";
  auto doc = Parse(src);
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("list values"), std::string::npos);
}

TEST(Table, BlockCellRejected) {
  std::string_view src = R"(@table x.Row ( a )
( { x = 1 } )
)";
  auto doc = Parse(src);
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("block values"), std::string::npos);
}

TEST(Table, StandaloneRejectsCoexistingAtType) {
  std::string_view src = R"(@type some.Other
@table x.Row ( a )
( 1 )
)";
  auto doc = Parse(src);
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("cannot coexist with @type"), std::string::npos);
}

TEST(Table, StandaloneRejectsCoexistingBodyEntries) {
  std::string_view src = R"(@table x.Row ( a )
( 1 )
extra = 5
)";
  auto doc = Parse(src);
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("cannot coexist with top-level field entries"),
            std::string::npos);
}

TEST(Directive, AtSignAloneIsIllegal) {
  // The lexer rejects a bare `@` with no name — the parser surfaces
  // this as an "expected value" / "expected =" depending on context.
  std::string_view src = "@\n";
  auto doc = Parse(src);
  EXPECT_FALSE(doc.ok());
}

// ---- AST-tier error paths --------------------------------------------------

TEST(Directive, AtTypeWithoutIdentRejected) {
  auto doc = Parse("@type =\n");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("expected type name after @type"), std::string::npos);
}

TEST(Directive, AtTypeAfterTableRejected) {
  // Reverse order of the "type before table" violation: @table first,
  // then @type — exercises the symmetric branch in ParseDocument.
  auto doc = Parse("@table x.Row ( a )\n@type other.Msg\n");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("cannot coexist with @type"), std::string::npos);
}

TEST(Table, MissingTypeRejected) {
  auto doc = Parse("@table ( a )\n");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("expected row message type after @table"),
            std::string::npos);
}

TEST(Table, MissingLParenAfterTypeRejected) {
  auto doc = Parse("@table x.Row a, b\n");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("expected '(' to start @table column list"),
            std::string::npos);
}

TEST(Table, EmptyColumnListRejected) {
  auto doc = Parse("@table x.Row ( )\n");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("at least one field name"), std::string::npos);
}

TEST(Table, BadTokenInColumnListRejected) {
  // Integer literal where a field name is expected.
  auto doc = Parse("@table x.Row ( a, 123 )\n");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("expected column field name"), std::string::npos);
}

TEST(Table, MissingCommaOrRParenInColumnListRejected) {
  auto doc = Parse("@table x.Row ( a b )\n");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("expected ',' or ')' in @table column list"),
            std::string::npos);
}

TEST(Table, MissingCommaOrRParenInRowRejected) {
  auto doc = Parse("@table x.Row ( a, b )\n( 1 2 )\n");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("expected ',' or ')' in @table row"), std::string::npos);
}

TEST(Directive, TrailingCommentInBlockBody) {
  // Exercise FindMatchingBrace's # line-comment skip.
  auto doc = Parse("@hdr T { a = 1  # trailing comment with } in it\n  b = 2\n}\n");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_TRUE(doc->directives[0].has_body);
}

TEST(Directive, DoubleSlashCommentInBlockBody) {
  auto doc = Parse("@hdr T { a = 1  // trailing } comment\n  b = 2\n}\n");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
}

TEST(Directive, BlockCommentInBlockBody) {
  // Exercise FindMatchingBrace's /* ... */ skip — a `}` inside the
  // block comment must not close the directive body.
  auto doc = Parse("@hdr T { a = 1 /* not a } close */ b = 2 }\n");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_TRUE(doc->directives[0].has_body);
}

TEST(Directive, BytesLiteralInBlockBody) {
  // Exercise FindMatchingBrace's `b"..."` skip. cpp's lexer enforces
  // base64 in bytes literals so `}` can never appear inside one — but
  // the skip path still walks the literal's characters when scanning
  // for the closing brace of the directive body.
  auto doc = Parse("@hdr T { blob = b\"YWJjZGVm\" }\n");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_TRUE(doc->directives[0].has_body);
  EXPECT_NE(doc->directives[0].body.find("YWJjZGVm"), std::string::npos);
}

TEST(Table, ZeroOnePrefixZeroLegacyType) {
  // Zero prefixes — `.type` stays empty (back-compat shape only kicks
  // in with exactly one prefix).
  auto doc = Parse("@bare\nx = 1\n");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->directives.size(), 1u);
  EXPECT_TRUE(doc->directives[0].type.empty());
  EXPECT_TRUE(doc->directives[0].prefixes.empty());
}

// ---- Fast-path (Unmarshal) tests, exercising decode_fast.cc ---------------

TEST_F(PxfDirectiveFast, BareDirectiveSkippedThenBodyDecodes) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@frob
string_field = "hi"
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(msg->GetReflection()->GetString(*msg, desc_->FindFieldByName("string_field")), "hi");
}

TEST_F(PxfDirectiveFast, SinglePrefixDirectiveSkipped) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@header pkg.Hdr
string_field = "x"
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfDirectiveFast, MultiplePrefixesSkipped) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@frob alpha beta gamma
string_field = "x"
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfDirectiveFast, BlockBodyDirectiveSkipped) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@header pkg.Hdr { id = "h1" version = 7 }
string_field = "x"
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfDirectiveFast, NestedBlocksInDirectiveBodySkipped) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@h T { inner { a = 1 nested { b = "x" } } }
string_field = "x"
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfDirectiveFast, MultipleDirectivesInterleavedWithType) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@type test.v1.AllTypes
@header pkg.Hdr { id = "h" }
@frob alpha
string_field = "x"
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfDirectiveFast, AtTypeBadIdentRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal("@type =\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("expected type name after @type"), std::string::npos);
}

TEST_F(PxfDirectiveFast, AtTypeAcceptsStringForm) {
  // Spec allows `@type "..."` (string form), which the AST parser
  // doesn't accept but the fast path does for back-compat with the
  // Any-sugar convention.
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@type "test.v1.AllTypes"
string_field = "x"
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfDirectiveFast, AtTypeAfterTableRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@table x.Row ( a )
@type other
)",
                                      msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("cannot coexist with @type"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableAfterTypeRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@type other
@table x.Row ( a )
)",
                                      msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("cannot coexist with @type"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableWithRowsAndStandalone) {
  // Fast path drops the @table rows in PR 1; the call succeeds because
  // the doc is well-formed (no @type, no body entries). PR 4 will
  // make Result.tables() expose the rows.
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@table trades.v1.Trade ( px, qty )
( 100, 5 )
( 101, 7 )
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfDirectiveFast, TableWithEmptyAndNullCells) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@table x.Row ( a, b, c )
( 1, , null )
( null, , 9 )
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfDirectiveFast, TableZeroRows) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal("@table x.Row ( a, b )\n", msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

TEST_F(PxfDirectiveFast, TableArityMismatchRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@table x.Row ( a, b )
( 1, 2, 3 )
)",
                                      msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("3 cells, expected 2"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableDottedColumnRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal("@table x.Row ( a.b )\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("dotted path"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableListCellRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@table x.Row ( a )
( [1, 2] )
)",
                                      msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("list/block"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableBlockCellRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@table x.Row ( a )
( { x = 1 } )
)",
                                      msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("list/block"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableMissingTypeRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal("@table ( a )\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("expected row message type after @table"),
            std::string::npos);
}

TEST_F(PxfDirectiveFast, TableMissingLParenRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal("@table x.Row a, b\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("expected '(' to start"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableEmptyColumnsRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal("@table x.Row ( )\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("at least one field name"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableBadColumnTokenRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal("@table x.Row ( a, 123 )\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("expected column field name"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableMissingCommaInColumnListRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal("@table x.Row ( a b )\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("expected ',' or ')' in @table column list"),
            std::string::npos);
}

TEST_F(PxfDirectiveFast, TableMissingCommaInRowRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal("@table x.Row ( a, b )\n( 1 2 )\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("expected ',' or ')' in @table row"), std::string::npos);
}

TEST_F(PxfDirectiveFast, TableWithBodyEntriesRejected) {
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@table x.Row ( a )
( 1 )
string_field = "extra"
)",
                                      msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(std::string(st.message()).find("cannot coexist with top-level field entries"),
            std::string::npos);
}

TEST_F(PxfDirectiveFast, PrefixLookaheadStopsAtBodyKey) {
  // `@frob alpha\nstring_field = "x"` — alpha is a directive prefix
  // (the next token is a newline, not `=`/`:`), but string_field is
  // the first body entry (followed by `=`).
  auto msg = NewAllTypes();
  auto st = protowire::pxf::Unmarshal(R"(@frob alpha
string_field = "x"
)",
                                      msg.get());
  ASSERT_TRUE(st.ok()) << st.message();
}

}  // namespace

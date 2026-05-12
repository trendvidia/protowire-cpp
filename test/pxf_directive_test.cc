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

#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

using protowire::pxf::Document;
using protowire::pxf::Parse;
using protowire::pxf::TableDirective;

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

}  // namespace

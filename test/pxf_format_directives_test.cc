// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// FormatDocument keeps every directive, with its dataset rows, in document
// order (trendvidia/protowire#558, #130). Port of protowire-go's
// encoding/pxf/format_directives_test.go (protowire-go#262); the expected
// texts are the ones protowire-go and protowire-typescript write.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "protowire/pxf/format.h"
#include "protowire/pxf/parser.h"

namespace {

using protowire::pxf::Document;
using protowire::pxf::FormatDocument;
using protowire::pxf::FormatValue;
using protowire::pxf::Parse;
using protowire::pxf::ProtoShapeName;

// DirectiveShape is what a document's directives say, without positions,
// in the order the formatter writes them.
std::vector<std::string> DirectiveShape(const Document& doc) {
  std::vector<std::string> out;
  out.push_back(doc.type_url);
  for (const auto& d : doc.directives) {
    out.push_back("@" + d.name);
    std::string prefixes;
    for (const auto& p : d.prefixes) prefixes += p + " ";
    out.push_back(prefixes);
    out.push_back(d.body);
    out.push_back(d.has_body ? "body" : "no body");
  }
  for (const auto& d : doc.datasets) {
    out.push_back("@dataset");
    out.push_back(d.type);
    std::string cols;
    for (const auto& c : d.columns) cols += c + " ";
    out.push_back(cols);
    for (const auto& r : d.rows) {
      std::string cells;
      for (const auto& c : r.cells) cells += (c ? FormatValue(*c) : "<absent>") + "|";
      out.push_back(cells);
    }
  }
  for (const auto& d : doc.protos) {
    out.push_back("@proto");
    out.push_back(ProtoShapeName(d.shape));
    out.push_back(d.type_name);
    out.push_back(d.body);
  }
  out.push_back(std::to_string(doc.entries.size()));
  return out;
}

// FormatRoundTrip formats src, checks that the output parses to the same
// directives and is a fixed point of formatting, and returns it.
std::string FormatRoundTrip(std::string_view src) {
  auto doc = Parse(src);
  EXPECT_TRUE(doc.ok()) << doc.status().ToString();
  if (!doc.ok()) return "";
  std::string out = FormatDocument(*doc);
  auto again = Parse(out);
  EXPECT_TRUE(again.ok()) << again.status().ToString() << "\n" << out;
  if (!again.ok()) return out;
  EXPECT_EQ(DirectiveShape(*doc), DirectiveShape(*again)) << out;
  EXPECT_EQ(out, FormatDocument(*again)) << "formatting is a fixed point";
  return out;
}

TEST(FormatDirectives, Dataset) {
  // The issue's document formatted to nothing.
  EXPECT_EQ(FormatRoundTrip("@dataset trades.v1.Trade (id, px)\n(1, 2.5)\n(2, 3.5)\n"),
            "@dataset trades.v1.Trade (id, px)\n(1, 2.5)\n(2, 3.5)\n");

  // An empty cell is an absent field and null a present one; both stay.
  EXPECT_EQ(FormatRoundTrip("@dataset trades.v1.Trade (id, px, note)\n(1, 2.5, )\n(2, , null)\n"),
            "@dataset trades.v1.Trade (id, px, note)\n(1, 2.5, )\n(2, , null)\n");
}

TEST(FormatDirectives, EntryAndNamedDirective) {
  EXPECT_EQ(FormatRoundTrip("@type a.v1.M\n@entry x { a = 1 }\n@header h.v1.H\n\nname = \"n\"\n"),
            "@type a.v1.M\n@entry x { a = 1 }\n@header h.v1.H\n\nname = \"n\"\n");
  // The issue's input B, which lost its @entry.
  EXPECT_EQ(FormatRoundTrip("@type a.v1.M\n@entry x { a = 1 }\n\nname = \"n\"\n"),
            "@type a.v1.M\n@entry x { a = 1 }\n\nname = \"n\"\n");
}

TEST(FormatDirectives, ProtoShapes) {
  struct Case {
    std::string src;
    std::string want;  // empty: the source itself
  };
  const std::vector<Case> cases = {
      {"@proto { string id = 1; }\n@dataset (id)\n(\"a\")\n", ""},
      {"@proto a.v1.R { string id = 1; }\n@dataset a.v1.R (id)\n(\"a\")\n", ""},
      {"@proto \"\"\"syntax = \"proto3\"; message R { string id = 1; }\"\"\"\n@dataset R "
       "(id)\n(\"a\")\n",
       ""},
      // @type moves to the front: it binds no @proto, so this changes nothing.
      {"@proto b\"CgA=\"\n@type a.v1.M\n\nx = 1\n", "@type a.v1.M\n@proto b\"CgA=\"\n\nx = 1\n"},
      // The issue's @proto case, which formatted to nothing.
      {"@proto a.v1.P { int32 b = 1; }\n@entry x { a = 1 }\n", ""},
  };
  for (const auto& tc : cases) {
    SCOPED_TRACE(tc.src);
    EXPECT_EQ(FormatRoundTrip(tc.src), tc.want.empty() ? tc.src : tc.want);
  }
}

// An anonymous @proto binds the next typed directive, so a @proto written
// after a dataset in the source stays after it.
TEST(FormatDirectives, DirectivesKeepSourceOrder) {
  const std::string src =
      "@dataset t.v1.T (id)\n(1)\n@proto { string id = 1; }\n@dataset (id)\n(\"a\")\n";
  EXPECT_EQ(FormatRoundTrip(src), src);
}

TEST(FormatDirectives, DirectiveComments) {
  const std::string out = FormatRoundTrip(
      "# about the rows\n@dataset t.v1.T (id)\n(1)\n# second\n@dataset t.v1.U (id)\n(2)\n");
  EXPECT_NE(out.find("# about the rows\n@dataset t.v1.T (id)"), std::string::npos) << out;
  EXPECT_NE(out.find("# second\n@dataset t.v1.U (id)"), std::string::npos) << out;
}

// A document whose only directive is @type formats as it always has.
TEST(FormatDirectives, OnlyTypeUnchanged) {
  auto doc = Parse("# c\n@type a.v1.M\n\nx = 1\n");
  ASSERT_TRUE(doc.ok()) << doc.status().ToString();
  EXPECT_EQ(FormatDocument(*doc), "@type a.v1.M\n\n# c\nx = 1\n");
}

}  // namespace

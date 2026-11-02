// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Mirrors protowire-go's tolerant_test.go. scripts/go_differential.sh runs the
// fuller check, every prefix and one-byte deletion of every fixture.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "protowire/pxf/format.h"
#include "protowire/pxf/parser.h"

namespace pxf = protowire::pxf;

namespace {

pxf::TolerantResult Tolerant(const std::string& src, size_t want_errors) {
  auto r = pxf::ParseTolerant(src);
  EXPECT_EQ(r.errors.size(), want_errors) << src;
  return r;
}

std::vector<std::string> Keys(const pxf::Document& doc) {
  std::vector<std::string> out;
  for (const auto& e : doc.entries) {
    if (auto* a = std::get_if<std::unique_ptr<pxf::Assignment>>(&e)) out.push_back((*a)->key);
    if (auto* m = std::get_if<std::unique_ptr<pxf::MapEntry>>(&e)) out.push_back((*m)->key);
    if (auto* b = std::get_if<std::unique_ptr<pxf::Block>>(&e)) out.push_back((*b)->name);
  }
  return out;
}

const pxf::Assignment& Assign(const pxf::Document& doc, size_t i) {
  return *std::get<std::unique_ptr<pxf::Assignment>>(doc.entries.at(i));
}
const pxf::Block& BlockAt(const pxf::Document& doc, size_t i) {
  return *std::get<std::unique_ptr<pxf::Block>>(doc.entries.at(i));
}

template <class T>
const T* As(const pxf::ValuePtr& v) {
  auto* p = std::get_if<std::unique_ptr<T>>(&v);
  return p ? p->get() : nullptr;
}

using Strings = std::vector<std::string>;

}  // namespace

TEST(PxfTolerant, ValidInputMatchesParse) {
  const std::string src = R"(@type demo.v1.Config

# leading comment
name = "value"  # trailing
count = 42
when = 2024-01-15T10:30:00Z
wait = 1h30m
data = b"aGk="
tags = ["a", "b", "c"]
labels {
  "x": 1
}
server {
  tls { enabled = true }
}
)";
  auto strict = pxf::Parse(src);
  ASSERT_TRUE(strict.ok());
  auto r = Tolerant(src, 0);
  EXPECT_EQ(pxf::FormatDocument(r.document), pxf::FormatDocument(*strict));
  EXPECT_EQ(Assign(r.document, 0).trailing_comment, "# trailing");
}

TEST(PxfTolerant, DanglingAssignmentAnchorsPastEquals) {
  auto r = Tolerant("port =\nhost = \"h\"\n", 1);
  EXPECT_EQ(Keys(r.document), (Strings{"port", "host"}));
  const auto* bad = As<pxf::BadVal>(Assign(r.document, 0).value);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(bad->pos.line, 1);
  EXPECT_EQ(bad->pos.offset, 6);
  EXPECT_EQ(r.errors[0].pos.line, 1);
  EXPECT_EQ(As<pxf::StringVal>(Assign(r.document, 1).value)->value, "h");
}

TEST(PxfTolerant, DanglingAssignmentAtEOF) {
  auto r = Tolerant("port =", 1);
  EXPECT_EQ(Assign(r.document, 0).key, "port");
  EXPECT_NE(As<pxf::BadVal>(Assign(r.document, 0).value), nullptr);
}

TEST(PxfTolerant, BareKey) {
  auto r = Tolerant("flush\nb = 2\n", 1);
  EXPECT_EQ(Keys(r.document), (Strings{"flush", "b"}));
  const auto* bad = As<pxf::BadVal>(Assign(r.document, 0).value);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(bad->pos.offset, 5);
}

TEST(PxfTolerant, UnterminatedStrings) {
  auto eof = Tolerant(R"(type = ")", 1);
  EXPECT_EQ(As<pxf::StringVal>(Assign(eof.document, 0).value)->value, "");

  auto nl = Tolerant("name = \"ab\nport = 1\n", 1);
  EXPECT_EQ(Keys(nl.document), (Strings{"name", "port"}));
  EXPECT_EQ(As<pxf::StringVal>(Assign(nl.document, 0).value)->value, "ab");
  EXPECT_NE(nl.errors[0].msg.find("unterminated string"), std::string::npos);

  auto triple = Tolerant("s = \"\"\"\nhello\n", 1);
  EXPECT_NE(As<pxf::StringVal>(Assign(triple.document, 0).value)->value.find("hello"),
            std::string::npos);
  EXPECT_NE(triple.errors[0].msg.find("unterminated triple-quoted string"), std::string::npos);
}

TEST(PxfTolerant, UnclosedBlocksCloseAtEOF) {
  auto one = Tolerant("server {\n  port = 8080\n", 1);
  EXPECT_EQ(BlockAt(one.document, 0).entries.size(), 1u);
  EXPECT_NE(one.errors[0].msg.find("unclosed block"), std::string::npos);
  // One error per unclosed brace.
  Tolerant("a {\n b {\n  c = 1\n", 2);
}

TEST(PxfTolerant, StrayCloseBrace) {
  auto r = Tolerant("a = 1\n}\nb = 2\n", 1);
  EXPECT_EQ(Keys(r.document), (Strings{"a", "b"}));
}

TEST(PxfTolerant, UnclosedList) {
  auto r = Tolerant("xs = [1, 2", 1);
  EXPECT_EQ(As<pxf::ListVal>(Assign(r.document, 0).value)->elements.size(), 2u);
  EXPECT_NE(r.errors[0].msg.find("']'"), std::string::npos);
}

TEST(PxfTolerant, MapMissingValue) {
  auto r = Tolerant("m {\n  \"a\":\n  \"b\": 2\n}\n", 1);
  const auto& m = BlockAt(r.document, 0);
  ASSERT_EQ(m.entries.size(), 2u);
  EXPECT_NE(As<pxf::BadVal>(std::get<std::unique_ptr<pxf::MapEntry>>(m.entries[0])->value),
            nullptr);
  EXPECT_EQ(As<pxf::IntVal>(std::get<std::unique_ptr<pxf::MapEntry>>(m.entries[1])->value)->raw,
            "2");
}

TEST(PxfTolerant, MapEntryAtTopLevelIsKept) {
  auto r = Tolerant("a: 1\n", 1);
  EXPECT_TRUE(std::holds_alternative<std::unique_ptr<pxf::MapEntry>>(r.document.entries[0]));
}

TEST(PxfTolerant, IllegalTokenAndBadBase64BecomeBadVal) {
  auto illegal = Tolerant("a = $\nb = 2\n", 1);
  EXPECT_EQ(Keys(illegal.document), (Strings{"a", "b"}));
  EXPECT_NE(As<pxf::BadVal>(Assign(illegal.document, 0).value), nullptr);

  auto b64 = Tolerant(R"(k = b"!!")", 1);
  EXPECT_NE(As<pxf::BadVal>(Assign(b64.document, 0).value), nullptr);
  EXPECT_EQ(b64.errors[0].msg, "invalid base64: illegal base64 data at input byte 0");

  auto bytes = Tolerant("k = b\"aGk=\nb = 2\n", 1);
  EXPECT_EQ(Keys(bytes.document), (Strings{"k", "b"}));
}

TEST(PxfTolerant, UnterminatedBlockComment) {
  auto r = Tolerant("a = 1\n/* dangling", 1);
  EXPECT_EQ(r.document.entries.size(), 1u);
  EXPECT_NE(r.errors[0].msg.find("unterminated block comment"), std::string::npos);
}

TEST(PxfTolerant, BadEscapeKeepsItsText) {
  auto r = Tolerant("s = \"a\\qb\"\nt = 2", 1);
  EXPECT_EQ(Keys(r.document), (Strings{"s", "t"}));
  EXPECT_EQ(As<pxf::StringVal>(Assign(r.document, 0).value)->value, "a\\qb");
  EXPECT_NE(r.errors[0].msg.find("escape"), std::string::npos);
}

TEST(PxfTolerant, MalformedDirectiveIsSkipped) {
  auto r = Tolerant("@dataset (\n@type demo.v1.Config\nx = 1\n", 1);
  EXPECT_TRUE(r.document.datasets.empty());
  EXPECT_EQ(r.document.type_url, "demo.v1.Config");
  EXPECT_EQ(Keys(r.document), (Strings{"x"}));

  auto missing = Tolerant("@type\nx = 1\n", 1);
  EXPECT_EQ(missing.document.type_url, "");
  EXPECT_EQ(Keys(missing.document), (Strings{"x"}));
}

TEST(PxfTolerant, ErrorsAreInSourceOrder) {
  auto r = Tolerant("a =\nb = $\nc {\n", 3);
  for (size_t i = 1; i < r.errors.size(); ++i) {
    EXPECT_LE(r.errors[i - 1].pos.offset, r.errors[i].pos.offset);
  }
}

TEST(PxfTolerant, UnclosedCellClosesAtRowEnd) {
  auto r = Tolerant("@dataset T ( a )\n( { x = 1 )\n( 3 )\n", 1);
  ASSERT_EQ(r.document.datasets.size(), 1u);
  EXPECT_EQ(r.document.datasets[0].rows.size(), 2u);
}

TEST(PxfTolerant, EveryTruncationTerminates) {
  const std::string src = R"(@type demo.v1.Config
@header chameleon.v1.LayerHeader { id = "x" }

# comment
name = "value"
data = b"aGk="
tags = ["a", "b"]
labels { "x": 1 }
server {
  host = "h"
  tls { enabled = true }
  timeout = 30s
}
)";
  for (size_t i = 0; i <= src.size(); ++i) pxf::ParseTolerant(src.substr(0, i));
}

// A list past the depth limit is skipped, and its span ends at its own ']'.
TEST(PxfTolerant, DepthRecoverySpan) {
  std::string closed = "xs = " + std::string(101, '[') + "1";
  for (int i = 0; i < 101; ++i) closed += " ]";
  closed += "\nnext = 1\n";
  auto r = Tolerant(closed, 1);
  const pxf::ValuePtr* v = &Assign(r.document, 0).value;
  for (int i = 0; i < 100; ++i) v = &As<pxf::ListVal>(*v)->elements.at(0);
  EXPECT_EQ(pxf::ValueSpan(*v).second.offset, static_cast<int>(closed.find(']')) + 1);
  EXPECT_NE(r.errors[0].msg.find("MaxNestingDepth"), std::string::npos);
}

TEST(PxfTolerant, ParseStaysStrict) {
  for (const char* src : {"port =",
                          R"(type = ")",
                          "server {\n  port = 8080\n",
                          "xs = [1, 2",
                          "a = 1\n}\nb = 2\n",
                          "a: 1\n"}) {
    EXPECT_FALSE(pxf::Parse(src).ok()) << src;
  }
}

TEST(PxfTolerant, InputOverTheLimit) {
  pxf::ParseOptions opts;
  opts.max_message_size = 2;
  auto r = pxf::ParseTolerant("a = 1\n", opts);
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_EQ(r.errors[0].ToString(), "1:1: input of 6 bytes exceeds MaxMessageSize=2");
}

TEST(PxfSpans, EntriesAndValues) {
  auto doc = pxf::Parse("a = \"x\" # c\nb { c: 1 }\nl = [1p53, 2]\n");
  ASSERT_TRUE(doc.ok());
  auto [a_start, a_end] = pxf::EntrySpan(doc->entries[0]);
  EXPECT_EQ(a_start.offset, 0);
  EXPECT_EQ(a_end.offset, 7);
  auto [b_start, b_end] = pxf::EntrySpan(doc->entries[1]);
  EXPECT_EQ(b_start.offset, 12);
  EXPECT_EQ(b_end.offset, 22);  // past the '}'
  auto [l_start, l_end] = pxf::ValueSpan(Assign(*doc, 2).value);
  EXPECT_EQ(l_start.offset, 27);
  EXPECT_EQ(l_end.offset, 36);  // past the ']'
  const auto* list = As<pxf::ListVal>(Assign(*doc, 2).value);
  EXPECT_EQ(pxf::ValueSpan(list->elements[0]).second.offset, 32);  // its suffix included
}

TEST(PxfSpans, TypeEntryIsItsOwnEntry) {
  auto doc = pxf::Parse("a { @type = \"u\" # c\n x = 1 }\n");
  ASSERT_TRUE(doc.ok());
  const auto& t = std::get<std::unique_ptr<pxf::TypeEntry>>(BlockAt(*doc, 0).entries[0]);
  EXPECT_EQ(t->url, "u");
  EXPECT_EQ(t->trailing_comment, "# c");
  EXPECT_EQ(pxf::FormatDocument(*doc), "a {\n  @type = \"u\" # c\n  x = 1\n}\n");
}

TEST(PxfFormat, AppendValueAppends) {
  auto doc = pxf::Parse("a = \"x\"\n");
  ASSERT_TRUE(doc.ok());
  std::string out = "k=";
  pxf::AppendValue(&out, Assign(*doc, 0).value);
  EXPECT_EQ(out, "k=\"x\"");
  auto r = pxf::ParseTolerant("a =");
  EXPECT_EQ(pxf::FormatValue(Assign(r.document, 0).value), "");
}

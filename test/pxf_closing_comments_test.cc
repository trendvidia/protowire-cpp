// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Comments that precede no entry — at the end of a document or of a block,
// and around dataset rows — survive Parse → FormatDocument in place (#176),
// as they do in protowire-go (#279, PR #283). Before, the parser attached a
// comment only to the entry after it, so a comment with none after it was
// dropped, or carried out of its block onto the next entry. The documents
// and expected outputs are protowire-go's (format_comments_test.go).

#include "protowire/pxf/dataset_reader.h"
#include "protowire/pxf/format.h"
#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <streambuf>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace pxf = protowire::pxf;

// FormatRoundTrip parses src and formats it, and requires the output to parse
// and to be a fixed point of formatting.
std::string FormatRoundTrip(const std::string& src) {
  auto doc = pxf::Parse(src);
  EXPECT_TRUE(doc.ok()) << doc.status().ToString() << "\n" << src;
  if (!doc.ok()) return "";
  std::string out = pxf::FormatDocument(*doc);
  auto again = pxf::Parse(out);
  EXPECT_TRUE(again.ok()) << again.status().ToString() << "\n" << out;
  if (!again.ok()) return out;
  EXPECT_EQ(out, pxf::FormatDocument(*again)) << "formatting is not a fixed point";
  return out;
}

std::vector<std::string> Texts(const std::vector<pxf::Comment>& cs) {
  std::vector<std::string> out;
  out.reserve(cs.size());
  for (const auto& c : cs) out.push_back(c.text);
  return out;
}

TEST(ClosingComments, AtTheEnd) {
  struct Case {
    const char* name;
    std::string src;
    std::string want;  // empty: the source itself
  };
  const std::vector<Case> cases = {
      // protowire-go#279's three documents.
      {"after an entry",
       "@type a.v1.M\nname = \"n\"\n# end of doc\n",
       "@type a.v1.M\n\nname = \"n\"\n# end of doc\n"},
      {"after a directive", "@type a.v1.M\n@flag\n# end of doc\n", ""},
      {"after a row", "@dataset t.v1.T (id)\n(1)\n# end of doc\n", ""},
      {"a run of comments",
       "@type a.v1.M\nname = \"n\" # inline\n# a\n// b\n/* c */\n",
       "@type a.v1.M\n\nname = \"n\" # inline\n# a\n// b\n/* c */\n"},
      {"after @type alone", "@type a.v1.M\n# only\n", "@type a.v1.M\n\n# only\n"},
  };
  for (const auto& c : cases) {
    EXPECT_EQ(FormatRoundTrip(c.src), c.want.empty() ? c.src : c.want) << c.name;
  }

  auto doc = pxf::Parse("@type a.v1.M\nname = \"n\"\n# end of doc\n");
  ASSERT_TRUE(doc.ok()) << doc.status().ToString();
  EXPECT_EQ(Texts(doc->trailing_comments), std::vector<std::string>{"# end of doc"});
  const auto* a = std::get_if<std::unique_ptr<pxf::Assignment>>(&doc->entries[0]);
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE((*a)->leading_comments.empty());
}

TEST(ClosingComments, BeforeTheClosingBrace) {
  const std::vector<std::pair<const char*, std::string>> cases = {
      // Before, this comment moved out of m, onto y.
      {"block then entry", "@type a.v1.M\n\nm {\n  k: 1\n  # end of m\n}\ny = 2\n"},
      {"last block", "@type a.v1.M\n\nb {\n  x = 1\n  # end of b\n}\n"},
      {"empty block", "@type a.v1.M\n\nb {\n  # empty\n}\n"},
      {"block value", "@type a.v1.M\n\nb = {\n  x = 1\n  # end of b\n}\n"},
      {"nested", "@type a.v1.M\n\na {\n  b {\n    x = 1\n    # end of b\n  }\n  # end of a\n}\n"},
      {"after the brace stays after", "@type a.v1.M\n\na {\n  x = 1\n}\n# about y\ny = 2\n"},
      {"block cell", "@dataset t.v1.T (id, m)\n(1, {\n  x = 1\n  # end of m\n})\n"},
  };
  for (const auto& [name, src] : cases) EXPECT_EQ(FormatRoundTrip(src), src) << name;

  auto doc = pxf::Parse("@type a.v1.M\nm {\n  k: 1\n  # end of m\n}\ny = 2\n");
  ASSERT_TRUE(doc.ok()) << doc.status().ToString();
  const auto* m = std::get_if<std::unique_ptr<pxf::Block>>(&doc->entries[0]);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(Texts((*m)->trailing_comments), std::vector<std::string>{"# end of m"});
  const auto* y = std::get_if<std::unique_ptr<pxf::Assignment>>(&doc->entries[1]);
  ASSERT_NE(y, nullptr);
  EXPECT_TRUE((*y)->leading_comments.empty());
}

TEST(ClosingComments, DatasetRows) {
  const std::string src = "@dataset t.v1.T (id)\n# first\n(1)\n# between\n(2)\n# end\n";
  EXPECT_EQ(FormatRoundTrip(src), src);

  // A row is written on one line, so a comment inside it moves before it.
  EXPECT_EQ(FormatRoundTrip("@dataset t.v1.T (a, b)\n(1, # one\n 2)\n(3, 4)\n"),
            "@dataset t.v1.T (a, b)\n# one\n(1, 2)\n(3, 4)\n");

  auto doc = pxf::Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().ToString();
  const auto& rows = doc->datasets[0].rows;
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(Texts(rows[0].leading_comments), std::vector<std::string>{"# first"});
  EXPECT_EQ(Texts(rows[1].leading_comments), std::vector<std::string>{"# between"});
  EXPECT_EQ(Texts(doc->trailing_comments), std::vector<std::string>{"# end"});
}

// OneByteBuf serves its bytes one per read, so every token can be cut by a
// read's end.
class OneByteBuf : public std::streambuf {
 public:
  explicit OneByteBuf(std::string data) : data_(std::move(data)) {}

 protected:
  int_type underflow() override {
    if (gptr() != nullptr && gptr() < egptr()) return traits_type::to_int_type(*gptr());
    if (next_ >= data_.size()) return traits_type::eof();
    char* at = &data_[next_++];
    setg(at, at, at + 1);
    return traits_type::to_int_type(*at);
  }

 private:
  std::string data_;
  size_t next_ = 0;
};

struct RowComments {
  pxf::Position pos;
  std::vector<pxf::Comment> comments;
};

std::vector<RowComments> StreamRows(std::istream* in) {
  std::vector<RowComments> out;
  auto tr = pxf::DatasetReader::Create(in);
  EXPECT_TRUE(tr.ok()) << tr.status().ToString();
  if (!tr.ok()) return out;
  for (;;) {
    pxf::DatasetRow row;
    auto st = (*tr)->Next(&row);
    EXPECT_TRUE(st.ok()) << st.ToString();
    if (!st.ok() || (*tr)->Done()) break;
    out.push_back({row.pos, row.leading_comments});
  }
  return out;
}

void ExpectSameComments(const std::vector<RowComments>& got,
                        const std::vector<pxf::DatasetRow>& want,
                        const std::string& how) {
  ASSERT_EQ(got.size(), want.size()) << how;
  for (size_t i = 0; i < want.size(); ++i) {
    EXPECT_EQ(got[i].pos.offset, want[i].pos.offset) << how << " row " << i;
    ASSERT_EQ(got[i].comments.size(), want[i].leading_comments.size()) << how << " row " << i;
    for (size_t j = 0; j < want[i].leading_comments.size(); ++j) {
      const auto& g = got[i].comments[j];
      const auto& w = want[i].leading_comments[j];
      EXPECT_EQ(g.text, w.text) << how << " row " << i;
      EXPECT_EQ(g.pos.line, w.pos.line) << how << " row " << i;
      EXPECT_EQ(g.pos.column, w.pos.column) << how << " row " << i;
      EXPECT_EQ(g.pos.offset, w.pos.offset) << how << " row " << i;
    }
  }
}

// DatasetReader files a row's comments as Parse does, read whole or a byte
// at a time.
TEST(ClosingComments, DatasetReaderRowComments) {
  for (const char* body : {"# first\n(1, 2)\n# between\n/* b */\n(3, 4)\n# end\n",
                           "(1, # one\n 2)\n(3, 4) # after\n(5, 6)\n",
                           "(1, 2)\n# about U\n@dataset U (z)\n(9)\n"}) {
    const std::string in = std::string("@dataset T (a, b)\n") + body;
    auto doc = pxf::Parse(in);
    ASSERT_TRUE(doc.ok()) << doc.status().ToString();
    const auto& want = doc->datasets[0].rows;

    std::istringstream whole(in);
    ExpectSameComments(StreamRows(&whole), want, std::string("whole: ") + body);
    OneByteBuf buf(in);
    std::istream bytes(&buf);
    ExpectSameComments(StreamRows(&bytes), want, std::string("one byte: ") + body);
  }

  std::istringstream src("@dataset T (a, b)\n(1, 2)\n# between\n(3, 4)\n");
  auto rows = StreamRows(&src);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(Texts(rows[1].comments), std::vector<std::string>{"# between"});
}

}  // namespace

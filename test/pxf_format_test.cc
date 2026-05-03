// Tests for the comment-preserving AST formatter (FormatDocument).
// Mirrors a Parse → Format → Parse round-trip; comments must be preserved
// across the round-trip and string values must round-trip byte-for-byte
// (including the new escape set: \\xHH for control bytes, multi-byte UTF-8,
// embedded NUL, embedded `"`).

#include "protowire/pxf/format.h"
#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>

namespace {

using protowire::pxf::Assignment;
using protowire::pxf::Block;
using protowire::pxf::Document;
using protowire::pxf::FormatDocument;
using protowire::pxf::MapEntry;
using protowire::pxf::Parse;
using protowire::pxf::StringVal;

const StringVal* GetStringVal(const protowire::pxf::ValuePtr& v) {
  if (auto* p = std::get_if<std::unique_ptr<StringVal>>(&v)) {
    return p->get();
  }
  return nullptr;
}

const Assignment* FindAssignment(const Document& doc,
                                 std::string_view key) {
  for (const auto& e : doc.entries) {
    if (auto* p = std::get_if<std::unique_ptr<Assignment>>(&e)) {
      if ((*p)->key == key) return p->get();
    }
  }
  return nullptr;
}

TEST(Format, RoundTripsStringValues) {
  // Each value should round-trip byte-for-byte through Parse/Format/Parse.
  std::vector<std::string> cases = {
      "hello",
      "with space",
      "embedded \" quote",
      "back\\slash",
      "tab\there",
      "newline\nin\nstring",
      "control \x01 byte",
      "café 日本 😀",
      std::string("nul\0byte", 8),
  };
  for (const auto& v : cases) {
    SCOPED_TRACE(v);

    // Build PXF source by quoting the value with the same rules the encoder
    // uses (so the lexer can read it back).
    std::string src = "string_field = ";
    src.push_back('"');
    static constexpr char kHex[] = "0123456789abcdef";
    for (unsigned char c : v) {
      switch (c) {
        case '"': src += "\\\""; break;
        case '\\': src += "\\\\"; break;
        case '\n': src += "\\n"; break;
        case '\r': src += "\\r"; break;
        case '\t': src += "\\t"; break;
        default:
          if (c < 0x20) {
            src += "\\x";
            src.push_back(kHex[c >> 4]);
            src.push_back(kHex[c & 0xF]);
          } else {
            src.push_back(static_cast<char>(c));
          }
      }
    }
    src.push_back('"');
    src.push_back('\n');

    auto doc = Parse(src);
    ASSERT_TRUE(doc.ok()) << doc.status().message();
    const Assignment* a = FindAssignment(*doc, "string_field");
    ASSERT_NE(a, nullptr);
    const StringVal* sv = GetStringVal(a->value);
    ASSERT_NE(sv, nullptr);
    EXPECT_EQ(sv->value, v);

    std::string out = FormatDocument(*doc);

    auto doc2 = Parse(out);
    ASSERT_TRUE(doc2.ok()) << "re-parse failed; output was: " << out;
    const Assignment* a2 = FindAssignment(*doc2, "string_field");
    ASSERT_NE(a2, nullptr);
    const StringVal* sv2 = GetStringVal(a2->value);
    ASSERT_NE(sv2, nullptr);
    EXPECT_EQ(sv2->value, v);
  }
}

TEST(Format, PreservesComments) {
  std::string src = R"(# leading
name = "Alice"
# block comment for nested
nested {
  inner = 42
}
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();

  std::string out = FormatDocument(*doc);
  EXPECT_NE(out.find("# leading"), std::string::npos);
  EXPECT_NE(out.find("# block comment for nested"), std::string::npos);

  // Re-parsing the formatted output should succeed.
  auto doc2 = Parse(out);
  ASSERT_TRUE(doc2.ok()) << "re-parse failed; output was: " << out;
}

TEST(Format, EmitsTypeDirective) {
  std::string src = R"(@type test.v1.Foo

x = 1
)";
  auto doc = Parse(src);
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  EXPECT_EQ(doc->type_url, "test.v1.Foo");

  std::string out = FormatDocument(*doc);
  EXPECT_EQ(out.substr(0, 19), "@type test.v1.Foo\n\n");

  auto doc2 = Parse(out);
  ASSERT_TRUE(doc2.ok());
  EXPECT_EQ(doc2->type_url, "test.v1.Foo");
}

}  // namespace

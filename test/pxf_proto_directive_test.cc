// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Parser tests for the @proto directive (draft §3.4.5).
//
// Four body shapes lexically distinguished: anonymous, named, source,
// descriptor. Plus reserved-directive-name rejection (draft §3.4.6).

#include "protowire/pxf.h"
#include "protowire/pxf/dataset_reader.h"
#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

namespace pp = protowire::pxf;

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

std::string Body(const pp::ProtoDirective& pd) {
  return pd.body;
}

TEST(Proto, AnonymousBody) {
  auto doc = pp::Parse(R"(@proto {
  string symbol = 1;
  double price = 2;
}
)");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->protos.size(), 1u);
  const auto& p = doc->protos[0];
  EXPECT_EQ(p.shape, pp::ProtoShape::kAnonymous);
  EXPECT_EQ(p.type_name, "");
  EXPECT_NE(Body(p).find("string symbol = 1;"), std::string::npos);
  EXPECT_NE(Body(p).find("double price = 2;"), std::string::npos);
}

TEST(Proto, NamedBody) {
  auto doc = pp::Parse(R"(@proto trades.v1.Trade {
  string symbol = 1;
  double price = 2;
}
)");
  ASSERT_TRUE(doc.ok());
  ASSERT_EQ(doc->protos.size(), 1u);
  EXPECT_EQ(doc->protos[0].shape, pp::ProtoShape::kNamed);
  EXPECT_EQ(doc->protos[0].type_name, "trades.v1.Trade");
  EXPECT_NE(Body(doc->protos[0]).find("string symbol = 1;"), std::string::npos);
}

TEST(Proto, SourceBody) {
  auto doc = pp::Parse(R"(@proto """
syntax = "proto3";
package trades.v1;
message Trade { string symbol = 1; }
""")");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->protos.size(), 1u);
  EXPECT_EQ(doc->protos[0].shape, pp::ProtoShape::kSource);
  EXPECT_NE(Body(doc->protos[0]).find("message Trade"), std::string::npos);
}

TEST(Proto, DescriptorBody) {
  // "hello" → "aGVsbG8="
  auto doc = pp::Parse(R"(@proto b"aGVsbG8=")");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->protos.size(), 1u);
  EXPECT_EQ(doc->protos[0].shape, pp::ProtoShape::kDescriptor);
  EXPECT_EQ(Body(doc->protos[0]), "hello");
}

TEST(Proto, Multiple) {
  auto doc = pp::Parse(R"(@proto trades.v1.Trade { string symbol = 1; }
@proto orders.v1.Order { string id = 1; }
)");
  ASSERT_TRUE(doc.ok());
  ASSERT_EQ(doc->protos.size(), 2u);
  EXPECT_EQ(doc->protos[0].type_name, "trades.v1.Trade");
  EXPECT_EQ(doc->protos[1].type_name, "orders.v1.Order");
}

TEST(Proto, AnonymousFollowedByUntypedDataset) {
  // One-shot binding: anonymous @proto types the next untyped @dataset
  // in document order (draft §3.4.4 Anonymous binding).
  auto doc = pp::Parse(R"(@proto {
  string symbol = 1;
  double price = 2;
}
@dataset (symbol, price)
("AAPL", 192.34)
("MSFT", 410.10)
)");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  ASSERT_EQ(doc->protos.size(), 1u);
  EXPECT_EQ(doc->protos[0].shape, pp::ProtoShape::kAnonymous);
  ASSERT_EQ(doc->datasets.size(), 1u);
  EXPECT_EQ(doc->datasets[0].type, "");
  EXPECT_EQ(doc->datasets[0].rows.size(), 2u);
}

TEST(Proto, NestedBracesInBody) {
  // Anonymous @proto with nested `message Side { ... }` must capture
  // the body up to the matching outer `}`.
  auto doc = pp::Parse(R"(@proto {
  message Side {
    string label = 1;
  }
  Side side = 1;
}
)");
  ASSERT_TRUE(doc.ok());
  ASSERT_EQ(doc->protos.size(), 1u);
  const std::string body = Body(doc->protos[0]);
  EXPECT_NE(body.find("message Side"), std::string::npos);
  EXPECT_NE(body.find("Side side = 1;"), std::string::npos);
}

TEST(Proto, RejectsBadShape) {
  auto doc = pp::Parse("@proto 42");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("after @proto"), std::string::npos);
}

TEST(Proto, RejectsNamedMissingBrace) {
  auto doc = pp::Parse("@proto trades.v1.Trade 42");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("'{'"), std::string::npos);
}

TEST(Proto, RejectsAnonymousUnmatchedBrace) {
  auto doc = pp::Parse("@proto { string symbol = 1;");
  ASSERT_FALSE(doc.ok());
  EXPECT_NE(doc.status().message().find("unmatched"), std::string::npos);
}

TEST(Proto, CoexistsWithType) {
  auto doc = pp::Parse(R"(@type some.pkg.Foo
@proto some.pkg.Foo {
  string name = 1;
}
)");
  ASSERT_TRUE(doc.ok()) << doc.status().message();
  EXPECT_EQ(doc->type_url, "some.pkg.Foo");
  ASSERT_EQ(doc->protos.size(), 1u);
  EXPECT_EQ(doc->protos[0].shape, pp::ProtoShape::kNamed);
}

TEST(ReservedDirectives, FutureReservedNamesRejected) {
  // Draft §3.4.6: v1 decoders MUST reject @table / @datasource /
  // @view / @procedure / @function / @permissions as spec-reserved
  // (future-allocated).
  for (const auto& name : {"table", "datasource", "view", "procedure", "function", "permissions"}) {
    std::string input = std::string("@") + name + " { x = 1 }";
    auto doc = pp::Parse(input);
    ASSERT_FALSE(doc.ok()) << "@" << name << " should be rejected";
    EXPECT_NE(doc.status().message().find("spec-reserved"), std::string::npos) << "for @" << name;
  }
}

// ---- ProtoShapeName coverage --------------------------------------------

TEST(ProtoShape, NameLookup) {
  EXPECT_STREQ(pp::ProtoShapeName(pp::ProtoShape::kAnonymous), "anonymous");
  EXPECT_STREQ(pp::ProtoShapeName(pp::ProtoShape::kNamed), "named");
  EXPECT_STREQ(pp::ProtoShapeName(pp::ProtoShape::kSource), "source");
  EXPECT_STREQ(pp::ProtoShapeName(pp::ProtoShape::kDescriptor), "descriptor");
}

// ---- A source body must be triple-quoted (#180) --------------------------
// The grammar's proto_source_body is a triple_string. A simple-quoted body
// could carry `"""` through its escapes, and no formatter can write that
// back. protowire-go refuses it as of trendvidia/protowire-go#311, with this
// text. Both string forms lex to kString, so the check reads the input.

constexpr const char* kNotTriple =
    R"(1:8: @proto source body must be a triple-quoted string ("""..."""))";

const std::vector<std::string>& SimpleQuotedSources() {
  static const std::vector<std::string> v = {
      R"(@proto "syntax = \"proto3\";")",
      R"(@proto "syntax = \"proto3\"; // a \"\"\" b")",  // protowire-go#305's body
      R"(@proto "")",
  };
  return v;
}

TEST(Proto, SourceMustBeTripleQuoted) {
  for (const auto& in : SimpleQuotedSources()) {
    auto doc = pp::Parse(in);
    ASSERT_FALSE(doc.ok()) << in;
    EXPECT_EQ(doc.status().ToString(), kNotTriple) << in;

    // A dataset stream's header is read by Parse.
    std::istringstream stream(in + "\n@dataset T (a)\n(1)\n");
    auto tr = pp::DatasetReader::Create(&stream);
    ASSERT_FALSE(tr.ok()) << in;
    EXPECT_NE(std::string(tr.status().message())
                  .find("@proto source body must be a triple-quoted string"),
              std::string::npos)
        << tr.status().message();
  }
}

// The position is the string's, wherever the directive is.
TEST(Proto, SourceMustBeTripleQuotedAfterType) {
  auto doc = pp::Parse("@type a.v1.M\n@proto \"x\"\n");
  ASSERT_FALSE(doc.ok());
  EXPECT_EQ(doc.status().ToString(),
            R"(2:8: @proto source body must be a triple-quoted string ("""..."""))");
}

// The triple-quoted form still reads, on the directive's line or the next,
// and so does the descriptor form, b"…", which is a bytes token, not a string
// (draft: proto-descr-body = 1*WSP bytes).
TEST(Proto, TripleQuotedAndDescriptorStillAccepted) {
  const std::string triple_same = "@proto \"\"\"syntax = \"proto3\";\"\"\"\n";
  const std::string triple_next = "@proto\n\"\"\"syntax = \"proto3\";\"\"\"\n";
  const std::string triple_ml = "@proto \"\"\"\nsyntax = \"proto3\";\n\"\"\"\n";
  for (const auto& in : {triple_same, triple_next, triple_ml}) {
    auto doc = pp::Parse(in);
    ASSERT_TRUE(doc.ok()) << in << doc.status().message();
    ASSERT_EQ(doc->protos.size(), 1u);
    EXPECT_EQ(doc->protos[0].shape, pp::ProtoShape::kSource);
    EXPECT_EQ(Body(doc->protos[0]).substr(0, 18), R"(syntax = "proto3";)") << in;

    std::istringstream stream(in + "@dataset T (a)\n(1)\n");
    auto tr = pp::DatasetReader::Create(&stream);
    ASSERT_TRUE(tr.ok()) << in << tr.status().message();
  }
  for (const std::string in : {"@proto b\"\"\n", "@proto b\"aGVsbG8=\"\n"}) {
    auto doc = pp::Parse(in);
    ASSERT_TRUE(doc.ok()) << in << doc.status().message();
    ASSERT_EQ(doc->protos.size(), 1u);
    EXPECT_EQ(doc->protos[0].shape, pp::ProtoShape::kDescriptor) << in;

    std::istringstream stream(in + "@dataset T (a)\n(1)\n");
    auto tr = pp::DatasetReader::Create(&stream);
    ASSERT_TRUE(tr.ok()) << in << tr.status().message();
  }
}

// ParseTolerant reports the simple-quoted body once and parses on: the string
// is consumed, so it is not read as an entry's key.
TEST(Proto, TolerantSourceNotTripleQuoted) {
  auto r = pp::ParseTolerant("@proto \"syntax\"\n@type a.v1.M\n\nx = 1\n");
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_EQ(r.errors[0].ToString(),
            R"(1:8: @proto source body must be a triple-quoted string ("""..."""))");
  EXPECT_TRUE(r.document.protos.empty());
  EXPECT_EQ(r.document.type_url, "a.v1.M");
  EXPECT_EQ(r.document.entries.size(), 1u);
}

// ---- Fast-path coverage (decode_fast.cc) --------------------------------
// Mirrors the AST tests above but routes through Unmarshal / UnmarshalFull
// — covers consumeProtoDirective + capture_brace_body on the fast path,
// plus Result::AddProto / Result::Protos() accessors.

class ProtoFast : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", TESTDATA_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("test.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
    desc_ = file_->FindMessageTypeByName("AllTypes");
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

TEST_F(ProtoFast, AnonymousBody) {
  auto msg = NewAllTypes();
  auto rr = pp::UnmarshalFull(R"(@proto {
  string symbol = 1;
}
string_field = "hi"
)",
                              msg.get());
  ASSERT_TRUE(rr.ok()) << rr.status().message();
  ASSERT_EQ(rr->Protos().size(), 1u);
  EXPECT_EQ(rr->Protos()[0].shape, pp::ProtoShape::kAnonymous);
  EXPECT_NE(rr->Protos()[0].body.find("string symbol = 1;"), std::string::npos);
}

TEST_F(ProtoFast, NamedBody) {
  auto msg = NewAllTypes();
  auto rr = pp::UnmarshalFull(R"(@proto trades.v1.Trade {
  string symbol = 1;
}
string_field = "hi"
)",
                              msg.get());
  ASSERT_TRUE(rr.ok()) << rr.status().message();
  ASSERT_EQ(rr->Protos().size(), 1u);
  EXPECT_EQ(rr->Protos()[0].shape, pp::ProtoShape::kNamed);
  EXPECT_EQ(rr->Protos()[0].type_name, "trades.v1.Trade");
}

TEST_F(ProtoFast, SourceBody) {
  auto msg = NewAllTypes();
  auto rr = pp::UnmarshalFull(R"(@proto """
syntax = "proto3";
message Trade { string symbol = 1; }
"""
string_field = "hi"
)",
                              msg.get());
  ASSERT_TRUE(rr.ok()) << rr.status().message();
  ASSERT_EQ(rr->Protos().size(), 1u);
  EXPECT_EQ(rr->Protos()[0].shape, pp::ProtoShape::kSource);
}

TEST_F(ProtoFast, DescriptorBody) {
  auto msg = NewAllTypes();
  // "hello" → "aGVsbG8="
  auto rr = pp::UnmarshalFull(R"(@proto b"aGVsbG8="
string_field = "hi"
)",
                              msg.get());
  ASSERT_TRUE(rr.ok()) << rr.status().message();
  ASSERT_EQ(rr->Protos().size(), 1u);
  EXPECT_EQ(rr->Protos()[0].shape, pp::ProtoShape::kDescriptor);
  EXPECT_EQ(rr->Protos()[0].body, "hello");
}

TEST_F(ProtoFast, NestedBracesInBody) {
  auto msg = NewAllTypes();
  auto rr = pp::UnmarshalFull(R"(@proto {
  message Side {
    string label = 1;
  }
  Side side = 1;
}
string_field = "hi"
)",
                              msg.get());
  ASSERT_TRUE(rr.ok()) << rr.status().message();
  ASSERT_EQ(rr->Protos().size(), 1u);
  EXPECT_NE(rr->Protos()[0].body.find("message Side"), std::string::npos);
}

TEST_F(ProtoFast, MultipleProtos) {
  auto msg = NewAllTypes();
  auto rr = pp::UnmarshalFull(R"(@proto trades.v1.Trade { string symbol = 1; }
@proto orders.v1.Order { string id = 1; }
string_field = "hi"
)",
                              msg.get());
  ASSERT_TRUE(rr.ok()) << rr.status().message();
  EXPECT_EQ(rr->Protos().size(), 2u);
}

TEST_F(ProtoFast, RejectsBadShape) {
  auto msg = NewAllTypes();
  auto st = pp::Unmarshal("@proto 42\nstring_field = \"hi\"\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("@proto"), std::string::npos);
}

// Unmarshal names the token's kind alone, as Parse and protowire-go do.
TEST_F(ProtoFast, RejectsBadShapeAsParseDoes) {
  for (const auto& [in, got] : std::vector<std::pair<std::string, std::string>>{
           {"@proto 42\n", "integer"},
           {"@proto [\n", "["},
           {"@proto 'x'\n", "ILLEGAL"},
       }) {
    const std::string want =
        "1:8: expected '{', dotted identifier, triple-quoted string, or b\"...\" after @proto, "
        "got " +
        got;
    auto msg = NewAllTypes();
    auto st = pp::Unmarshal(in, msg.get());
    ASSERT_FALSE(st.ok()) << in;
    EXPECT_EQ(st.ToString(), want) << in;
    auto doc = pp::Parse(in);
    ASSERT_FALSE(doc.ok()) << in;
    EXPECT_EQ(doc.status().ToString(), want) << in;
  }
}

TEST_F(ProtoFast, RejectsNamedMissingBrace) {
  auto msg = NewAllTypes();
  auto st = pp::Unmarshal("@proto trades.v1.Trade 42\nstring_field = \"hi\"\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("'{'"), std::string::npos);
}

TEST_F(ProtoFast, RejectsAnonymousUnmatchedBrace) {
  auto msg = NewAllTypes();
  auto st = pp::Unmarshal("@proto { string symbol = 1;\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("unmatched"), std::string::npos);
}

// Unmarshal reads directives on a path of its own (#180).
TEST_F(ProtoFast, SourceMustBeTripleQuoted) {
  for (const auto& in : SimpleQuotedSources()) {
    auto msg = NewAllTypes();
    auto st = pp::Unmarshal(in + "\nstring_field = \"hi\"\n", msg.get());
    ASSERT_FALSE(st.ok()) << in;
    EXPECT_EQ(st.ToString(), kNotTriple) << in;
  }
}

TEST_F(ProtoFast, TripleQuotedAndDescriptorStillAccepted) {
  for (const std::string in : {
           "@proto \"\"\"syntax = \"proto3\";\"\"\"\n",
           "@proto\n\"\"\"syntax = \"proto3\";\"\"\"\n",
           "@proto \"\"\"\nsyntax = \"proto3\";\n\"\"\"\n",
           "@proto b\"\"\n",
       }) {
    auto msg = NewAllTypes();
    auto rr = pp::UnmarshalFull(in + "string_field = \"hi\"\n", msg.get());
    ASSERT_TRUE(rr.ok()) << in << rr.status().message();
    ASSERT_EQ(rr->Protos().size(), 1u) << in;
  }
}

TEST_F(ProtoFast, ReservedDirectiveRejected) {
  // Draft §3.4.6 enforcement on the fast path.
  auto msg = NewAllTypes();
  auto st = pp::Unmarshal("@table { x = 1 }\nstring_field = \"hi\"\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("spec-reserved"), std::string::npos);
}

}  // namespace

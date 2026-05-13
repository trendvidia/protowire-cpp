// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Parser tests for the @proto directive (draft §3.4.5).
//
// Four body shapes lexically distinguished: anonymous, named, source,
// descriptor. Plus reserved-directive-name rejection (draft §3.4.6).

#include "protowire/pxf.h"
#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <memory>
#include <string>

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

TEST_F(ProtoFast, ReservedDirectiveRejected) {
  // Draft §3.4.6 enforcement on the fast path.
  auto msg = NewAllTypes();
  auto st = pp::Unmarshal("@table { x = 1 }\nstring_field = \"hi\"\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("spec-reserved"), std::string::npos);
}

}  // namespace

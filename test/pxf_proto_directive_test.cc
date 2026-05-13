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

#include <string>

namespace pp = protowire::pxf;

namespace {

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
    EXPECT_NE(doc.status().message().find("spec-reserved"), std::string::npos)
        << "for @" << name;
  }
}

}  // namespace

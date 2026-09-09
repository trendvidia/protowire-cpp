// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// HARDENING.md § Mandatory limits on the PXF parser and decoder (#26, #25):
// each limit rejects input the default accepts once lowered, the depth cap
// accepts exactly kMaxNestingDepth descents and rejects one more (both
// kinds of nesting, in Parse and in Unmarshal alike), the default
// MaxMessageSize rejects 64 MiB + 1 byte, and a proto3 string field
// refuses invalid UTF-8 while a bytes field takes it.

#include "protowire/limits.h"
#include "protowire/pxf.h"
#include "protowire/pxf/lexer.h"
#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <memory>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;
using protowire::kMaxMessageSize;
using protowire::kMaxNestingDepth;
using protowire::pxf::ParseOptions;
using protowire::pxf::UnmarshalOptions;

class CollectErrors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

constexpr const char* kProto = R"(
syntax = "proto3";
package hardening.v1;
import "pxf/bignum.proto";
message Tree {
  Tree child = 1;
  string label = 2;
  repeated Tree children = 3;
}
message Holder {
  string text = 1;
  bytes raw = 2;
  repeated int32 values = 3;
  map<string, string> labels = 4;
  pxf.BigInt big = 5;
  repeated string texts = 6;
}
)";

class Hardening : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", PROTO_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    source_tree_.MapPath("", testing::TempDir());
    std::string path = std::string(testing::TempDir()) + "/hardening.proto";
    FILE* f = std::fopen(path.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fwrite(kProto, 1, std::strlen(kProto), f);
    std::fclose(f);
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    const pb::FileDescriptor* file = importer_->Import("hardening.proto");
    ASSERT_NE(file, nullptr) << errors_.last_;
    tree_ = importer_->pool()->FindMessageTypeByName("hardening.v1.Tree");
    holder_ = importer_->pool()->FindMessageTypeByName("hardening.v1.Holder");
    ASSERT_NE(tree_, nullptr);
    ASSERT_NE(holder_, nullptr);
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
  }

  protowire::Status Decode(const pb::Descriptor* d,
                           const std::string& text,
                           UnmarshalOptions opts = {}) {
    std::unique_ptr<pb::Message> msg(factory_->GetPrototype(d)->New());
    return protowire::pxf::Unmarshal(text, msg.get(), opts);
  }

  pb::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::Descriptor* tree_ = nullptr;
  const pb::Descriptor* holder_ = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
};

// n nested `child { ... }` blocks: n descents from the root.
std::string NestedBlocks(int n) {
  std::string s;
  for (int i = 0; i < n; ++i) s += "child {\n";
  s += "label = \"leaf\"\n";
  for (int i = 0; i < n; ++i) s += "}\n";
  return s;
}

// n/2 pairs of `children = [ { ... } ]`: a list and a block each, n
// descents of both kinds; plus one `child { }` at the deepest point when
// `plus_one`.
std::string NestedLists(int n, bool plus_one) {
  std::string s;
  for (int i = 0; i < n / 2; ++i) s += "children = [ {\n";
  if (plus_one) s += "child { label = \"leaf\" }\n";
  s += "label = \"leaf\"\n";
  for (int i = 0; i < n / 2; ++i) s += "} ]\n";
  return s;
}

TEST_F(Hardening, DepthBoundBothSidesInParseAndDecode) {
  for (const auto& [name, at, over] :
       {std::tuple{"blocks", NestedBlocks(kMaxNestingDepth), NestedBlocks(kMaxNestingDepth + 1)},
        std::tuple{
            "lists", NestedLists(kMaxNestingDepth, false), NestedLists(kMaxNestingDepth, true)}}) {
    SCOPED_TRACE(name);
    EXPECT_TRUE(protowire::pxf::Parse(at).ok());
    EXPECT_TRUE(Decode(tree_, at).ok());
    auto parsed = protowire::pxf::Parse(over);
    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("MaxNestingDepth=100"), std::string::npos)
        << parsed.status().ToString();
    auto decoded = Decode(tree_, over);
    ASSERT_FALSE(decoded.ok());
    EXPECT_NE(decoded.message().find("MaxNestingDepth=100"), std::string::npos)
        << decoded.ToString();
  }
}

TEST_F(Hardening, DepthLoweredPerCall) {
  UnmarshalOptions opts;
  opts.max_nesting_depth = 3;
  EXPECT_TRUE(Decode(tree_, NestedBlocks(3), opts).ok());
  EXPECT_FALSE(Decode(tree_, NestedBlocks(4), opts).ok());
  ParseOptions popts;
  popts.max_nesting_depth = 3;
  EXPECT_TRUE(protowire::pxf::Parse(NestedBlocks(3), popts).ok());
  EXPECT_FALSE(protowire::pxf::Parse(NestedBlocks(4), popts).ok());
}

// 100k levels used to overflow the native stack before any cap tripped.
TEST_F(Hardening, DeepNestingRejectsCleanly) {
  auto st = Decode(tree_, NestedBlocks(100000));
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("MaxNestingDepth"), std::string::npos);
}

TEST_F(Hardening, MessageSizeLoweredAndDefault) {
  std::string doc = "label = \"" + std::string(2000, 'x') + "\"\n";
  UnmarshalOptions opts;
  opts.max_message_size = 1024;
  auto st = Decode(tree_, doc, opts);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("exceeds MaxMessageSize=1024"), std::string::npos) << st.ToString();
  opts.max_message_size = 4096;
  EXPECT_TRUE(Decode(tree_, doc, opts).ok());
  ParseOptions popts;
  popts.max_message_size = 1024;
  EXPECT_FALSE(protowire::pxf::Parse(doc, popts).ok());

  // The default rejects 64 MiB + 1 byte before reading a token: the
  // document is a comment, so nothing else could be wrong with it.
  std::string huge(static_cast<size_t>(kMaxMessageSize) + 1, '#');
  st = Decode(tree_, huge);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("exceeds MaxMessageSize=67108864"), std::string::npos)
      << st.ToString();
  EXPECT_FALSE(protowire::pxf::Parse(huge).ok());
}

TEST_F(Hardening, BytesLiteralLengthBound) {
  // 256 characters of base64 decode to 192 bytes.
  std::string doc = "raw = b\"" + std::string(256, 'A') + "\"\n";
  UnmarshalOptions opts;
  opts.max_bytes_literal_length = 128;
  auto st = Decode(holder_, doc, opts);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("MaxBytesLiteralLength=128"), std::string::npos) << st.ToString();
  opts.max_bytes_literal_length = 256;
  EXPECT_TRUE(Decode(holder_, doc, opts).ok());
  EXPECT_TRUE(Decode(holder_, doc).ok());
  ParseOptions popts;
  popts.max_bytes_literal_length = 128;
  EXPECT_FALSE(protowire::pxf::Parse(doc, popts).ok());
  EXPECT_TRUE(protowire::pxf::Parse(doc).ok());
}

TEST_F(Hardening, RepeatedCountBound) {
  std::string list = "values = [";
  for (int i = 0; i < 16; ++i) list += (i ? ", " : "") + std::to_string(i);
  list += "]\n";
  UnmarshalOptions opts;
  opts.max_repeated_count = 8;
  auto st = Decode(holder_, list, opts);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("repeated field \"values\" exceeds MaxRepeatedCount=8"),
            std::string::npos)
      << st.ToString();
  opts.max_repeated_count = 16;
  EXPECT_TRUE(Decode(holder_, list, opts).ok());
  // Concatenation across two bindings counts toward the same bound.
  opts.max_repeated_count = 16;
  EXPECT_FALSE(Decode(holder_, list + list, opts).ok());

  std::string map = "labels = {\n";
  for (int i = 0; i < 16; ++i) map += "k" + std::to_string(i) + ": \"v\"\n";
  map += "}\n";
  opts.max_repeated_count = 8;
  st = Decode(holder_, map, opts);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("map field \"labels\" exceeds MaxRepeatedCount=8"), std::string::npos)
      << st.ToString();
  opts.max_repeated_count = 16;
  EXPECT_TRUE(Decode(holder_, map, opts).ok());
}

TEST_F(Hardening, NumericLiteralDigitsBound) {
  std::string at = "big = " + std::string(4096, '7') + "\n";
  std::string over = "big = " + std::string(4097, '7') + "\n";
  EXPECT_TRUE(Decode(holder_, at).ok());
  auto st = Decode(holder_, over);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("MaxNumericLiteralDigits=4096"), std::string::npos) << st.ToString();
  UnmarshalOptions opts;
  opts.max_numeric_literal_digits = 8;
  EXPECT_TRUE(Decode(holder_, "big = 12345678\n", opts).ok());
  EXPECT_FALSE(Decode(holder_, "big = 123456789\n", opts).ok());
}

TEST_F(Hardening, InvalidUTF8RejectedOnStringAcceptedOnBytes) {
  // \xFF\xFE escapes, and raw bytes, in a string field: rejected.
  for (const char* doc : {"text = \"\\xFF\\xFE\"\n",
                          "text = \"a\\377b\"\n",
                          "text = \"\xC0\xAF\"\n",
                          "texts = [\"ok\", \"\\xFF\"]\n",
                          "labels = { \"\\xFF\": \"v\" }\n"}) {
    SCOPED_TRACE(doc);
    auto st = Decode(holder_, doc);
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find("invalid UTF-8"), std::string::npos) << st.ToString();
  }
  // The same bytes into a bytes field are fine, and valid multi-byte
  // UTF-8 in a string field is fine.
  EXPECT_TRUE(Decode(holder_, "raw = \"\\xFF\\xFE\"\n").ok());
  EXPECT_TRUE(Decode(holder_, "text = \"caf\\u00e9 \xE6\x97\xA5\"\n").ok());
  // Surrogates and out-of-range escapes are already refused by the lexer.
  EXPECT_FALSE(Decode(holder_, "text = \"\\uD800\"\n").ok());
}

TEST(HardeningLexer, BytesLiteralCapIsJudgedFromLength) {
  const std::string input = "b\"" + std::string(2000, 'A') + "\"";
  protowire::pxf::Lexer lex(input);
  lex.SetMaxBytesLiteral(128);
  auto t = lex.Next();
  EXPECT_EQ(t.kind, protowire::pxf::TokenKind::kIllegal);
  EXPECT_EQ(t.value, "bytes literal decodes to more than MaxBytesLiteralLength=128 bytes");
}

}  // namespace

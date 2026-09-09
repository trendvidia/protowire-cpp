// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/sbe.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;

class CollectErrors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

constexpr const char* kOrderProto = R"(
syntax = "proto3";
package sbe.test;

import "sbe/annotations.proto";

option (sbe.schema_id) = 1;
option (sbe.version) = 0;

message Order {
  option (sbe.template_id) = 1;
  uint64 order_id = 1;
  string symbol   = 2 [(sbe.length) = 8];
  int64  price    = 3;
  uint32 quantity = 4;
  uint32 side     = 5 [(sbe.encoding) = "uint8"];
}
)";

class SbeCodec : public ::testing::Test {
 protected:
  void SetUp() override {
    // Map SBE annotations dir.
    source_tree_.MapPath("", PROTO_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    // Map a virtual file for our schema.
    source_tree_.MapPath("", testing::TempDir());
    std::string schema_path = std::string(testing::TempDir()) + "/order.proto";
    FILE* f = std::fopen(schema_path.c_str(), "w");
    std::fwrite(kOrderProto, 1, std::strlen(kOrderProto), f);
    std::fclose(f);

    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("order.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("sbe.test.Order");
    ASSERT_NE(desc_, nullptr);
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());

    auto codec = protowire::sbe::Codec::New({file_});
    ASSERT_TRUE(codec.ok()) << codec.status().ToString();
    codec_ = std::make_unique<protowire::sbe::Codec>(std::move(codec).consume());
  }

  std::unique_ptr<pb::Message> NewOrder() {
    return std::unique_ptr<pb::Message>(factory_->GetPrototype(desc_)->New());
  }

  pb::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  const pb::Descriptor* desc_ = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
  std::unique_ptr<protowire::sbe::Codec> codec_;
};

TEST_F(SbeCodec, RoundTrip) {
  auto orig = NewOrder();
  const pb::Reflection* r = orig->GetReflection();
  r->SetUInt64(orig.get(), desc_->FindFieldByName("order_id"), 12345);
  r->SetString(orig.get(), desc_->FindFieldByName("symbol"), "AAPL");
  r->SetInt64(orig.get(), desc_->FindFieldByName("price"), 17500);
  r->SetUInt32(orig.get(), desc_->FindFieldByName("quantity"), 100);
  r->SetUInt32(orig.get(), desc_->FindFieldByName("side"), 2);

  auto bytes = codec_->Marshal(*orig);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();

  auto got = NewOrder();
  ASSERT_TRUE(codec_->Unmarshal(*bytes, got.get()).ok());

  EXPECT_EQ(r->GetUInt64(*got, desc_->FindFieldByName("order_id")), 12345u);
  std::string s;
  EXPECT_EQ(r->GetStringReference(*got, desc_->FindFieldByName("symbol"), &s), "AAPL");
  EXPECT_EQ(r->GetInt64(*got, desc_->FindFieldByName("price")), 17500);
  EXPECT_EQ(r->GetUInt32(*got, desc_->FindFieldByName("quantity")), 100u);
  EXPECT_EQ(r->GetUInt32(*got, desc_->FindFieldByName("side")), 2u);
}

// HARDENING.md § UTF-8 covers SBE char[] arrays decoded into a proto3
// string: the bytes are copied from the wire, so an invalid sequence is
// refused at the field rather than stored.
TEST_F(SbeCodec, CharFieldRejectsInvalidUTF8) {
  auto orig = NewOrder();
  orig->GetReflection()->SetString(orig.get(), desc_->FindFieldByName("symbol"), "AAPL");
  auto bytes = codec_->Marshal(*orig);
  ASSERT_TRUE(bytes.ok());
  // symbol occupies 8 bytes after order_id (8 bytes) in the root block.
  (*bytes)[8 + 8] = 0xFF;
  (*bytes)[8 + 9] = 0xFE;
  auto got = NewOrder();
  auto st = codec_->Unmarshal(*bytes, got.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("invalid UTF-8 in string field symbol"), std::string::npos)
      << st.ToString();
}

TEST_F(SbeCodec, ViewReadsZeroAlloc) {
  auto orig = NewOrder();
  const pb::Reflection* r = orig->GetReflection();
  r->SetUInt64(orig.get(), desc_->FindFieldByName("order_id"), 7);
  r->SetString(orig.get(), desc_->FindFieldByName("symbol"), "ETH");
  r->SetInt64(orig.get(), desc_->FindFieldByName("price"), -99);
  r->SetUInt32(orig.get(), desc_->FindFieldByName("side"), 1);

  auto bytes = codec_->Marshal(*orig);
  ASSERT_TRUE(bytes.ok());
  auto view = codec_->NewView(*bytes);
  ASSERT_TRUE(view.ok());

  EXPECT_EQ(view->Uint("order_id"), 7u);
  EXPECT_EQ(view->String("symbol"), "ETH");
  EXPECT_EQ(view->Int("price"), -99);
  EXPECT_EQ(view->Uint("side"), 1u);
}

TEST_F(SbeCodec, EncodingOverrideNarrowsField) {
  // The "side" field is uint32 in proto but (sbe.encoding) = "uint8" — verify
  // it occupies a single byte in the wire layout. With order_id (8) + symbol
  // (8) + price (8) + quantity (4) + side (1) = 29 bytes block + 8 header.
  auto orig = NewOrder();
  auto bytes = codec_->Marshal(*orig);
  ASSERT_TRUE(bytes.ok());
  EXPECT_EQ(bytes->size(), 8u + 29u);
}

}  // namespace

// ---- HARDENING.md § SBE validation (#26, #25) -----------------------------

namespace {

constexpr const char* kGroupProto = R"(
syntax = "proto3";
package sbe.hardening;

import "sbe/annotations.proto";

option (sbe.schema_id) = 9001;
option (sbe.version) = 0;

message WithGroup {
  option (sbe.template_id) = 9001;
  uint32 root_value = 1;
  message Entry {
    uint32 entry_value = 1;
  }
  repeated Entry entries = 2;
}
)";

class SbeHardening : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", PROTO_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    source_tree_.MapPath("", testing::TempDir());
    std::string schema_path = std::string(testing::TempDir()) + "/with_group.proto";
    FILE* f = std::fopen(schema_path.c_str(), "w");
    std::fwrite(kGroupProto, 1, std::strlen(kGroupProto), f);
    std::fclose(f);
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("with_group.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("sbe.hardening.WithGroup");
    ASSERT_NE(desc_, nullptr);
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
  }

  protowire::sbe::Codec NewCodec(protowire::sbe::CodecOptions opts = {}) {
    auto codec = protowire::sbe::Codec::New({file_}, opts);
    EXPECT_TRUE(codec.ok()) << codec.status().ToString();
    return std::move(codec).consume();
  }

  // header(8) + root_block(4) + group_header(4) + count × entry_block(4).
  static std::vector<uint8_t> Wire(uint16_t block_length,
                                   uint16_t entry_block,
                                   uint16_t count,
                                   size_t body_bytes) {
    std::vector<uint8_t> b;
    auto u16 = [&](uint16_t v) {
      b.push_back(static_cast<uint8_t>(v & 0xFF));
      b.push_back(static_cast<uint8_t>(v >> 8));
    };
    u16(block_length);
    u16(9001);
    u16(9001);
    u16(0);
    for (uint16_t i = 0; i < block_length; ++i) b.push_back(0);
    u16(entry_block);
    u16(count);
    for (size_t i = 0; i < body_bytes; ++i) b.push_back(static_cast<uint8_t>(i));
    return b;
  }

  protowire::Status Decode(const protowire::sbe::Codec& codec, const std::vector<uint8_t>& b) {
    std::unique_ptr<pb::Message> msg(factory_->GetPrototype(desc_)->New());
    return codec.Unmarshal(b, msg.get());
  }

  pb::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  const pb::Descriptor* desc_ = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
};

TEST_F(SbeHardening, WellFormedGroupDecodesAndViews) {
  auto codec = NewCodec();
  auto b = Wire(4, 4, 3, 12);
  EXPECT_TRUE(Decode(codec, b).ok());
  auto view = codec.NewView(b);
  ASSERT_TRUE(view.ok()) << view.status().ToString();
  EXPECT_EQ(view->Group("entries").Len(), 3u);
  EXPECT_EQ(view->Group("entries").Entry(2).Uint("entry_value"), 0x0b0a0908u);
  // Out of range reads as zero rather than past the buffer.
  EXPECT_EQ(view->Group("entries").Entry(3).Uint("entry_value"), 0u);
}

TEST_F(SbeHardening, ShortBlockLengthRejected) {
  auto codec = NewCodec();
  auto b = Wire(2, 4, 0, 0);
  auto st = Decode(codec, b);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("wire blockLength 2 < schema blockLength 4"), std::string::npos)
      << st.ToString();
  EXPECT_FALSE(codec.NewView(b).ok());
  // A larger wire block is forward-compatible and accepted.
  EXPECT_TRUE(Decode(codec, Wire(6, 4, 0, 0)).ok());
}

TEST_F(SbeHardening, GroupCountOverflowRejectedBeforeAllocation) {
  auto codec = NewCodec();
  auto b = Wire(4, 0xFFFF, 0xFFFF, 0);
  auto st = Decode(codec, b);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("declares 65535 entries"), std::string::npos) << st.ToString();
  EXPECT_FALSE(codec.NewView(b).ok());
}

TEST_F(SbeHardening, ZeroBlockLengthNonZeroCountRejected) {
  auto codec = NewCodec();
  // The template's entry block is 4, so a wire block of 0 is short as
  // well; a group with no fields has template block 0, and the explicit
  // check covers that case too.
  auto b = Wire(4, 0, 10000, 0);
  auto st = Decode(codec, b);
  ASSERT_FALSE(st.ok());
  EXPECT_FALSE(codec.NewView(b).ok());
}

TEST_F(SbeHardening, GroupEntryBlockShorterThanTemplateRejected) {
  auto codec = NewCodec();
  auto st = Decode(codec, Wire(4, 2, 2, 4));
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("group entries wire blockLength 2 < schema blockLength 4"),
            std::string::npos)
      << st.ToString();
}

TEST_F(SbeHardening, RepeatedCountBound) {
  protowire::sbe::CodecOptions opts;
  opts.max_repeated_count = 8;
  auto codec = NewCodec(opts);
  auto b = Wire(4, 4, 16, 64);
  auto st = Decode(codec, b);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("declares 16 entries, MaxRepeatedCount=8"), std::string::npos)
      << st.ToString();
  EXPECT_FALSE(codec.NewView(b).ok());
  opts.max_repeated_count = 16;
  auto at = NewCodec(opts);
  EXPECT_TRUE(Decode(at, b).ok());
  EXPECT_TRUE(at.NewView(b).ok());
}

TEST_F(SbeHardening, MessageSizeBound) {
  protowire::sbe::CodecOptions opts;
  opts.max_message_size = 32;
  auto codec = NewCodec(opts);
  auto b = Wire(4, 4, 8, 32);  // 48 bytes
  auto st = Decode(codec, b);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("input of 48 bytes exceeds MaxMessageSize=32"), std::string::npos)
      << st.ToString();
  EXPECT_FALSE(codec.NewView(b).ok());
  opts.max_message_size = 64;
  EXPECT_TRUE(Decode(NewCodec(opts), b).ok());
}

TEST_F(SbeHardening, TemplateIdMismatchRejected) {
  auto codec = NewCodec();
  auto b = Wire(4, 4, 0, 0);
  b[2] = 0x02;  // low byte of the template id: 9001 (0x2329) becomes 0x2302
  auto st = Decode(codec, b);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("template ID mismatch"), std::string::npos) << st.ToString();
}

}  // namespace

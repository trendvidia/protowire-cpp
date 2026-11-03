// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/sbe.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
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
    source_tree_.MapPath("", protowire_test::FixtureDir());
    std::string schema_path = protowire_test::FixtureDir() + "/order.proto";
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

TEST_F(SbeCodec, ValidatorRunsOnEveryDecode) {
  class Refuse : public protowire::check::Validator {
   public:
    protowire::StatusOr<protowire::check::Report> Validate(const pb::Message&) override {
      ++calls;
      return protowire::check::Report{{{"price", "test.rule", "too low"}}};
    }
    int calls = 0;
  } refuse;
  protowire::sbe::CodecOptions opts;
  opts.validator = &refuse;
  auto codec = protowire::sbe::Codec::New({file_}, opts);
  ASSERT_TRUE(codec.ok());
  auto bytes = codec->Marshal(*NewOrder());
  ASSERT_TRUE(bytes.ok());
  auto got = NewOrder();
  EXPECT_EQ(codec->Unmarshal(*bytes, got.get()).message(), "check: price: too low (test.rule)");
  EXPECT_EQ(refuse.calls, 1);
  EXPECT_TRUE(codec_->Unmarshal(*bytes, got.get()).ok());  // no validator
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
    source_tree_.MapPath("", protowire_test::FixtureDir());
    std::string schema_path = protowire_test::FixtureDir() + "/with_group.proto";
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

namespace {

// Draft -01 § SBE Annotations (protowire#495): an overlong string is cut at the
// last whole character at or before the limit and NUL-padded, so it still
// decodes; a bytes field is cut at the limit. Mirrors
// TestOverlongStringCutOnCharacterBoundary in protowire-go (#246).
constexpr const char* kCutProto = R"(
syntax = "proto3";
package sbe.cut;

import "sbe/annotations.proto";

option (sbe.schema_id) = 2;
option (sbe.version) = 0;

message CutCode {
  string text = 1 [(sbe.length) = 8];
}

message CutRow {
  string tag = 1 [(sbe.length) = 8];
}

message Cut {
  option (sbe.template_id) = 2;
  string text = 1 [(sbe.length) = 8];
  bytes raw = 2 [(sbe.length) = 8];
  CutCode code = 3;
  repeated CutRow rows = 4;
}
)";

TEST(SbeCut, OverlongStringCutOnCharacterBoundary) {
  pb::compiler::DiskSourceTree tree;
  tree.MapPath("", PROTO_DIR);
  tree.MapPath("", WKT_PROTO_DIR);
  tree.MapPath("", protowire_test::FixtureDir());
  {
    FILE* f = std::fopen((protowire_test::FixtureDir() + "/cut.proto").c_str(), "w");
    std::fwrite(kCutProto, 1, std::strlen(kCutProto), f);
    std::fclose(f);
  }
  CollectErrors errors;
  pb::compiler::Importer importer(&tree, &errors);
  const pb::FileDescriptor* file = importer.Import("cut.proto");
  ASSERT_NE(file, nullptr) << errors.last_;
  const pb::Descriptor* desc = file->FindMessageTypeByName("Cut");
  ASSERT_NE(desc, nullptr);
  pb::DynamicMessageFactory factory(importer.pool());
  auto codec = protowire::sbe::Codec::New({file});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();

  const auto marshal = [&](const std::string& text) {
    std::unique_ptr<pb::Message> msg(factory.GetPrototype(desc)->New());
    const pb::Reflection* r = msg->GetReflection();
    r->SetString(msg.get(), desc->FindFieldByName("text"), text);
    r->SetString(msg.get(), desc->FindFieldByName("raw"), text);
    pb::Message* code = r->MutableMessage(msg.get(), desc->FindFieldByName("code"));
    code->GetReflection()->SetString(code, code->GetDescriptor()->FindFieldByName("text"), text);
    pb::Message* row = r->AddMessage(msg.get(), desc->FindFieldByName("rows"));
    row->GetReflection()->SetString(row, row->GetDescriptor()->FindFieldByName("tag"), text);
    auto frame = codec->Marshal(*msg);
    EXPECT_TRUE(frame.ok()) << frame.status().ToString();
    return frame.ok() ? *frame : std::vector<uint8_t>();
  };
  // Header 8; block: text 0-8, raw 8-16, code.text 16-24; group header 4; the
  // one row's tag 28-36.
  constexpr size_t kBlock = 8;
  const struct {
    const char* name;
    size_t at;
  } sites[] = {
      {"root field", kBlock}, {"composite field", kBlock + 16}, {"group field", kBlock + 28}};
  const struct {
    const char* name;
    std::string in, text;
  } cases[] = {
      {"2-byte character across the limit", "ABCDEFG\xc3\xa9", "ABCDEFG"},
      {"3-byte character across the limit", "ABCDEF\xe2\x82\xac", "ABCDEF"},
      {"3-byte character, one byte over", "ABCDE\xe2\x82\xacx", "ABCDE\xe2\x82\xac"},
      {"4-byte character across the limit", "ABCDE\xf0\x9f\x98\x80", "ABCDE"},
      {"limit between two 4-byte characters",
       "\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80",
       "\xf0\x9f\x98\x80\xf0\x9f\x98\x80"},
      {"cut already on a boundary", "ABCDEFGHI", "ABCDEFGH"},
      {"fits exactly", "ABCDEF\xc3\xa9", "ABCDEF\xc3\xa9"},
  };
  for (const auto& c : cases) {
    const std::vector<uint8_t> frame = marshal(c.in);
    ASSERT_GE(frame.size(), kBlock + 36) << c.name;
    std::vector<uint8_t> want(c.text.begin(), c.text.end());
    want.resize(8, 0);
    for (const auto& s : sites) {
      EXPECT_EQ(std::vector<uint8_t>(frame.begin() + s.at, frame.begin() + s.at + 8), want)
          << c.name << ", " << s.name;
    }
    EXPECT_EQ(std::vector<uint8_t>(frame.begin() + kBlock + 8, frame.begin() + kBlock + 16),
              std::vector<uint8_t>(c.in.begin(), c.in.begin() + 8))
        << c.name << ", bytes field";
    std::unique_ptr<pb::Message> got(factory.GetPrototype(desc)->New());
    if (!codec->Unmarshal(frame, got.get()).ok()) {
      ADD_FAILURE() << c.name << " does not decode";
      continue;
    }
    EXPECT_EQ(got->GetReflection()->GetString(*got, desc->FindFieldByName("text")), c.text)
        << c.name;
  }

  // The issue's example: ABCDEFGé in a string field and in a bytes field.
  const std::vector<uint8_t> frame = marshal("ABCDEFG\xc3\xa9");
  EXPECT_EQ(std::vector<uint8_t>(frame.begin() + kBlock, frame.begin() + kBlock + 8),
            (std::vector<uint8_t>{0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x00}));
  EXPECT_EQ(std::vector<uint8_t>(frame.begin() + kBlock + 8, frame.begin() + kBlock + 16),
            (std::vector<uint8_t>{0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0xC3}));
}

}  // namespace

namespace {

// A narrow signed encoding is sign-extended into a signed field on decode, as
// protowire-go and View read it (#120).
constexpr const char* kSignedProto = R"(
syntax = "proto3";
package sbe.signed;
import "sbe/annotations.proto";
option (sbe.schema_id) = 3;
option (sbe.version) = 0;
message Signed {
  option (sbe.template_id) = 1;
  int32 a8 = 1 [(sbe.encoding) = "int8"];
  int32 a16 = 2 [(sbe.encoding) = "int16"];
  int64 b8 = 3 [(sbe.encoding) = "int8"];
  int64 b16 = 4 [(sbe.encoding) = "int16"];
  int64 b32 = 5 [(sbe.encoding) = "int32"];
  sint32 c16 = 6 [(sbe.encoding) = "int16"];
}
)";

TEST(SbeSigned, NarrowSignedEncodingsKeepTheirSign) {
  pb::compiler::DiskSourceTree tree;
  tree.MapPath("", PROTO_DIR);
  tree.MapPath("", WKT_PROTO_DIR);
  tree.MapPath("", protowire_test::FixtureDir());
  {
    FILE* f = std::fopen((protowire_test::FixtureDir() + "/signed.proto").c_str(), "w");
    std::fwrite(kSignedProto, 1, std::strlen(kSignedProto), f);
    std::fclose(f);
  }
  CollectErrors errors;
  pb::compiler::Importer importer(&tree, &errors);
  const pb::FileDescriptor* file = importer.Import("signed.proto");
  ASSERT_NE(file, nullptr) << errors.last_;
  const pb::Descriptor* desc = file->FindMessageTypeByName("Signed");
  pb::DynamicMessageFactory factory(importer.pool());
  auto codec = protowire::sbe::Codec::New({file});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();

  std::unique_ptr<pb::Message> msg(factory.GetPrototype(desc)->New());
  const pb::Reflection* r = msg->GetReflection();
  r->SetInt32(msg.get(), desc->FindFieldByName("a8"), -100);
  r->SetInt32(msg.get(), desc->FindFieldByName("a16"), -1234);
  r->SetInt64(msg.get(), desc->FindFieldByName("b8"), -1);
  r->SetInt64(msg.get(), desc->FindFieldByName("b16"), -32768);
  r->SetInt64(msg.get(), desc->FindFieldByName("b32"), -2147483648LL);
  r->SetInt32(msg.get(), desc->FindFieldByName("c16"), -7);
  auto data = codec->Marshal(*msg);
  ASSERT_TRUE(data.ok()) << data.status().ToString();

  std::unique_ptr<pb::Message> got(factory.GetPrototype(desc)->New());
  ASSERT_TRUE(codec->Unmarshal(*data, got.get()).ok());
  EXPECT_EQ(r->GetInt32(*got, desc->FindFieldByName("a8")), -100);
  EXPECT_EQ(r->GetInt32(*got, desc->FindFieldByName("a16")), -1234);
  EXPECT_EQ(r->GetInt64(*got, desc->FindFieldByName("b8")), -1);
  EXPECT_EQ(r->GetInt64(*got, desc->FindFieldByName("b16")), -32768);
  EXPECT_EQ(r->GetInt64(*got, desc->FindFieldByName("b32")), -2147483648LL);
  EXPECT_EQ(r->GetInt32(*got, desc->FindFieldByName("c16")), -7);

  // View reads the same values.
  auto view = codec->NewView(*data);
  ASSERT_TRUE(view.ok()) << view.status().ToString();
  EXPECT_EQ(view->Int("a16"), -1234);
  EXPECT_EQ(view->Int("b32"), -2147483648LL);
}

}  // namespace

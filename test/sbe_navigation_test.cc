// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Tests for the View navigation API on top of a richer SBE schema:
//   - Composite() — non-repeated nested message inlined at a fixed offset
//   - Group() / Entry(i) — repeating group iteration
//   - Bytes() — fixed-length bytes field returned without trim
//
// The existing sbe_codec_test.cc covers scalar marshal/unmarshal on the Order
// schema. This file adds a Trade schema that exercises the rest.

#include "protowire/sbe.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

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

constexpr const char* kTradeProto = R"(
syntax = "proto3";
package sbe.test;

import "sbe/annotations.proto";

option (sbe.schema_id) = 2;
option (sbe.version) = 0;

message Price {
  int64 mantissa = 1;
  int32 exponent = 2;
}

message Trade {
  option (sbe.template_id) = 10;

  // Composite: non-repeated nested message → inlined at a fixed offset.
  Price price = 1;

  uint64 qty = 2;

  // Fixed-size bytes field.
  bytes signature = 3 [(sbe.length) = 4];

  // Repeating group.
  message Fill {
    int64  fill_price = 1;
    uint32 fill_qty   = 2;
  }
  repeated Fill fills = 4;
}
)";

class SbeNavigation : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", PROTO_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    source_tree_.MapPath("", testing::TempDir());
    std::string schema_path = std::string(testing::TempDir()) + "/trade.proto";
    FILE* f = std::fopen(schema_path.c_str(), "w");
    std::fwrite(kTradeProto, 1, std::strlen(kTradeProto), f);
    std::fclose(f);

    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("trade.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("sbe.test.Trade");
    ASSERT_NE(desc_, nullptr);
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());

    auto codec = protowire::sbe::Codec::New({file_});
    ASSERT_TRUE(codec.ok()) << codec.status().ToString();
    codec_ = std::make_unique<protowire::sbe::Codec>(std::move(codec).consume());
  }

  std::unique_ptr<pb::Message> NewTrade() {
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

TEST_F(SbeNavigation, ViewComposite) {
  auto trade = NewTrade();
  const pb::Reflection* r = trade->GetReflection();

  // Populate the nested Price composite.
  pb::Message* price_msg = r->MutableMessage(trade.get(), desc_->FindFieldByName("price"));
  const pb::Descriptor* price_desc = price_msg->GetDescriptor();
  price_msg->GetReflection()->SetInt64(price_msg, price_desc->FindFieldByName("mantissa"), 12345);
  price_msg->GetReflection()->SetInt32(price_msg, price_desc->FindFieldByName("exponent"), -2);

  r->SetUInt64(trade.get(), desc_->FindFieldByName("qty"), 100);

  auto bytes = codec_->Marshal(*trade);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  auto view = codec_->NewView(*bytes);
  ASSERT_TRUE(view.ok());

  // Top-level fields readable directly.
  EXPECT_EQ(view->Uint("qty"), 100u);

  // Composite navigation: view.Composite("price") returns a sub-view that
  // reads the nested Price fields at their inlined offsets.
  auto price = view->Composite("price");
  EXPECT_EQ(price.Int("mantissa"), 12345);
  EXPECT_EQ(price.Int("exponent"), -2);
}

TEST_F(SbeNavigation, ViewGroup) {
  auto trade = NewTrade();
  const pb::Reflection* r = trade->GetReflection();

  r->SetUInt64(trade.get(), desc_->FindFieldByName("qty"), 50);

  // Add three Fill entries.
  const pb::FieldDescriptor* fills_fd = desc_->FindFieldByName("fills");
  for (int i = 0; i < 3; ++i) {
    pb::Message* fill = r->AddMessage(trade.get(), fills_fd);
    const pb::Descriptor* fill_desc = fill->GetDescriptor();
    fill->GetReflection()->SetInt64(fill, fill_desc->FindFieldByName("fill_price"), 1000 + i);
    fill->GetReflection()->SetUInt32(fill, fill_desc->FindFieldByName("fill_qty"), 10u + i);
  }

  auto bytes = codec_->Marshal(*trade);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  auto view = codec_->NewView(*bytes);
  ASSERT_TRUE(view.ok());

  auto group = view->Group("fills");
  ASSERT_EQ(group.Len(), 3u);
  for (int i = 0; i < 3; ++i) {
    auto entry = group.Entry(i);
    EXPECT_EQ(entry.Int("fill_price"), 1000 + i);
    EXPECT_EQ(entry.Uint("fill_qty"), 10u + static_cast<uint64_t>(i));
  }
}

TEST_F(SbeNavigation, ViewGroupEmpty) {
  auto trade = NewTrade();
  auto bytes = codec_->Marshal(*trade);
  ASSERT_TRUE(bytes.ok());
  auto view = codec_->NewView(*bytes);
  ASSERT_TRUE(view.ok());

  EXPECT_EQ(view->Group("fills").Len(), 0u);
}

TEST_F(SbeNavigation, ViewBytes) {
  auto trade = NewTrade();
  const pb::Reflection* r = trade->GetReflection();
  // Set 4-byte signature (matches (sbe.length) = 4).
  std::string sig = std::string("\xDE\xAD\xBE\xEF", 4);
  r->SetString(trade.get(), desc_->FindFieldByName("signature"), sig);

  auto bytes = codec_->Marshal(*trade);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  auto view = codec_->NewView(*bytes);
  ASSERT_TRUE(view.ok());

  auto raw = view->Bytes("signature");
  ASSERT_EQ(raw.size(), 4u);
  EXPECT_EQ(raw[0], 0xDE);
  EXPECT_EQ(raw[1], 0xAD);
  EXPECT_EQ(raw[2], 0xBE);
  EXPECT_EQ(raw[3], 0xEF);

  // Bytes does NOT trim trailing zeros — set a shorter value and verify
  // the full 4-byte slice includes the zero padding.
  auto t2 = NewTrade();
  t2->GetReflection()->SetString(
      t2.get(), desc_->FindFieldByName("signature"), std::string("\xAB\xCD", 2));
  auto bytes2 = codec_->Marshal(*t2);
  ASSERT_TRUE(bytes2.ok());
  auto v2 = codec_->NewView(*bytes2);
  ASSERT_TRUE(v2.ok());
  auto raw2 = v2->Bytes("signature");
  ASSERT_EQ(raw2.size(), 4u);
  EXPECT_EQ(raw2[0], 0xAB);
  EXPECT_EQ(raw2[1], 0xCD);
  EXPECT_EQ(raw2[2], 0x00);
  EXPECT_EQ(raw2[3], 0x00);
}

TEST_F(SbeNavigation, MarshalUnmarshalRoundTrip) {
  // Sanity: build a Trade with composite + bytes + group, marshal,
  // unmarshal, and verify the message comes back intact.
  auto orig = NewTrade();
  const pb::Reflection* r = orig->GetReflection();

  pb::Message* price = r->MutableMessage(orig.get(), desc_->FindFieldByName("price"));
  price->GetReflection()->SetInt64(price, price->GetDescriptor()->FindFieldByName("mantissa"), 999);
  price->GetReflection()->SetInt32(price, price->GetDescriptor()->FindFieldByName("exponent"), 1);

  r->SetUInt64(orig.get(), desc_->FindFieldByName("qty"), 7);
  r->SetString(orig.get(), desc_->FindFieldByName("signature"), std::string("\x01\x02\x03\x04", 4));

  const pb::FieldDescriptor* fills_fd = desc_->FindFieldByName("fills");
  pb::Message* fill = r->AddMessage(orig.get(), fills_fd);
  fill->GetReflection()->SetInt64(fill, fill->GetDescriptor()->FindFieldByName("fill_price"), 500);
  fill->GetReflection()->SetUInt32(fill, fill->GetDescriptor()->FindFieldByName("fill_qty"), 2);

  auto bytes = codec_->Marshal(*orig);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();

  auto got = NewTrade();
  ASSERT_TRUE(codec_->Unmarshal(*bytes, got.get()).ok());

  EXPECT_EQ(r->GetUInt64(*got, desc_->FindFieldByName("qty")), 7u);
  std::string sig;
  EXPECT_EQ(r->GetStringReference(*got, desc_->FindFieldByName("signature"), &sig),
            std::string("\x01\x02\x03\x04", 4));
  const pb::Message& gprice = r->GetMessage(*got, desc_->FindFieldByName("price"));
  EXPECT_EQ(
      gprice.GetReflection()->GetInt64(gprice, gprice.GetDescriptor()->FindFieldByName("mantissa")),
      999);
  EXPECT_EQ(r->FieldSize(*got, fills_fd), 1);
}

}  // namespace

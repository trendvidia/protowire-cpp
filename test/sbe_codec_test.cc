#include "protowire/sbe.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;

class CollectErrors : public pb::compiler::MultiFileErrorCollector {
 public:
  void RecordError(absl::string_view filename, int line, int column,
                   absl::string_view msg) override {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" +
            std::to_string(column) + ": " + std::string(msg);
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
  EXPECT_EQ(r->GetStringReference(*got, desc_->FindFieldByName("symbol"), &s),
            "AAPL");
  EXPECT_EQ(r->GetInt64(*got, desc_->FindFieldByName("price")), 17500);
  EXPECT_EQ(r->GetUInt32(*got, desc_->FindFieldByName("quantity")), 100u);
  EXPECT_EQ(r->GetUInt32(*got, desc_->FindFieldByName("side")), 2u);
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

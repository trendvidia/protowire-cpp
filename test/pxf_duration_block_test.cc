// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// A Duration longer than a literal holds, 64 bits of nanoseconds or about 292
// years, is written as a block, which keeps its value exactly. Mirrors
// duration_block_test.go in protowire-go (protowire-go#226), whose values and
// text come from protowire-java's DurationReadBackTest.

#include "protowire/pxf.h"
#include "protowire/pxf/dataset_reader.h"
#include "protowire/pxf/dataset_writer.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/message_differencer.h>

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;

constexpr const char* kSchema = R"src(
syntax = "proto3";
package durblock;
import "google/protobuf/duration.proto";
message Holder {
  google.protobuf.Duration dur_field = 1;
  repeated google.protobuf.Duration list = 2;
  map<string, google.protobuf.Duration> by_name = 3;
}
)src";

class Errors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
           ": " + std::string(msg);
  }
  std::string last;
};

class PxfDurationBlock : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream(protowire_test::FixtureDir() + "/durblock.proto", std::ios::binary) << kSchema;
    tree_.MapPath("", protowire_test::FixtureDir());
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    const pb::FileDescriptor* file = importer_->Import("durblock.proto");
    ASSERT_NE(file, nullptr) << errors_.last;
    desc_ = file->FindMessageTypeByName("Holder");
  }

  std::unique_ptr<pb::Message> New() {
    return std::unique_ptr<pb::Message>(factory_.GetPrototype(desc_)->New());
  }

  static void SetDuration(pb::Message* d, int64_t seconds, int32_t nanos) {
    const pb::Descriptor* dd = d->GetDescriptor();
    d->GetReflection()->SetInt64(d, dd->FindFieldByName("seconds"), seconds);
    d->GetReflection()->SetInt32(d, dd->FindFieldByName("nanos"), nanos);
  }

  pb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::Descriptor* desc_ = nullptr;
  pb::DynamicMessageFactory factory_;
};

TEST_F(PxfDurationBlock, AroundTheLiteralLimit) {
  const struct {
    int64_t seconds;
    int32_t nanos;
    const char* text;
  } cases[] = {
      {9223372036, 854775807, "dur_field = 2562047h47m16.854775807s\n"},
      {-9223372036, -854775808, "dur_field = -2562047h47m16.854775808s\n"},
      {9223372036, 854775808, "dur_field {\n  seconds = 9223372036\n  nanos = 854775808\n}\n"},
      {-9223372036, -854775809, "dur_field {\n  seconds = -9223372036\n  nanos = -854775809\n}\n"},
      {9500000000, 0, "dur_field {\n  seconds = 9500000000\n}\n"},
      {-9500000000, -5, "dur_field {\n  seconds = -9500000000\n  nanos = -5\n}\n"},
      {315576000000, 999999999, "dur_field {\n  seconds = 315576000000\n  nanos = 999999999\n}\n"},
  };
  const pb::FieldDescriptor* fd = desc_->FindFieldByName("dur_field");
  for (const auto& c : cases) {
    auto m = New();
    SetDuration(m->GetReflection()->MutableMessage(m.get(), fd), c.seconds, c.nanos);
    auto text = pxf::Marshal(*m);
    ASSERT_TRUE(text.ok()) << c.text << text.status().ToString();
    EXPECT_EQ(*text, c.text);
    auto back = New();
    ASSERT_TRUE(pxf::Unmarshal(*text, back.get()).ok()) << *text;
    const pb::Message& got = back->GetReflection()->GetMessage(*back, fd);
    const pb::Descriptor* gd = got.GetDescriptor();
    EXPECT_EQ(got.GetReflection()->GetInt64(got, gd->FindFieldByName("seconds")), c.seconds);
    EXPECT_EQ(got.GetReflection()->GetInt32(got, gd->FindFieldByName("nanos")), c.nanos);
  }
}

TEST_F(PxfDurationBlock, PastTheLimitInListAndMap) {
  auto m = New();
  const pb::Reflection* r = m->GetReflection();
  const pb::FieldDescriptor* list = desc_->FindFieldByName("list");
  SetDuration(r->AddMessage(m.get(), list), 5400, 0);
  SetDuration(r->AddMessage(m.get(), list), 9500000000, 1);
  const pb::FieldDescriptor* by_name = desc_->FindFieldByName("by_name");
  for (const auto& [key, seconds, nanos] :
       {std::tuple<const char*, int64_t, int32_t>{"big", 9500000000, 1}, {"small", 5400, 0}}) {
    pb::Message* entry = r->AddMessage(m.get(), by_name);
    const pb::Descriptor* ed = entry->GetDescriptor();
    entry->GetReflection()->SetString(entry, ed->map_key(), key);
    SetDuration(entry->GetReflection()->MutableMessage(entry, ed->map_value()), seconds, nanos);
  }
  auto text = pxf::Marshal(*m);
  ASSERT_TRUE(text.ok()) << text.status().ToString();
  EXPECT_EQ(*text,
            "list = [\n"
            "  1h30m0s,\n"
            "  {\n"
            "    seconds = 9500000000\n"
            "    nanos = 1\n"
            "  }\n"
            "]\n"
            "by_name = {\n"
            "  big: {\n"
            "    seconds = 9500000000\n"
            "    nanos = 1\n"
            "  }\n"
            "  small: 1h30m0s\n"
            "}\n");
  auto back = New();
  ASSERT_TRUE(pxf::Unmarshal(*text, back.get()).ok()) << *text;
  EXPECT_TRUE(pb::util::MessageDifferencer::Equals(*m, *back));
}

TEST_F(PxfDurationBlock, PastTheLimitInADatasetCell) {
  auto m = New();
  SetDuration(m->GetReflection()->MutableMessage(m.get(), desc_->FindFieldByName("dur_field")),
              9500000000,
              1);
  const std::vector<std::string> columns = {"dur_field"};
  std::string table;
  ASSERT_TRUE(pxf::AppendDatasetHeader(&table, desc_, columns).ok());
  ASSERT_TRUE(pxf::AppendDatasetRow(&table, *m, columns).ok());
  std::istringstream in(table);
  auto reader = pxf::DatasetReader::Create(&in);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString() << "\n" << table;
  auto back = New();
  ASSERT_TRUE((*reader)->Scan(back.get()).ok()) << table;
  EXPECT_TRUE(pb::util::MessageDifferencer::Equals(*m, *back)) << table;
}

}  // namespace

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Marshal refuses a message deeper than a decoder at the same limit accepts,
// so nothing it writes is refused for depth when read back (protowire#502).
// Mirrors marshal_depth_test.go in protowire-go (protowire-go#236).

#include "protowire/limits.h"
#include "protowire/pxf.h"
#include "protowire/pxf/dataset_reader.h"
#include "protowire/pxf/dataset_writer.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/message_differencer.h>

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;

constexpr const char* kSchema = R"src(
syntax = "proto3";
package mdepth.v1;

import "google/protobuf/any.proto";
import "google/protobuf/duration.proto";
import "pxf/annotations.proto";

message T {
  T next = 1;
  repeated T kids = 2;
  map<string, T> by_key = 3;
  map<string, int32> scalars = 4;
  repeated int32 ns = 5;
  int32 v = 6;
  repeated T keyed = 7 [(pxf.key) = "id"];
  string id = 8;
  google.protobuf.Any payload = 9;
  google.protobuf.Duration long = 10;
}

message Cell {
  Cell next = 1;
  repeated int32 must = 2 [(pxf.required) = true];
}
)src";

constexpr const char* kRefused = "pxf: nesting depth exceeds MaxNestingDepth=100";

class Errors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
           ": " + std::string(msg);
  }
  std::string last;
};

class Resolver : public pxf::TypeResolver {
 public:
  explicit Resolver(const pb::DescriptorPool* pool) : pool_(pool) {}
  const pb::Descriptor* FindMessageByURL(std::string_view url) override {
    return pool_->FindMessageTypeByName(std::string(url));
  }

 private:
  const pb::DescriptorPool* pool_;
};

using Msg = std::unique_ptr<pb::Message>;

class PxfMarshalDepth : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream(protowire_test::FixtureDir() + "/marshal_depth.proto", std::ios::binary)
        << kSchema;
    tree_.MapPath("", protowire_test::FixtureDir());
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    file_ = importer_->Import("marshal_depth.proto");
    ASSERT_NE(file_, nullptr) << errors_.last;
    t_ = file_->FindMessageTypeByName("T");
    resolver_ = std::make_unique<Resolver>(importer_->pool());
  }

  Msg New(const pb::Descriptor* d = nullptr) {
    return Msg(factory_.GetPrototype(d != nullptr ? d : t_)->New());
  }
  const pb::FieldDescriptor* F(const char* name) const { return t_->FindFieldByName(name); }

  // Wrap puts m in field `field` of a new message of m's type: singular,
  // appended to a list, or as the value of a map entry with key "k".
  Msg Wrap(Msg m, const char* field) {
    const pb::Descriptor* d = m->GetDescriptor();
    Msg p = New(d);
    const pb::FieldDescriptor* fd = d->FindFieldByName(field);
    const pb::Reflection* r = p->GetReflection();
    if (fd->is_map()) {
      pb::Message* entry = r->AddMessage(p.get(), fd);
      const pb::Descriptor* ed = entry->GetDescriptor();
      entry->GetReflection()->SetString(entry, ed->map_key(), "k");
      entry->GetReflection()->SetAllocatedMessage(entry, m.release(), ed->map_value());
    } else if (fd->is_repeated()) {
      r->AddAllocatedMessage(p.get(), fd, m.release());
    } else {
      r->SetAllocatedMessage(p.get(), m.release(), fd);
    }
    return p;
  }

  // Deep wraps m in n singular `next` fields.
  Msg Deep(Msg m, int n) {
    for (int i = 0; i < n; ++i) m = Wrap(std::move(m), "next");
    return m;
  }

  // WithAny returns a T whose payload holds value as a T.
  Msg WithAny(const pb::Message& value) {
    Msg m = New();
    pb::Message* a = m->GetReflection()->MutableMessage(m.get(), F("payload"));
    const pb::Descriptor* ad = a->GetDescriptor();
    a->GetReflection()->SetString(a, ad->FindFieldByName("type_url"), "mdepth.v1.T");
    a->GetReflection()->SetString(a, ad->FindFieldByName("value"), value.SerializeAsString());
    return m;
  }

  static bool Equal(const pb::Message& a, const pb::Message& b) {
    return pb::util::MessageDifferencer::Equals(a, b);
  }

  pb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  const pb::Descriptor* t_ = nullptr;
  pb::DynamicMessageFactory factory_;
  std::unique_ptr<Resolver> resolver_;
};

TEST_F(PxfMarshalDepth, AgreesWithDecoderAndParser) {
  const std::vector<std::pair<const char*, std::function<Msg(Msg)>>> wraps = {
      {"singular", [&](Msg c) { return Wrap(std::move(c), "next"); }},
      {"list", [&](Msg c) { return Wrap(std::move(c), "kids"); }},
      {"map", [&](Msg c) { return Wrap(std::move(c), "by_key"); }},
      {"keyed",
       [&](Msg c) {
         c->GetReflection()->SetString(c.get(), F("id"), "x");
         return Wrap(std::move(c), "keyed");
       }},
  };
  const std::vector<std::pair<const char*, std::function<void(pb::Message*)>>> leaves = {
      {"empty", [](pb::Message*) {}},
      {"scalar", [&](pb::Message* m) { m->GetReflection()->SetInt32(m, F("v"), 1); }},
      {"list", [&](pb::Message* m) { m->GetReflection()->AddInt32(m, F("ns"), 1); }},
      {"scalar map",
       [&](pb::Message* m) {
         pb::Message* e = m->GetReflection()->AddMessage(m, F("scalars"));
         const pb::Descriptor* ed = e->GetDescriptor();
         e->GetReflection()->SetString(e, ed->map_key(), "a");
         e->GetReflection()->SetInt32(e, ed->map_value(), 1);
       }},
      // A Duration past the literal's range is a block, not a literal.
      {"duration block",
       [&](pb::Message* m) {
         pb::Message* d = m->GetReflection()->MutableMessage(m, F("long"));
         d->GetReflection()->SetInt64(
             d, d->GetDescriptor()->FindFieldByName("seconds"), 9500000000);
       }},
  };
  for (const auto& [wn, wrap] : wraps) {
    for (const auto& [ln, leaf] : leaves) {
      int written = 0;
      int refused = 0;
      for (int n = 1; n <= protowire::kMaxNestingDepth + 2; ++n) {
        const std::string where = std::string(wn) + "/" + ln + " " + std::to_string(n);
        Msg m = New();
        leaf(m.get());
        for (int i = 0; i < n; ++i) m = wrap(std::move(m));
        auto out = pxf::Marshal(*m);
        if (!out.ok()) {
          ASSERT_EQ(out.status().message(), kRefused) << where;
          ++refused;
          continue;
        }
        ++written;
        Msg back = New();
        {
          auto st = pxf::Unmarshal(*out, back.get());
          ASSERT_TRUE(st.ok()) << where << ": " << st.ToString() << "\n" << *out;
        }
        ASSERT_TRUE(Equal(*m, *back)) << where;
        ASSERT_TRUE(pxf::Parse(*out).ok()) << where;
      }
      EXPECT_GT(written, 0) << wn << "/" << ln;
      EXPECT_GT(refused, 0) << wn << "/" << ln;
    }
  }
}

// The largest shape that fits is written: refusal is not a level early.
TEST_F(PxfMarshalDepth, Bound) {
  auto out = pxf::Marshal(*Deep(New(), protowire::kMaxNestingDepth));
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  Msg back = New();
  EXPECT_TRUE(pxf::Unmarshal(*out, back.get()).ok());
  auto past = pxf::Marshal(*Deep(New(), protowire::kMaxNestingDepth + 1));
  ASSERT_FALSE(past.ok());
  EXPECT_EQ(past.status().message(), kRefused);
}

TEST_F(PxfMarshalDepth, Option) {
  Msg m = Deep(New(), 150);
  pxf::MarshalOptions mopts;
  mopts.max_nesting_depth = 150;
  auto out = pxf::Marshal(*m, mopts);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  Msg back = New();
  pxf::UnmarshalOptions uopts;
  uopts.max_nesting_depth = 150;
  ASSERT_TRUE(pxf::Unmarshal(*out, back.get(), uopts).ok());
  EXPECT_TRUE(Equal(*m, *back));

  mopts.max_nesting_depth = 149;
  auto past = pxf::Marshal(*m, mopts);
  ASSERT_FALSE(past.ok());
  EXPECT_NE(past.status().message().find("MaxNestingDepth=149"), std::string::npos);
  mopts.max_nesting_depth = 2;
  past = pxf::Marshal(*Deep(New(), 3), mopts);
  ASSERT_FALSE(past.ok());
  EXPECT_NE(past.status().message().find("MaxNestingDepth=2"), std::string::npos);
}

// An Any is not refused for what its bytes hold: when a body would be too
// deep, the Any stays raw, at any limit.
TEST_F(PxfMarshalDepth, AnyTooDeepStaysRaw) {
  Msg m = WithAny(*Deep(New(), 150));
  pxf::MarshalOptions mopts;
  mopts.type_resolver = resolver_.get();
  auto out = pxf::Marshal(*m, mopts);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_EQ(out->find("@type"), std::string::npos);
  pxf::UnmarshalOptions uopts;
  uopts.type_resolver = resolver_.get();
  Msg back = New();
  ASSERT_TRUE(pxf::Unmarshal(*out, back.get(), uopts).ok());
  EXPECT_TRUE(Equal(*m, *back));

  // The body fits under a raised limit, on a reader raised to match.
  mopts.max_nesting_depth = 200;
  out = pxf::Marshal(*m, mopts);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_NE(out->find("@type"), std::string::npos);
  uopts.max_nesting_depth = 200;
  back = New();
  ASSERT_TRUE(pxf::Unmarshal(*out, back.get(), uopts).ok());
  EXPECT_TRUE(Equal(*m, *back));
}

// An Any that sits past the limit itself has no room even for its raw block.
TEST_F(PxfMarshalDepth, AnyPastTheLimitIsRefused) {
  Msg inner = New();
  inner->GetReflection()->SetInt32(inner.get(), F("v"), 1);
  pxf::MarshalOptions mopts;
  mopts.type_resolver = resolver_.get();
  auto out = pxf::Marshal(*Deep(WithAny(*inner), protowire::kMaxNestingDepth), mopts);
  ASSERT_FALSE(out.ok());
  EXPECT_EQ(out.status().message(), kRefused);
}

// A @dataset cell obeys the same limit, as the reader does.
TEST_F(PxfMarshalDepth, DatasetRow) {
  std::string dst;
  ASSERT_TRUE(
      pxf::AppendDatasetRow(&dst, *Deep(New(), protowire::kMaxNestingDepth), {"next"}).ok());
  EXPECT_FALSE(dst.empty());
  dst.clear();
  protowire::Status st =
      pxf::AppendDatasetRow(&dst, *Deep(New(), protowire::kMaxNestingDepth + 1), {"next"});
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.message(), std::string("pxf: @dataset column \"next\": ") + kRefused);
}

// In a cell, an unset required list is written as `[]`, which is a descent of
// its own at the bottom of the row.
TEST_F(PxfMarshalDepth, DatasetRowCountsAnEmptyCollection) {
  const pb::Descriptor* cell = file_->FindMessageTypeByName("Cell");
  const auto row = [&](int n) { return Deep(New(cell), n); };
  const std::vector<std::string> columns = {"next", "must"};
  std::string head;
  ASSERT_TRUE(pxf::AppendDatasetHeader(&head, cell, columns).ok());

  std::string dst = head;
  ASSERT_TRUE(pxf::AppendDatasetRow(&dst, *row(protowire::kMaxNestingDepth - 1), columns).ok());
  std::istringstream in(dst);
  auto tr = pxf::DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok()) << tr.status().ToString() << "\n" << dst;
  Msg got = New(cell);
  ASSERT_TRUE((*tr)->Scan(got.get()).ok()) << dst;

  std::string past;
  protowire::Status st = pxf::AppendDatasetRow(&past, *row(protowire::kMaxNestingDepth), columns);
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.message(), std::string("pxf: @dataset column \"next\": ") + kRefused);
}

}  // namespace

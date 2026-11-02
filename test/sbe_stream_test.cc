// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Ported from protowire-go's encoding/sbe/stream_test.go, with
// Codec::UnmarshalDescriptor.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <streambuf>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

#include "fixture_dir.h"
#include "protoc_compat.h"
#include "protowire/sbe.h"

namespace {

namespace gpb = google::protobuf;
using protowire::IsEOF;
using protowire::IsUnexpectedEOF;
using protowire::Status;
using protowire::sbe::Codec;
using protowire::sbe::IsFramingCorrupt;

constexpr const char* kProto = R"(
syntax = "proto3";
package stream.test;
import "sbe/annotations.proto";
option (sbe.schema_id) = 1;
option (sbe.version) = 0;
message Simple {
  option (sbe.template_id) = 2;
  uint32 id = 1;
  int32 value = 2;
}
message Unregistered {
  uint32 id = 1;
}
message Named {
  option (sbe.template_id) = 3;
  string name = 1 [(sbe.length) = 4];
}
)";

class Errors : public gpb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    ADD_FAILURE() << filename << ":" << line << ":" << column << ": " << msg;
  }
};

class SbeStream : public ::testing::Test {
 protected:
  void SetUp() override {
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    tree_.MapPath("", protowire_test::FixtureDir());
    std::string path = protowire_test::FixtureDir() + "/sbe_stream.proto";
    FILE* f = std::fopen(path.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fwrite(kProto, 1, std::strlen(kProto), f);
    std::fclose(f);
    importer_ = std::make_unique<gpb::compiler::Importer>(&tree_, &errors_);
    const gpb::FileDescriptor* fd = importer_->Import("sbe_stream.proto");
    ASSERT_NE(fd, nullptr);
    simple_ = fd->FindMessageTypeByName("Simple");
    unregistered_ = fd->FindMessageTypeByName("Unregistered");
    auto codec = Codec::New({fd});
    ASSERT_TRUE(codec.ok()) << codec.status().message();
    codec_.emplace(std::move(codec).consume());
    factory_ = std::make_unique<gpb::DynamicMessageFactory>(importer_->pool());
  }

  std::unique_ptr<gpb::Message> New(uint32_t id = 0, int32_t value = 0) {
    std::unique_ptr<gpb::Message> m(factory_->GetPrototype(simple_)->New());
    m->GetReflection()->SetUInt32(m.get(), simple_->FindFieldByName("id"), id);
    m->GetReflection()->SetInt32(m.get(), simple_->FindFieldByName("value"), value);
    return m;
  }
  uint32_t Id(const gpb::Message& m) {
    return m.GetReflection()->GetUInt32(m, simple_->FindFieldByName("id"));
  }
  int32_t Value(const gpb::Message& m) {
    return m.GetReflection()->GetInt32(m, simple_->FindFieldByName("value"));
  }
  static std::string Header(uint32_t total, uint16_t enc) {
    const char h[6] = {static_cast<char>(total >> 24),
                       static_cast<char>(total >> 16),
                       static_cast<char>(total >> 8),
                       static_cast<char>(total),
                       static_cast<char>(enc >> 8),
                       static_cast<char>(enc)};
    return std::string(h, 6);
  }

  gpb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<gpb::compiler::Importer> importer_;
  std::unique_ptr<gpb::DynamicMessageFactory> factory_;
  const gpb::Descriptor* simple_ = nullptr;
  const gpb::Descriptor* unregistered_ = nullptr;
  std::optional<Codec> codec_;
};

TEST_F(SbeStream, RoundTrip) {
  struct Row {
    uint32_t id;
    int32_t value;
  };
  const Row rows[] = {{1, -100}, {2, 0}, {1 << 20, 2147483647}};
  std::stringstream buf;
  auto enc = codec_->NewEncoder(&buf);
  for (const Row& r : rows) ASSERT_TRUE(enc.Encode(*New(r.id, r.value)).ok());

  auto dec = codec_->NewDecoder(&buf);
  for (const Row& r : rows) {
    auto got = New();
    ASSERT_TRUE(dec.Decode(got.get()).ok());
    EXPECT_EQ(Id(*got), r.id);
    EXPECT_EQ(Value(*got), r.value);
  }
  EXPECT_TRUE(IsEOF(dec.Decode(New().get())));
}

TEST_F(SbeStream, CleanEOF) {
  std::stringstream empty;
  auto dec = codec_->NewDecoder(&empty);
  EXPECT_TRUE(IsEOF(dec.Decode(New().get())));
}

TEST_F(SbeStream, PartialHeader) {
  std::stringstream s(std::string(3, '\0'));
  auto dec = codec_->NewDecoder(&s);
  Status st = dec.Decode(New().get());
  EXPECT_TRUE(IsUnexpectedEOF(st)) << st.message();
  EXPECT_EQ(dec.Decode(New().get()).message(), st.message());
}

TEST_F(SbeStream, PartialBody) {
  std::stringstream s(Header(100, 0xEB50) + std::string(10, '\0'));
  auto dec = codec_->NewDecoder(&s);
  EXPECT_TRUE(IsUnexpectedEOF(dec.Decode(New().get())));
}

TEST_F(SbeStream, WrongEncodingType) {
  std::stringstream s(Header(6, 0x5BE0));  // SBE 1.0 big-endian; only LE is accepted
  auto dec = codec_->NewDecoder(&s);
  Status st = dec.Decode(New().get());
  EXPECT_TRUE(IsFramingCorrupt(st)) << st.message();
  EXPECT_EQ(st.message(), "sbe: stream framing corrupt: SOFH encoding type 0x5be0 not supported");
  EXPECT_TRUE(IsFramingCorrupt(dec.Decode(New().get())));
}

TEST_F(SbeStream, LengthBelowHeader) {
  std::stringstream s(Header(4, 0xEB50));
  auto dec = codec_->NewDecoder(&s);
  Status st = dec.Decode(New().get());
  EXPECT_EQ(st.message(), "sbe: stream framing corrupt: SOFH total length 4 < header size");
}

TEST_F(SbeStream, Oversized) {
  std::stringstream s(Header(6 + (1u << 30), 0xEB50));
  auto dec = codec_->NewDecoder(&s);
  dec.SetMaxFrameSize(1 << 16);
  Status st = dec.Decode(New().get());
  EXPECT_EQ(st.message(), "sbe: stream framing corrupt: frame size 1073741824 exceeds max 65536");
}

class FailingBuf : public std::streambuf {
 protected:
  int_type underflow() override { throw std::ios_base::failure("disk gone"); }
};

TEST_F(SbeStream, ReadErrorIsNotEOF) {
  FailingBuf buf;
  std::istream r(&buf);
  auto dec = codec_->NewDecoder(&r);
  EXPECT_TRUE(IsUnexpectedEOF(dec.Decode(New().get())));  // as Go's header read reports it
}

TEST_F(SbeStream, UnmarshalDescriptor) {
  auto bytes = codec_->Marshal(*New(5, -6));
  ASSERT_TRUE(bytes.ok());
  auto got = codec_->UnmarshalDescriptor(*bytes, simple_);
  ASSERT_TRUE(got.ok()) << got.status().message();
  EXPECT_EQ((*got)->GetDescriptor(), simple_);
  EXPECT_EQ(Id(**got), 5u);
  EXPECT_EQ(Value(**got), -6);

  auto missing = codec_->UnmarshalDescriptor(*bytes, unregistered_);
  EXPECT_EQ(missing.status().message(), "sbe: no template registered for stream.test.Unregistered");
  std::vector<uint8_t> shortened(bytes->begin(), bytes->begin() + 3);
  EXPECT_FALSE(codec_->UnmarshalDescriptor(shortened, simple_).ok());
}

TEST_F(SbeStream, UnmarshalDescriptorOutlivesItsCaller) {
  std::shared_ptr<gpb::Message> kept;
  {
    auto bytes = codec_->Marshal(*New(5, -6));
    ASSERT_TRUE(bytes.ok());
    auto got = codec_->UnmarshalDescriptor(*bytes, simple_);
    ASSERT_TRUE(got.ok());
    kept = *got;
  }
  EXPECT_EQ(Id(*kept), 5u);
}

TEST_F(SbeStream, StringDropsOnlyTrailingZeros) {
  const gpb::Descriptor* named = simple_->file()->FindMessageTypeByName("Named");
  const uint8_t wire[] = {4, 0, 3, 0, 1, 0, 0, 0, 'A', 0, 'C', 0};
  auto got = codec_->UnmarshalDescriptor(wire, named);
  ASSERT_TRUE(got.ok()) << got.status().message();
  EXPECT_EQ((*got)->GetReflection()->GetString(**got, named->FindFieldByName("name")),
            std::string("A\0C", 3));
  auto view = codec_->NewView(wire);
  ASSERT_TRUE(view.ok()) << view.status().message();
  EXPECT_EQ(view->String("name"), std::string_view("A\0C", 3));
}

}  // namespace

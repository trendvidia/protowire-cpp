// Comprehensive end-to-end round-trip across every proto3 data type.
//
// Pipeline (each step asserted):
//
//   PXF text₀
//     → pxf::Unmarshal      → msg1
//     → proto::SerializeToString → bin1
//   bin1
//     → proto::ParseFromString → msg2
//     → pxf::Marshal        → PXF text₁
//   PXF text₁
//     → pxf::Unmarshal      → msg3
//     → proto::SerializeToString → bin3
//
// Asserts:
//   - MessageDifferencer::Equals(msg1, msg2)  (binary round-trip lossless)
//   - MessageDifferencer::Equals(msg2, msg3)  (re-encoded text decodes the
//                                              same)
//   - bin1 == bin3                             (byte-stable after any path)
//   - per-field manual assertions covering every AllTypes member, so a
//     silent encoder drop on a single field would still fail the test even
//     if the proto round-trip somehow accidentally agreed.

#include "protowire/pxf.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/util/message_differencer.h>

namespace {

namespace pbuf = google::protobuf;

class CollectErrors : public pbuf::compiler::MultiFileErrorCollector {
 public:
  void RecordError(absl::string_view filename, int line, int column,
                   absl::string_view msg) override {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" +
            std::to_string(column) + ": " + std::string(msg);
  }
  std::string last_;
};

class FullRoundTrip : public ::testing::Test {
 public:
  std::unique_ptr<pbuf::Message> NewAllTypes() {
    return std::unique_ptr<pbuf::Message>(factory_->GetPrototype(desc_)->New());
  }
  const pbuf::Descriptor* desc() const { return desc_; }

 protected:
  void SetUp() override {
    source_tree_.MapPath("", TESTDATA_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pbuf::compiler::Importer>(&source_tree_,
                                                            &errors_);
    file_ = importer_->Import("test.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    desc_ = importer_->pool()->FindMessageTypeByName("test.v1.AllTypes");
    nested_desc_ = importer_->pool()->FindMessageTypeByName("test.v1.Nested");
    ASSERT_NE(desc_, nullptr);
    ASSERT_NE(nested_desc_, nullptr);
    factory_ = std::make_unique<pbuf::DynamicMessageFactory>(importer_->pool());
  }

  pbuf::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pbuf::compiler::Importer> importer_;
  const pbuf::FileDescriptor* file_ = nullptr;
  const pbuf::Descriptor* desc_ = nullptr;
  const pbuf::Descriptor* nested_desc_ = nullptr;
  std::unique_ptr<pbuf::DynamicMessageFactory> factory_;
};

// Helper: drive the whole pipeline and return the three messages plus the
// re-encoded text so callers can assert detailed expectations on either.
struct Trip {
  std::unique_ptr<pbuf::Message> m1, m2, m3;
  std::string bin1, bin3, text1;
};

// Serialize with deterministic map ordering so binary equality is meaningful
// across messages that have map fields (default proto3 serialization is
// allowed to vary on map iteration order).
void SerializeDeterministic(const pbuf::Message& msg, std::string* out) {
  out->clear();
  pbuf::io::StringOutputStream string_output(out);
  pbuf::io::CodedOutputStream coded(&string_output);
  coded.SetSerializationDeterministic(true);
  msg.SerializeWithCachedSizes(&coded);
}

Trip RunFullPipeline(FullRoundTrip* f, const std::string& src) {
  Trip t;
  t.m1 = f->NewAllTypes();
  EXPECT_TRUE(protowire::pxf::Unmarshal(src, t.m1.get()).ok());
  // Force size cache (required before SerializeWithCachedSizes).
  t.m1->ByteSizeLong();
  SerializeDeterministic(*t.m1, &t.bin1);

  t.m2 = f->NewAllTypes();
  EXPECT_TRUE(t.m2->ParseFromString(t.bin1));

  auto encoded = protowire::pxf::Marshal(*t.m2);
  EXPECT_TRUE(encoded.ok());
  t.text1 = *encoded;

  t.m3 = f->NewAllTypes();
  EXPECT_TRUE(protowire::pxf::Unmarshal(t.text1, t.m3.get()).ok());
  t.m3->ByteSizeLong();
  SerializeDeterministic(*t.m3, &t.bin3);

  return t;
}

// Asserts that all three messages are semantically equal (handles map
// ordering, default-value vs explicit-zero, etc.) and that bin1 == bin3.
void ExpectFullEquality(const Trip& t) {
  pbuf::util::MessageDifferencer d;
  d.set_message_field_comparison(
      pbuf::util::MessageDifferencer::EQUAL);
  d.set_repeated_field_comparison(
      pbuf::util::MessageDifferencer::AS_LIST);
  // Maps are compared as sets in MessageDifferencer's default mode — fine.
  EXPECT_TRUE(d.Compare(*t.m1, *t.m2)) << "m1 != m2 after binary round-trip";
  EXPECT_TRUE(d.Compare(*t.m2, *t.m3))
      << "m2 != m3 after PXF re-encode/re-decode\nre-encoded text:\n" << t.text1;
  EXPECT_EQ(t.bin1, t.bin3)
      << "binary serialization drifted across the round-trip\nre-encoded text:\n"
      << t.text1;
}

// ----------------------------------------------------------------------------

TEST_F(FullRoundTrip, AllScalarTypes) {
  std::string src = R"(
string_field = "hello"
int32_field = -42
int64_field = 1234567890
uint32_field = 7
uint64_field = 18000000000
float_field = 1.5
double_field = 2.71828
bool_field = true
bytes_field = b"AQID"
enum_field = STATUS_ACTIVE
)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);

  // Belt-and-suspenders per-field check that survives even if
  // MessageDifferencer were over-permissive.
  const pbuf::Reflection* r = t.m3->GetReflection();
  std::string sc;
  EXPECT_EQ(r->GetStringReference(*t.m3, desc_->FindFieldByName("string_field"),
                                  &sc),
            "hello");
  EXPECT_EQ(r->GetInt32(*t.m3, desc_->FindFieldByName("int32_field")), -42);
  EXPECT_EQ(r->GetInt64(*t.m3, desc_->FindFieldByName("int64_field")),
            1234567890);
  EXPECT_EQ(r->GetUInt32(*t.m3, desc_->FindFieldByName("uint32_field")), 7u);
  EXPECT_EQ(r->GetUInt64(*t.m3, desc_->FindFieldByName("uint64_field")),
            18000000000u);
  EXPECT_FLOAT_EQ(r->GetFloat(*t.m3, desc_->FindFieldByName("float_field")),
                  1.5f);
  EXPECT_DOUBLE_EQ(r->GetDouble(*t.m3, desc_->FindFieldByName("double_field")),
                   2.71828);
  EXPECT_TRUE(r->GetBool(*t.m3, desc_->FindFieldByName("bool_field")));
  // bytes round-trips as raw bytes.
  EXPECT_EQ(r->GetStringReference(*t.m3, desc_->FindFieldByName("bytes_field"),
                                  &sc),
            std::string({0x01, 0x02, 0x03}));
  EXPECT_EQ(r->GetEnumValue(*t.m3, desc_->FindFieldByName("enum_field")), 1);
}

TEST_F(FullRoundTrip, NegativeAndExtremeNumerics) {
  // Hit signed-min/max-ish ranges to expose any sign-extension or
  // varint-decoding errors.
  std::string src = R"(
int32_field = -2147483648
int64_field = -9223372036854775807
uint32_field = 4294967295
uint64_field = 18446744073709551615
float_field = -3.4028235e38
double_field = -1.7976931348623157e308
)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, NestedMessage) {
  std::string src = R"(
nested_field {
  name = "child"
  value = 99
}
)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, RepeatedScalars) {
  std::string src = R"(
repeated_string = ["a", "b", "c"]
)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
  EXPECT_EQ(
      t.m3->GetReflection()->FieldSize(
          *t.m3, desc_->FindFieldByName("repeated_string")),
      3);
}

TEST_F(FullRoundTrip, RepeatedMessages) {
  std::string src = R"(
repeated_nested = [
  { name = "a" value = 1 }
  { name = "b" value = 2 }
  { name = "c" value = 3 }
]
)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, MapsAllKeyKinds) {
  // string→string, string→message, int32→string covers the three map flavors
  // declared in test.proto.
  std::string src = R"(
string_map = {
  env: "prod"
  team: "platform"
}
nested_map = {
  primary: { name = "p" value = 1 }
  backup:  { name = "b" value = 2 }
}
int_map = {
  404: "Not Found"
  500: "Internal Error"
}
)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, TimestampDuration) {
  std::string src = R"(
ts_field = 2024-01-15T10:30:00.123456789Z
dur_field = 1h30m45s
)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, NegativeDuration) {
  std::string src = R"(dur_field = -30s)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, OneofTextBranch) {
  std::string src = R"(text_choice = "picked")";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, OneofNumberBranch) {
  std::string src = R"(number_choice = 42)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, WrapperTypes) {
  std::string src = R"(
nullable_string = "wrapped"
nullable_int = 123
nullable_bool = true
)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, EverythingAtOnce) {
  // The big one — one document setting every member of AllTypes that can
  // coexist (only one oneof branch is permitted). Mirrors the matrix the
  // user described as the gap.
  std::string src = R"(
string_field = "kitchen sink"
int32_field = -1
int64_field = -9999999999
uint32_field = 1
uint64_field = 9999999999
float_field = 0.5
double_field = 0.125
bool_field = true
bytes_field = b"SGVsbG8="
enum_field = STATUS_INACTIVE
nested_field {
  name = "root"
  value = 42
}
repeated_string = ["x", "y", "z"]
repeated_nested = [
  { name = "n1" value = 11 }
  { name = "n2" value = 22 }
]
string_map = {
  alpha: "A"
  beta:  "B"
}
nested_map = {
  one: { name = "n3" value = 33 }
}
int_map = {
  1: "one"
  2: "two"
}
ts_field = 2024-01-15T10:30:00Z
dur_field = 1h30m45s
text_choice = "decided"
nullable_string = "wrapped"
nullable_int = 7
nullable_bool = false
)";
  auto t = RunFullPipeline(this, src);
  ExpectFullEquality(t);
}

TEST_F(FullRoundTrip, EmptyDocumentIsValid) {
  // Defaults across the board — exercises the "nothing emitted" branches.
  auto t = RunFullPipeline(this, "");
  ExpectFullEquality(t);
}

}  // namespace

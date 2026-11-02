// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// AppendDatasetHeader and AppendDatasetRow. Ported from protowire-go's
// dataset_write_test.go, with the same expected text.

#include "protowire/pxf.h"
#include "protowire/pxf/dataset_writer.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/field_comparator.h>
#include <google/protobuf/util/message_differencer.h>

#include "protowire/detail/go_quote.h"
#include "protowire/pxf/wellknown.h"

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;
using protowire::Status;
using protowire::detail::GoQuote;
using Columns = std::vector<std::string>;
using MessagePtr = std::unique_ptr<pb::Message>;
using Rng = std::mt19937_64;

constexpr char kSchema[] = R"schema(
syntax = "proto3";
package dsw.v1;

import "google/protobuf/duration.proto";
import "google/protobuf/timestamp.proto";
import "google/protobuf/wrappers.proto";
import "pxf/annotations.proto";
import "pxf/bignum.proto";
import "pxf/secret.proto";

enum Color {
  COLOR_UNSPECIFIED = 0;
  COLOR_RED = 1;
  COLOR_GREEN = 2;
}

message Row {
  string s = 1;
  bool b = 2;
  int32 i32 = 3;
  sint32 si32 = 4;
  sfixed32 sf32 = 5;
  int64 i64 = 6;
  sint64 si64 = 7;
  sfixed64 sf64 = 8;
  uint32 u32 = 9;
  fixed32 f32 = 10;
  uint64 u64 = 11;
  fixed64 f64 = 12;
  float fl = 13;
  double db = 14;
  bytes by = 15;
  Color color = 16;
  optional int32 opt = 17;

  google.protobuf.Timestamp ts = 20;
  google.protobuf.Duration dur = 21;
  google.protobuf.StringValue w_s = 22;
  google.protobuf.Int64Value w_i64 = 23;
  google.protobuf.BoolValue w_b = 24;
  google.protobuf.BytesValue w_by = 25;
  pxf.BigInt bi = 26;
  pxf.Decimal dec = 27;
  pxf.BigFloat bf = 28;
  pxf.Secret sec = 29;

  // Defaults: an empty cell would read back as the default.
  string d_s = 40 [(pxf.default) = "dflt"];
  int32 d_i32 = 41 [(pxf.default) = "7"];
  optional int32 d_opt = 42 [(pxf.default) = "8"];
  google.protobuf.Int32Value d_w = 43 [(pxf.default) = "9"];
  google.protobuf.Timestamp d_ts = 44 [(pxf.default) = "2026-01-01T00:00:00Z"];
  Color d_color = 45 [(pxf.default) = "COLOR_GREEN"];

  oneof choice {
    google.protobuf.StringValue c_w = 50 [(pxf.default) = "picked"];
    int32 c_i = 51;
    bool c_b = 52;
  }

  // A scalar member with a default: unset with no member set, it is an
  // empty cell (null is rejected on a scalar) and reads back as the
  // default.
  oneof other {
    string o_s = 53 [(pxf.default) = "other"];
    int32 o_i = 54;
  }

  // Value cells (protowire#341): lists and blocks.
  repeated int32 list = 60;
  map<string, int32> m = 61;
  Inner inner = 62;
  repeated Inner inners = 63;
  repeated Keyed keyed = 64 [(pxf.key) = "id"];
  map<string, Inner> by_key = 65;
}

message Keyed {
  string id = 1;
  int32 v = 2;
}

message Inner {
  int32 x = 1;
  string label = 2;
  Deep deep = 3;
}

message Deep {
  int32 n = 1;
}

// Required fields below the root, for dotted columns.
message ReqParent {
  string id = 1;
  ReqChild child = 2 [(pxf.required) = true];
}

message ReqChild {
  int32 n = 1 [(pxf.required) = true];
  int32 m = 2;
}

message Pick {
  oneof choice {
    google.protobuf.StringValue c_w = 1 [(pxf.default) = "picked"];
    int32 c_i = 2;
    bool c_b = 3;
  }
  oneof other {
    string o_s = 4 [(pxf.default) = "other"];
    int32 o_i = 5;
  }
}

message Req {
  int32 id = 1 [(pxf.required) = true];
  optional string name = 2 [(pxf.required) = true];
  string note = 3;
}

// Annotated fields inside a value cell.
message CellBody {
  int32 d = 1 [(pxf.default) = "7"];
  google.protobuf.Int32Value dw = 2 [(pxf.default) = "9"];
  optional int32 od = 3 [(pxf.default) = "8"];
  int32 r = 4 [(pxf.required) = true];
}

message CellHolder {
  CellBody body = 1;
  repeated CellBody bodies = 2;
}

message CellRequired {
  optional string name = 1 [(pxf.required) = true];
}

message CellRequiredHolder {
  CellRequired body = 1;
}

message EmptyRequiredList {
  repeated int32 ints = 1 [(pxf.required) = true];
}

message EmptyRequiredMessages {
  repeated Deep deeps = 1 [(pxf.required) = true];
}

message EmptyRequiredMap {
  map<string, int32> counts = 1 [(pxf.required) = true];
}
)schema";

// Every cell-shaped field of dsw.v1.Row.
const Columns kRowColumns = {
    "s",    "b",   "i32", "si32", "sf32",  "i64",  "si64",  "sf64",  "u32",    "f32",   "u64",
    "f64",  "fl",  "db",  "by",   "color", "opt",  "ts",    "dur",   "w_s",    "w_i64", "w_b",
    "w_by", "bi",  "dec", "bf",   "sec",   "d_s",  "d_i32", "d_opt", "d_w",    "d_ts",  "d_color",
    "c_w",  "c_i", "c_b", "o_s",  "o_i",   "list", "m",     "inner", "inners", "keyed", "by_key",
};

// The columns of dsw.v1.Row with (pxf.default). A table that leaves one out
// reads it back as its default, so a round trip over a column subset always
// keeps these.
const Columns kDefaultedColumns = {"d_s", "d_i32", "d_opt", "d_w", "d_ts", "d_color", "c_w", "o_s"};

const char* const kTrickyStrings[] = {"",
                                      "plain",
                                      "a, b",
                                      "(x)",
                                      "\"q\"",
                                      "back\\slash",
                                      "line\nbreak",
                                      "tab\t",
                                      "# not a comment",
                                      "// nor this",
                                      "ünï ✓",
                                      "null",
                                      "true",
                                      "@dataset"};

class Errors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
           ": " + std::string(msg);
  }
  std::string last;
};

int IntN(Rng& r, int n) {
  return static_cast<int>(r() % static_cast<uint64_t>(n));
}

int32_t SmallInt(Rng& r) {
  return static_cast<int32_t>(r() & 0xFFFFFFFF) - static_cast<int32_t>(r() & 0xFFFF);
}

std::string Tricky(Rng& r) {
  return kTrickyStrings[IntN(r, static_cast<int>(std::size(kTrickyStrings)))];
}

bool Contains(const Columns& cols, const std::string& c) {
  return std::find(cols.begin(), cols.end(), c) != cols.end();
}

std::string Join(const std::vector<std::string>& parts, const char* sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += sep;
    out += parts[i];
  }
  return out;
}

// A document setting one of the value-cell columns at random, or "" for any
// other column.
std::string RandomNested(Rng& r, const std::string& col) {
  const auto inner = [&r]() -> std::string {
    switch (IntN(r, 3)) {
      case 0:
        return "{}";
      case 1:
        return "{ x = " + std::to_string(SmallInt(r)) + " }";
      default:
        return "{ x = " + std::to_string(IntN(r, 9)) + " label = " + GoQuote(Tricky(r)) + " }";
    }
  };
  const int n = IntN(r, 4);
  std::vector<std::string> parts;
  if (col == "list") {
    for (int i = 0; i < n; ++i) parts.push_back(std::to_string(SmallInt(r)));
    return "list = [" + Join(parts, ", ") + "]";
  }
  if (col == "m") {
    for (int i = 0; i < n; ++i) {
      parts.push_back(GoQuote(std::to_string(i) + Tricky(r)) + ": " + std::to_string(IntN(r, 100)));
    }
    return "m = { " + Join(parts, " ") + " }";
  }
  if (col == "inner") return "inner " + inner();
  if (col == "inners") {
    for (int i = 0; i < n; ++i) parts.push_back(inner());
    return "inners = [" + Join(parts, ", ") + "]";
  }
  if (col == "keyed") {
    if (IntN(r, 2) == 0) {  // the keyed form: distinct, non-empty keys
      for (int i = 0; i < n; ++i) {
        parts.push_back("k" + std::to_string(i) + " { v = " + std::to_string(IntN(r, 9)) + " }");
      }
      return "keyed { " + Join(parts, " ") + " }";
    }
    for (int i = 0; i < n; ++i) {  // anonymous: no key at all
      parts.push_back("{ v = " + std::to_string(IntN(r, 9)) + " }");
    }
    return "keyed = [" + Join(parts, ", ") + "]";
  }
  if (col == "by_key") {
    for (int i = 0; i < n; ++i) parts.push_back("\"k" + std::to_string(i) + "\": " + inner());
    return "by_key = { " + Join(parts, " ") + " }";
  }
  return "";
}

// Unlike the Go generator this leaves out inf, -inf and nan: this port's lexer
// and decoder do not read them yet (#78), so a row holding one cannot read
// back. NonFiniteFloats pins the text the writer gives them.
void SetRandomScalar(Rng& r, pb::Message* m, const pb::FieldDescriptor* fd) {
  const pb::Reflection* ref = m->GetReflection();
  using FD = pb::FieldDescriptor;
  switch (fd->cpp_type()) {
    case FD::CPPTYPE_STRING: {
      if (fd->type() != FD::TYPE_BYTES) {
        ref->SetString(m, fd, Tricky(r));
        break;
      }
      std::string b(static_cast<size_t>(IntN(r, 12)), '\0');
      for (char& c : b) c = static_cast<char>(r());
      ref->SetString(m, fd, b);
      break;
    }
    case FD::CPPTYPE_BOOL:
      ref->SetBool(m, fd, IntN(r, 2) == 0);
      break;
    case FD::CPPTYPE_INT32: {
      using L = std::numeric_limits<int32_t>;
      const int32_t v[] = {0, -1, 1, L::min(), L::max(), static_cast<int32_t>(r())};
      ref->SetInt32(m, fd, v[IntN(r, 6)]);
      break;
    }
    case FD::CPPTYPE_INT64: {
      using L = std::numeric_limits<int64_t>;
      const int64_t v[] = {0, -1, L::min(), L::max(), static_cast<int64_t>(r())};
      ref->SetInt64(m, fd, v[IntN(r, 5)]);
      break;
    }
    case FD::CPPTYPE_UINT32: {
      const uint32_t v[] = {0, 1, std::numeric_limits<uint32_t>::max(), static_cast<uint32_t>(r())};
      ref->SetUInt32(m, fd, v[IntN(r, 4)]);
      break;
    }
    case FD::CPPTYPE_UINT64: {
      const uint64_t v[] = {0, 1, std::numeric_limits<uint64_t>::max(), r()};
      ref->SetUInt64(m, fd, v[IntN(r, 4)]);
      break;
    }
    case FD::CPPTYPE_FLOAT: {
      using L = std::numeric_limits<float>;
      const float v[] = {
          0, -0.5F, 3.25F, L::max(), L::denorm_min(), static_cast<float>(r() % 1000) / 1000.0F};
      ref->SetFloat(m, fd, v[IntN(r, 6)]);
      break;
    }
    case FD::CPPTYPE_DOUBLE: {
      using L = std::numeric_limits<double>;
      const double v[] = {
          0, -1e300, 1.0 / 3, L::max(), L::denorm_min(), static_cast<double>(r() % 100000) / 977.0};
      ref->SetDouble(m, fd, v[IntN(r, 6)]);
      break;
    }
    case FD::CPPTYPE_ENUM:
      ref->SetEnumValue(m, fd, IntN(r, 3));
      break;
    case FD::CPPTYPE_MESSAGE:
      FAIL() << "no generator for " << fd->name();
  }
}

std::string RandomLiteral(Rng& r, const std::string& name) {
  const auto pick = [&r](std::initializer_list<const char*> v) -> std::string {
    return *(v.begin() + IntN(r, static_cast<int>(v.size())));
  };
  if (name == "google.protobuf.Timestamp") {
    return pick({"1970-01-01T00:00:00Z",
                 "2026-10-23T12:34:56.789Z",
                 "0001-01-01T00:00:00Z",
                 "9999-12-31T23:59:59.999999999Z"});
  }
  if (name == "google.protobuf.Duration") return pick({"0s", "1.5s", "-3h2m", "1ns", "-1µs"});
  if (name == "google.protobuf.StringValue" || name == "pxf.Secret") return GoQuote(Tricky(r));
  if (name == "google.protobuf.Int64Value") return pick({"0", "-9223372036854775808", "42"});
  if (name == "google.protobuf.Int32Value") return pick({"0", "-2147483648", "42"});
  if (name == "google.protobuf.BoolValue") return pick({"true", "false"});
  if (name == "google.protobuf.BytesValue") return pick({"b\"\"", "b\"AQID\""});
  if (name == "pxf.BigInt") return pick({"0", "-1", "123456789012345678901234567890"});
  if (name == "pxf.Decimal") {
    return pick({"0", "-0.001", "3.14159265358979323846", "100000000000000000000"});
  }
  if (name == "pxf.BigFloat") return pick({"0", "-2.5", "1.25e-30"});
  ADD_FAILURE() << "no literal generator for " << name;
  return "";
}

class DatasetWriter : public ::testing::Test {
 protected:
  void SetUp() override {
    tree_.MapPath("", protowire_test::FixtureDir());
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    Compile("dsw.proto", kSchema);
    ASSERT_NE(file_, nullptr) << errors_.last;
  }

  // Compile writes src to name and compiles it, into file_.
  void Compile(const std::string& name, const std::string& src) {
    std::ofstream(protowire_test::FixtureDir() + "/" + name, std::ios::binary) << src;
    file_ = importer_->Import(name);
  }

  const pb::Descriptor* Desc(const std::string& name) const {
    return importer_->pool()->FindMessageTypeByName("dsw.v1." + name);
  }

  MessagePtr New(const pb::Descriptor* d) { return MessagePtr(factory_.GetPrototype(d)->New()); }

  // Parsed is a new message of type d holding doc.
  MessagePtr Parsed(const pb::Descriptor* d, const std::string& doc) {
    MessagePtr m = New(d);
    Status st = pxf::Unmarshal(doc, m.get());
    EXPECT_TRUE(st.ok()) << doc << ": " << st.message();
    return m;
  }

  static std::string Row(const pb::Message& m, const Columns& cols) {
    std::string out;
    Status st = pxf::AppendDatasetRow(&out, m, cols);
    EXPECT_TRUE(st.ok()) << st.message();
    return out;
  }

  static std::string Header(const pb::Descriptor* d, const Columns& cols) {
    std::string out;
    Status st = pxf::AppendDatasetHeader(&out, d, cols);
    EXPECT_TRUE(st.ok()) << st.message();
    return out;
  }

  static std::string RowError(const pb::Message& m, const Columns& cols) {
    std::string out;
    Status st = pxf::AppendDatasetRow(&out, m, cols);
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(out, "");
    return st.message();
  }

  // MakeWritable sets what a row cannot carry unset: d_opt, an optional
  // scalar with a default, and a member of oneof "other", whose defaulted
  // member is a scalar. Written unset they are empty cells and read back as
  // their defaults, so a round trip expecting equal messages sets them.
  // "other" counts as unset when its set member is not among cols, since
  // that member is not written. Only fields among cols are touched.
  static void MakeWritable(pb::Message* m, const Columns& cols) {
    const auto* desc = m->GetDescriptor();
    const pb::Reflection* r = m->GetReflection();
    const auto* d_opt = desc->FindFieldByName("d_opt");
    if (Contains(cols, "d_opt") && !r->HasField(*m, d_opt)) r->SetInt32(m, d_opt, 0);
    const auto* set = r->GetOneofFieldDescriptor(*m, desc->FindOneofByName("other"));
    if (Contains(cols, "o_s") && (set == nullptr || !Contains(cols, std::string(set->name())))) {
      r->SetString(m, desc->FindFieldByName("o_s"), "");
    }
  }

  // OnlyColumns is a copy of m with only the fields among cols: what a row of
  // those columns reads back as.
  MessagePtr OnlyColumns(const pb::Message& m, const Columns& cols) {
    MessagePtr out(m.New());
    out->CopyFrom(m);
    const auto* desc = m.GetDescriptor();
    for (int i = 0; i < desc->field_count(); ++i) {
      const auto* fd = desc->field(i);
      if (!Contains(cols, std::string(fd->name()))) out->GetReflection()->ClearField(out.get(), fd);
    }
    return out;
  }

  // RandomRow fills each column of a dsw.v1.Row at random, leaving about a
  // third unset. Scalars are set through reflection; literal-shaped messages
  // through Unmarshal of a random literal, which is how a caller would hold
  // them.
  MessagePtr RandomRow(Rng& r, const pb::Descriptor* desc) {
    MessagePtr m = New(desc);
    for (const std::string& c : kRowColumns) {
      if (IntN(r, 3) == 0) continue;
      const auto* fd = desc->FindFieldByName(c);
      if (std::string doc = RandomNested(r, c); !doc.empty()) {
        Status st = pxf::Unmarshal(doc, m.get());
        EXPECT_TRUE(st.ok()) << "setting " << doc << ": " << st.message();
        continue;
      }
      if (fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
        const std::string doc =
            c + " = " + RandomLiteral(r, std::string(fd->message_type()->full_name()));
        Status st = pxf::Unmarshal(doc, m.get());
        EXPECT_TRUE(st.ok()) << "setting " << doc << ": " << st.message();
        continue;
      }
      SetRandomScalar(r, m.get(), fd);
    }
    MakeWritable(m.get(), kRowColumns);
    return m;
  }

  // WriteTable writes a header and one row per message.
  static std::string WriteTable(const pb::Descriptor* desc,
                                const Columns& cols,
                                const std::vector<MessagePtr>& msgs) {
    std::string out = Header(desc, cols);
    for (const MessagePtr& m : msgs) {
      const size_t before = out.size();
      Status st = pxf::AppendDatasetRow(&out, *m, cols);
      EXPECT_TRUE(st.ok()) << st.message();
      // One row per line, nested cells included.
      EXPECT_EQ(std::count(out.begin() + static_cast<std::ptrdiff_t>(before), out.end(), '\n'), 1)
          << out.substr(before);
    }
    return out;
  }

  static bool Same(const pb::Message& want, const pb::Message& got, std::string* diff) {
    pb::util::MessageDifferencer differ;
    pb::util::DefaultFieldComparator comparator;
    comparator.set_treat_nan_as_equal(true);
    differ.set_field_comparator(&comparator);
    differ.ReportDifferencesToString(diff);
    return differ.Compare(want, got);
  }

  // ExpectReadsBack reads table both ways, streaming (Scan) and materializing
  // (Parse + BindRow), and requires each row to equal msgs, in order.
  using Msgs = std::vector<const pb::Message*>;

  void ExpectReadsBack(const pb::Descriptor* desc,
                       const std::string& table,
                       const std::vector<const pb::Message*>& msgs) {
    std::istringstream in(table);
    auto tr = pxf::DatasetReader::Create(&in);
    ASSERT_TRUE(tr.ok()) << tr.status().message();
    EXPECT_EQ((*tr)->Type(), std::string(desc->full_name()));
    for (size_t i = 0; i < msgs.size(); ++i) {
      MessagePtr got = New(desc);
      Status st = (*tr)->Scan(got.get());
      ASSERT_TRUE(st.ok()) << "row " << i << ": " << st.message() << "\n" << table;
      ASSERT_FALSE((*tr)->Done()) << "row " << i;
      std::string diff;
      ASSERT_TRUE(Same(*msgs[i], *got, &diff)) << "Scan row " << i << ":\n" << diff << table;
    }
    pxf::DatasetRow end;
    ASSERT_TRUE((*tr)->Next(&end).ok());
    EXPECT_TRUE((*tr)->Done());

    auto doc = pxf::Parse(table);
    ASSERT_TRUE(doc.ok()) << doc.status().message();
    ASSERT_EQ(doc->datasets.size(), 1u);
    const auto& ds = doc->datasets[0];
    ASSERT_EQ(ds.rows.size(), msgs.size());
    for (size_t i = 0; i < msgs.size(); ++i) {
      MessagePtr got = New(desc);
      Status st = pxf::BindRow(got.get(), ds.columns, ds.rows[i]);
      ASSERT_TRUE(st.ok()) << "row " << i << ": " << st.message();
      std::string diff;
      ASSERT_TRUE(Same(*msgs[i], *got, &diff)) << "BindRow row " << i << ":\n" << diff;
    }
  }

  void ExpectReadsBack(const pb::Descriptor* desc,
                       const std::string& table,
                       const std::vector<MessagePtr>& msgs) {
    std::vector<const pb::Message*> raw;
    raw.reserve(msgs.size());
    for (const MessagePtr& m : msgs) raw.push_back(m.get());
    ExpectReadsBack(desc, table, raw);
  }

  // WithDefaults is m as a table with these columns reads it back: every
  // field of dsw.v1.Row outside the columns that has (pxf.default) takes it.
  MessagePtr WithDefaults(const pb::Message& m, const Columns& cols) {
    const auto* desc = m.GetDescriptor();
    MessagePtr out(m.New());
    out->CopyFrom(m);
    MessagePtr defaults = New(desc);
    EXPECT_TRUE(pxf::UnmarshalFull("", defaults.get()).ok());
    const pb::Reflection* r = out->GetReflection();
    for (const std::string& c : kDefaultedColumns) {
      if (Contains(cols, c)) continue;
      const auto* fd = desc->FindFieldByName(c);
      const auto* od = fd->real_containing_oneof();
      if (od != nullptr && r->GetOneofFieldDescriptor(*out, od) != nullptr) continue;
      r->SwapFields(out.get(), defaults.get(), {fd});
    }
    return out;
  }

  pb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  pb::DynamicMessageFactory factory_;
};

#define EXPECT_CONTAINS(haystack, needle) \
  EXPECT_NE(std::string(haystack).find(needle), std::string::npos) << "got: " << (haystack)

TEST_F(DatasetWriter, RoundTrip) {
  const auto* desc = Desc("Row");
  Rng r(142);
  std::vector<MessagePtr> msgs;
  for (int i = 0; i < 2000; ++i) msgs.push_back(RandomRow(r, desc));
  MessagePtr empty = New(desc);  // every column unset that can be
  MakeWritable(empty.get(), kRowColumns);
  msgs.push_back(std::move(empty));
  ExpectReadsBack(desc, WriteTable(desc, kRowColumns, msgs), msgs);
}

// The round trip on a chosen seed and column subset. The whole message is
// written, as a caller exporting some of its fields would write it, and each
// row must read back as the message's column fields alone. A field outside
// the columns can still shape a cell: a set oneof member that is not a column
// must not keep a column member's default off.
TEST_F(DatasetWriter, ColumnSubsetRoundTrip) {
  const auto* desc = Desc("Row");
  Rng masks(7);
  for (int round = 0; round < 40; ++round) {
    const uint64_t mask = round == 0 ? 0 : round == 1 ? ~uint64_t{0} : masks();
    Columns cols;
    for (size_t i = 0; i < kRowColumns.size(); ++i) {
      const std::string& c = kRowColumns[i];
      if ((mask & (uint64_t{1} << i)) != 0 || Contains(kDefaultedColumns, c)) cols.push_back(c);
    }
    Rng r(masks());
    std::vector<MessagePtr> msgs;
    std::vector<MessagePtr> want;
    for (int i = 0; i < 8; ++i) {
      msgs.push_back(RandomRow(r, desc));
      for (const char* c : {"c_i", "c_b", "o_i"}) {
        // Set a non-defaulted oneof member now and then, so the ones left out
        // of cols get exercised.
        if (IntN(r, 4) == 0) SetRandomScalar(r, msgs.back().get(), desc->FindFieldByName(c));
      }
      MakeWritable(msgs.back().get(), cols);
      want.push_back(OnlyColumns(*msgs.back(), cols));
    }
    ExpectReadsBack(desc, WriteTable(desc, cols, msgs), want);
    if (HasFatalFailure()) return;
  }
}

// Appending rows to a file that already holds a header and rows gives the
// same bytes as writing them all at once: nothing earlier changes.
TEST_F(DatasetWriter, AppendNeedsNoRewrite) {
  const auto* desc = Desc("Row");
  Rng r(77);
  std::vector<MessagePtr> msgs;
  for (int i = 0; i < 50; ++i) msgs.push_back(RandomRow(r, desc));
  const std::string whole = WriteTable(desc, kRowColumns, msgs);

  std::string file = Header(desc, kRowColumns);
  for (int i = 0; i < 20; ++i) file += Row(*msgs[static_cast<size_t>(i)], kRowColumns);
  const std::string before = file;
  for (size_t i = 20; i < msgs.size(); ++i) {
    ASSERT_TRUE(pxf::AppendDatasetRow(&file, *msgs[i], kRowColumns).ok());
  }
  EXPECT_EQ(file.substr(0, before.size()), before);
  EXPECT_EQ(file, whole);
  ExpectReadsBack(desc, file, msgs);
}

TEST_F(DatasetWriter, Text) {
  const auto* desc = Desc("Row");
  const Columns cols = {"s", "i32", "opt", "d_i32", "d_w", "color", "ts"};
  EXPECT_EQ(Header(desc, cols), "@dataset dsw.v1.Row (s, i32, opt, d_i32, d_w, color, ts)\n");

  MessagePtr m = New(desc);
  // Unset: empty, except d_i32 (zero written, so the default does not replace
  // it) and d_w (a wrapper: null, so it stays unset).
  EXPECT_EQ(Row(*m, cols), "(, , , 0, null, , )\n");

  const std::string doc =
      "s = \"a, (b)\" i32 = -3 opt = 0 d_i32 = 4 color = COLOR_RED ts = 2026-10-23T00:00:00Z";
  ASSERT_TRUE(pxf::Unmarshal(doc, m.get()).ok());
  EXPECT_EQ(Row(*m, cols), "(\"a, (b)\", -3, 0, 4, null, COLOR_RED, 2026-10-23T00:00:00Z)\n");
}

TEST_F(DatasetWriter, NonFiniteFloats) {
  const auto* desc = Desc("Row");
  MessagePtr m = New(desc);
  const pb::Reflection* r = m->GetReflection();
  r->SetFloat(m.get(), desc->FindFieldByName("fl"), -std::numeric_limits<float>::infinity());
  r->SetDouble(m.get(), desc->FindFieldByName("db"), std::numeric_limits<double>::infinity());
  EXPECT_EQ(Row(*m, {"fl", "db"}), "(-inf, inf)\n");
  r->SetDouble(m.get(), desc->FindFieldByName("db"), std::numeric_limits<double>::quiet_NaN());
  EXPECT_EQ(Row(*m, {"db"}), "(nan)\n");
}

TEST_F(DatasetWriter, OneofDefaults) {
  const auto* desc = Desc("Pick");
  const auto field = [desc](const char* name) { return desc->FindFieldByName(name); };
  const Columns cols = {"c_w", "c_i", "c_b", "o_s", "o_i"};

  // No member of "choice" set: its defaulted member is a wrapper, so it is
  // null, which keeps the default off. "other" has o_i set.
  MessagePtr m = New(desc);
  const pb::Reflection* r = m->GetReflection();
  r->SetInt32(m.get(), field("o_i"), 4);
  const std::string row = Row(*m, cols);
  EXPECT_EQ(row, "(null, , , , 4)\n");

  // A member set: the others stay empty, the defaulted one included.
  MessagePtr m2 = New(desc);
  r->SetBool(m2.get(), field("c_b"), false);
  r->SetString(m2.get(), field("o_s"), "");
  const std::string row2 = Row(*m2, cols);
  EXPECT_EQ(row2, "(, , false, \"\", )\n");

  ExpectReadsBack(desc, Header(desc, cols) + row + row2, Msgs{m.get(), m2.get()});

  // No member of "other" set: its defaulted member is a scalar, which no cell
  // holds unset (null is rejected on a scalar), so it is empty and reads back
  // as its default.
  EXPECT_EQ(Row(*New(desc), cols), "(null, , , , )\n");

  // A member set, but not among the columns: it is not written, so it cannot
  // keep the default off. The wrapper member is null, as if no member were
  // set, and reads back unset; the scalar member is empty, and reads back as
  // its default.
  MessagePtr m4 = New(desc);
  r->SetInt32(m4.get(), field("c_i"), 5);
  r->SetInt32(m4.get(), field("o_i"), 6);
  EXPECT_EQ(Row(*m4, {"c_w", "o_s"}), "(null, )\n");
  // o_i is a column here, so it keeps o_s's default off and the row reads back
  // as m4 without c_i.
  const Columns cols5 = {"c_w", "o_i"};
  const std::string row5 = Row(*m4, cols5);
  EXPECT_EQ(row5, "(null, 6)\n");
  MessagePtr want5 = New(desc);
  r->SetInt32(want5.get(), field("o_i"), 6);
  ExpectReadsBack(desc, Header(desc, cols5) + row5, Msgs{want5.get()});
}

// The reader parses rows with the bind-time schema checks skipped, so the
// writers refuse a schema Unmarshal refuses rather than write a table that
// cannot be read back: a field named null makes the header unparsable, and two
// defaulted members of one oneof would be two null cells in one row.
TEST_F(DatasetWriter, RefusesInvalidSchema) {
  const std::string prelude =
      "syntax = \"proto3\";\npackage dswbad.v1;\n"
      "import \"google/protobuf/wrappers.proto\";\nimport \"pxf/annotations.proto\";\n";
  struct Case {
    const char* file;
    const char* type;
    const char* src;
    Columns cols;
    const char* want;
  };
  const Case cases[] = {
      {"res.proto",
       "dswbad.v1.Res",
       "message Res { int32 null = 1; int32 ok = 2; }",
       {"ok"},
       "uses PXF-reserved name \"null\""},
      {"two.proto",
       "dswbad.v1.Two",
       R"(message Two { oneof k {
          google.protobuf.StringValue a = 1 [(pxf.default) = "x"];
          google.protobuf.StringValue b = 2 [(pxf.default) = "y"]; } })",
       {"a", "b"},
       "at most one member of oneof \"k\" may carry a default"},
  };
  for (const Case& c : cases) {
    Compile(c.file, prelude + c.src);
    ASSERT_NE(file_, nullptr) << errors_.last;
    const auto* desc = importer_->pool()->FindMessageTypeByName(c.type);
    ASSERT_NE(desc, nullptr);
    std::string out;
    Status st = pxf::AppendDatasetHeader(&out, desc, c.cols);
    ASSERT_FALSE(st.ok());
    EXPECT_CONTAINS(st.message(), c.want);
    EXPECT_EQ(
        st.message().rfind(
            "pxf: @dataset " + std::string(desc->full_name()) + ": PXF schema violations:\n  ", 0),
        0u)
        << st.message();
    EXPECT_CONTAINS(RowError(*New(desc), c.cols), c.want);
  }
}

// A field with presence in the message's _null mask is null, as Marshal writes
// it, so a default stays off and a required field is satisfied; that includes
// an optional scalar. A plain scalar in the mask is written as if the mask did
// not name it, by the writer and by Marshal alike: null on it is a decode
// error.
TEST_F(DatasetWriter, NullMask) {
  Compile("null.proto", R"(syntax = "proto3";
package dswnull.v1;
import "google/protobuf/field_mask.proto";
import "google/protobuf/wrappers.proto";
import "pxf/annotations.proto";
message N {
  google.protobuf.StringValue w = 1;
  google.protobuf.Int32Value dw = 2 [(pxf.default) = "9"];
  google.protobuf.Int32Value rw = 3 [(pxf.required) = true];
  int32 n = 4;
  google.protobuf.FieldMask _null = 5;
  optional int32 o = 6 [(pxf.default) = "7"];
})");
  ASSERT_NE(file_, nullptr) << errors_.last;
  const auto* desc = file_->FindMessageTypeByName("N");
  const Columns cols = {"w", "dw", "rw", "n", "o"};

  MessagePtr m = New(desc);
  auto full = pxf::UnmarshalFull("w = null\ndw = null\nrw = null\no = null\n", m.get());
  ASSERT_TRUE(full.ok()) << full.status().message();
  const pb::Reflection* r = m->GetReflection();
  r->SetInt32(m.get(), desc->FindFieldByName("n"), 3);
  const auto* wrapper = desc->FindFieldByName("w")->message_type();
  pb::Message* w = r->MutableMessage(m.get(), desc->FindFieldByName("w"));
  w->GetReflection()->SetString(w, wrapper->FindFieldByName("value"), "set anyway");
  // A scalar in the mask: no decode puts one there, but a mask from another
  // producer can carry one. Marshal and the writer both write its value.
  pb::Message* mask = r->MutableMessage(m.get(), desc->FindFieldByName("_null"));
  mask->GetReflection()->AddString(mask, mask->GetDescriptor()->FindFieldByName("paths"), "n");
  auto marshalled = pxf::Marshal(*m);
  ASSERT_TRUE(marshalled.ok()) << marshalled.status().message();
  EXPECT_EQ(*marshalled, "w = null\ndw = null\nrw = null\nn = 3\no = null\n");

  const std::string row = Row(*m, cols);
  EXPECT_EQ(row, "(null, null, null, 3, null)\n");

  // The null o reads back unset, not as its default, and in the mask.
  std::istringstream in(Header(desc, cols) + row);
  auto tr = pxf::DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok()) << tr.status().message();
  MessagePtr back = New(desc);
  Status st = (*tr)->Scan(back.get());
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_FALSE(back->GetReflection()->HasField(*back, desc->FindFieldByName("o")));
  const auto paths = pxf::ReadNullPaths(*back, desc->FindFieldByName("_null"));
  EXPECT_NE(std::find(paths.begin(), paths.end(), "o"), paths.end());

  // Without the mask, the set wrapper is its value and the unset required one
  // is refused.
  r->ClearField(m.get(), desc->FindFieldByName("_null"));
  EXPECT_EQ(RowError(*m, cols),
            "pxf: @dataset column \"rw\": unset, and the field is (pxf.required)");
}

// An unset optional scalar with a default is an empty cell: it reads back as
// the default, which is what an absent annotated field means.
TEST_F(DatasetWriter, UnsetOptionalScalarWithDefaultIsEmpty) {
  EXPECT_EQ(Row(*New(Desc("Row")), {"d_opt"}), "()\n");
}

// The same empty cells read back as the defaults: an unset optional scalar,
// and an unset oneof whose defaulted member is a scalar, through Scan and
// BindRow both.
TEST_F(DatasetWriter, EmptyDefaultedCellsReadBackAsDefaults) {
  const auto* desc = Desc("Row");
  const Columns cols = {"d_opt", "o_s", "o_i"};
  const std::string row = Row(*New(desc), cols);
  EXPECT_EQ(row, "(, , )\n");

  MessagePtr want = New(desc);
  want->GetReflection()->SetInt32(want.get(), desc->FindFieldByName("d_opt"), 8);
  want->GetReflection()->SetString(want.get(), desc->FindFieldByName("o_s"), "other");
  // Defaulted fields outside the columns take their defaults too.
  MessagePtr full = WithDefaults(*want, cols);
  ExpectReadsBack(desc, Header(desc, cols) + row, Msgs{full.get()});
}

TEST_F(DatasetWriter, Required) {
  const auto* desc = Desc("Req");

  std::string out;
  Status st = pxf::AppendDatasetHeader(&out, desc, {"id", "note"});
  EXPECT_EQ(st.message(),
            "pxf: @dataset columns omit \"name\", which is (pxf.required): every row would be "
            "rejected");

  const Columns cols = {"id", "name", "note"};
  MessagePtr m = New(desc);
  EXPECT_EQ(RowError(*m, cols),
            "pxf: @dataset column \"name\": unset, and the field is (pxf.required)");

  m->GetReflection()->SetString(m.get(), desc->FindFieldByName("name"), "");
  // id is a proto3 scalar: its zero is written, so it is present.
  EXPECT_EQ(Row(*m, cols), "(0, \"\", )\n");
}

TEST_F(DatasetWriter, RefusesWhatACellCannotHold) {
  const auto* desc = Desc("Row");
  const struct {
    Columns cols;
    const char* want;
  } cases[] = {
      {{}, "pxf: @dataset needs at least one column"},
      {{"inner", "inner.x"},
       "pxf: @dataset column \"inner.x\" overlaps column \"inner\": a field and a path inside it "
       "cannot both be columns (draft §3.4.4)"},
      {{"list.x"},
       "pxf: @dataset column \"list.x\": \"list\" is a repeated field, and a column path cannot "
       "go through one; use a value cell for \"list\" (draft §3.4.4)"},
      {{"m.x"},
       "pxf: @dataset column \"m.x\": \"m\" is a map field, and a column path cannot go through "
       "one; use a value cell for \"m\" (draft §3.4.4)"},
      {{"ts.seconds"},
       "pxf: @dataset column \"ts.seconds\": \"ts\" is a google.protobuf.Timestamp, which is "
       "written as a literal; a column path cannot go into it (draft §3.4.4)"},
      {{"s.x"}, "pxf: @dataset column \"s.x\": \"s\" is not a message field"},
      {{"inner.nope"}, "pxf: @dataset column \"inner.nope\": dsw.v1.Inner has no field \"nope\""},
      {{"nope"}, "pxf: @dataset column \"nope\": dsw.v1.Row has no field \"nope\""},
      {{"s", "s"}, "pxf: @dataset column \"s\" appears twice (draft §3.4.4)"},
      {{"S"}, "pxf: @dataset column \"S\": dsw.v1.Row has no field \"S\""},  // proto names, exactly
      {{"a..b"},
       "pxf: @dataset column \"a..b\": a column path has an empty segment (draft §3.4.4)"},
      // Beyond the Go test: an empty name is no path, and %q escapes.
      {{""}, "pxf: @dataset column \"\": dsw.v1.Row has no field \"\""},
      {{"a\"b\n"}, "pxf: @dataset column \"a\\\"b\\n\": dsw.v1.Row has no field \"a\\\"b\\n\""},
  };
  for (const auto& c : cases) {
    std::string dst = "keep";
    Status st = pxf::AppendDatasetHeader(&dst, desc, c.cols);
    EXPECT_EQ(st.message(), c.want);
    EXPECT_EQ(dst, "keep");
    st = pxf::AppendDatasetRow(&dst, *New(desc), c.cols);
    EXPECT_EQ(st.message(), c.want);
    EXPECT_EQ(dst, "keep");
  }
}

// A list, map or block value is one cell on one line, the text Marshal writes
// for the field; a pxf.Secret with a hint, which needs a block, is one too.
TEST_F(DatasetWriter, ValueCells) {
  const auto* desc = Desc("Row");
  MessagePtr m = Parsed(desc,
                        "list = [1, 2, 3] m = { \"a b\": 1 \"c\": 2 } inner {} "
                        "inners = [{ x = 1 }, { label = \"l,)\" }] keyed { a { v = 1 } } "
                        "by_key = { \"k\": { x = 2 } } sec { value = \"pw\" hint = \"vault\" }");
  const Columns cols = {"list", "m", "inner", "inners", "keyed", "by_key", "sec"};
  const std::string row = Row(*m, cols);
  EXPECT_EQ(row,
            "([ 1, 2, 3 ], { \"a b\": 1 c: 2 }, { }, [ { x = 1 }, { label = \"l,)\" } ], "
            "{ a { v = 1 } }, { k: { x = 2 } }, { value = \"pw\" hint = \"vault\" })\n");
  ExpectReadsBack(desc, Header(desc, cols) + row, Msgs{WithDefaults(*m, cols).get()});
}

// Inside a cell an unset annotated field follows the row's rules, and an
// unset required field with presence is refused under the field's own path.
TEST_F(DatasetWriter, ValueCellBodies) {
  const auto* desc = Desc("CellHolder");
  MessagePtr m = New(desc);
  const pb::Reflection* r = m->GetReflection();
  r->MutableMessage(m.get(), desc->FindFieldByName("body"));
  r->AddMessage(m.get(), desc->FindFieldByName("bodies"));
  EXPECT_EQ(Row(*m, {"body", "bodies"}),
            "({ d = 0 dw = null r = 0 }, [ { d = 0 dw = null r = 0 } ])\n");

  const auto* held = Desc("CellRequiredHolder");
  MessagePtr h = New(held);
  h->GetReflection()->MutableMessage(h.get(), held->FindFieldByName("body"));
  EXPECT_EQ(RowError(*h, {"body"}),
            "pxf: @dataset column \"body\": name is unset, and the field is (pxf.required)");
}

// An empty required list has no value a cell can hold. For a map or a list of
// messages this is Go's error. For a list of scalars Go panics, or for strings
// writes the list's address, so this port returns the same kind of error.
TEST_F(DatasetWriter, EmptyRequiredList) {
  EXPECT_EQ(RowError(*New(Desc("EmptyRequiredMessages")), {"deeps"}),
            "pxf: @dataset column \"deeps\": unsupported kind: message");
  EXPECT_EQ(RowError(*New(Desc("EmptyRequiredMap")), {"counts"}),
            "pxf: @dataset column \"counts\": unsupported kind: message");
  EXPECT_EQ(RowError(*New(Desc("EmptyRequiredList")), {"ints"}),
            "pxf: @dataset column \"ints\": unsupported kind: int32");
}

// Dotted columns write the leaf through its parents; a row reads back as the
// message it came from, except that a sub-message whose cells all come out
// empty reads back unset: the accepted loss.
TEST_F(DatasetWriter, DottedRoundTrip) {
  const auto* desc = Desc("Row");
  const Columns cols = {"s", "inner.x", "inner.label", "inner.deep.n", "i32"};
  const auto* inner_fd = desc->FindFieldByName("inner");
  const auto* deep_fd = inner_fd->message_type()->FindFieldByName("deep");
  Rng r(342);
  std::vector<MessagePtr> wrote;
  std::vector<MessagePtr> want;
  for (int round = 0; round < 500; ++round) {
    std::vector<std::string> parts;
    if (IntN(r, 2) == 0) parts.push_back("s = " + GoQuote(Tricky(r)));
    if (IntN(r, 4) > 0) {
      std::vector<std::string> in;
      if (IntN(r, 2) == 0) in.push_back("x = " + std::to_string(IntN(r, 5)));
      if (IntN(r, 2) == 0) in.push_back("label = " + GoQuote(Tricky(r)));
      if (IntN(r, 2) == 0) {
        const char* deeps[] = {"", "n = 0", "n = 4"};
        in.push_back(std::string("deep { ") + deeps[IntN(r, 3)] + " }");
      }
      parts.push_back("inner { " + Join(in, " ") + " }");
    }
    MessagePtr m = Parsed(desc, Join(parts, "\n"));
    MakeWritable(m.get(), cols);

    // What reads back: a sub-message is present only if a cell under it is
    // non-empty. A proto3 int32 of 0 and an empty string are unset, so they
    // write empty cells.
    MessagePtr exp(m->New());
    exp->CopyFrom(*m);
    const pb::Reflection* er = exp->GetReflection();
    if (er->HasField(*exp, inner_fd)) {
      pb::Message* in = er->MutableMessage(exp.get(), inner_fd);
      const pb::Reflection* ir = in->GetReflection();
      if (ir->HasField(*in, deep_fd)) {
        const pb::Message& deep = ir->GetMessage(*in, deep_fd);
        std::vector<const pb::FieldDescriptor*> set;
        deep.GetReflection()->ListFields(deep, &set);
        if (set.empty()) ir->ClearField(in, deep_fd);
      }
      std::vector<const pb::FieldDescriptor*> set;
      ir->ListFields(*in, &set);
      if (set.empty()) er->ClearField(exp.get(), inner_fd);
    }
    want.push_back(WithDefaults(*exp, cols));
    wrote.push_back(std::move(m));
  }
  ExpectReadsBack(desc, WriteTable(desc, cols, wrote), want);
}

TEST_F(DatasetWriter, DottedText) {
  const auto* desc = Desc("Row");
  const Columns cols = {"inner.x", "inner.deep.n", "s"};
  EXPECT_EQ(Header(desc, cols), "@dataset dsw.v1.Row (inner.x, inner.deep.n, s)\n");

  const struct {
    const char* doc;
    const char* want;
  } cases[] = {
      {R"(inner { x = 3 deep { n = 4 } } s = "a")", "(3, 4, \"a\")\n"},
      {R"(inner { x = 3 } s = "a")", "(3, , \"a\")\n"},
      {R"(s = "a")", "(, , \"a\")\n"},
      // Present but empty: all-empty cells, read back unset.
      {"inner { deep {} }", "(, , )\n"},
  };
  for (const auto& c : cases) EXPECT_EQ(Row(*Parsed(desc, c.doc), cols), c.want) << c.doc;
}

// A required field below the root is the one way a dotted column can make a
// row the reader rejects; the writer refuses such a row.
TEST_F(DatasetWriter, DottedRequired) {
  const auto* desc = Desc("ReqParent");

  // The required child is covered by a path, but this row's cells under it
  // are all empty, so it would read back unset.
  EXPECT_EQ(RowError(*Parsed(desc, "child { n = 1 }"), {"id", "child.m"}),
            "pxf: @dataset columns under \"child\" are all empty in this row, so it reads back "
            "unset, and it is (pxf.required)");

  // The child reads back present, but its required n is no column.
  EXPECT_EQ(RowError(*Parsed(desc, "child { n = 1 m = 5 }"), {"id", "child.m"}),
            "pxf: @dataset \"child\" reads back present, and its (pxf.required) field \"child.n\" "
            "is not a column: the row would be rejected");

  // Covered: n = 0 is written as 0 (a required proto3 scalar), so the child
  // reads back present and complete.
  const Columns cols = {"id", "child.n", "child.m"};
  MessagePtr m = Parsed(desc, "child { n = 0 m = 2 }");
  const std::string row = Row(*m, cols);
  EXPECT_EQ(row, "(, 0, 2)\n");
  ExpectReadsBack(desc, Header(desc, cols) + row, Msgs{m.get()});

  // The root's required child is not covered at all: every row fails.
  std::string out;
  EXPECT_CONTAINS(pxf::AppendDatasetHeader(&out, desc, {"id"}).message(),
                  "omit \"child\", which is (pxf.required)");
}

// A column path may go into a pxf.Secret, which has a block form: each of its
// fields is a cell, and the row reads back as written. A present Secret with
// every cell empty reads back absent, the loss every dotted parent takes.
TEST_F(DatasetWriter, DottedIntoSecret) {
  const auto* desc = Desc("Row");
  Columns cols = {"sec.value", "sec.hint"};
  cols.insert(cols.end(), kDefaultedColumns.begin(), kDefaultedColumns.end());
  const std::string header = Header(desc, cols);
  for (const char* doc :
       {R"(sec { value = "pw" hint = "vault" })", R"(sec { value = "pw" })", R"(sec = "pw")", ""}) {
    MessagePtr m = Parsed(desc, std::string(doc) + " d_opt = 1 o_s = \"x\"");
    ExpectReadsBack(desc, header + Row(*m, cols), Msgs{WithDefaults(*m, cols).get()});
  }
}

}  // namespace

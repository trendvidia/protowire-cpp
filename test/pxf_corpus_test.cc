// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Drives the cross-port fixtures vendored from trendvidia/protowire's
// testdata/ through this port. CI has no checkout of that repository, so
// the fixtures live here; refresh a directory by copying it from there,
// and a new file fails the test until it is given a verdict.
//
// A file named err-* MUST be rejected and any other MUST be accepted,
// except in testdata/dataset, whose verdicts are the table below.

#include "protowire/pxf.h"
#include "protowire/pxf/dataset_reader.h"
#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

namespace {

namespace pbuf = google::protobuf;
namespace fs = std::filesystem;
using protowire::Status;
namespace pxf = protowire::pxf;

class CollectErrors : public pbuf::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

std::string ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<fs::path> Fixtures(const std::string& dir) {
  std::vector<fs::path> out;
  for (const auto& e : fs::directory_iterator(fs::path(TESTDATA_DIR) / dir)) {
    if (e.path().extension() == ".pxf") out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool IsReject(const fs::path& p) {
  return p.filename().string().rfind("err-", 0) == 0;
}

// Schema compiles the named protos of one corpus directory into one pool.
class Schema {
 public:
  Schema(const std::string& dir, const std::vector<std::string>& protos) {
    tree_.MapPath("", WKT_PROTO_DIR);
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", TESTDATA_DIR);
    tree_.MapPath("", (fs::path(TESTDATA_DIR) / dir).string());
    importer_ = std::make_unique<pbuf::compiler::Importer>(&tree_, &errors_);
    for (const auto& p : protos) {
      EXPECT_NE(importer_->Import(p), nullptr) << errors_.last_;
    }
    factory_ = std::make_unique<pbuf::DynamicMessageFactory>(importer_->pool());
  }

  const pbuf::Descriptor* Find(const std::string& name) const {
    return importer_->pool()->FindMessageTypeByName(name);
  }

  std::unique_ptr<pbuf::Message> New(const pbuf::Descriptor* d) const {
    return std::unique_ptr<pbuf::Message>(factory_->GetPrototype(d)->New());
  }

 private:
  pbuf::compiler::DiskSourceTree tree_;
  CollectErrors errors_;
  std::unique_ptr<pbuf::compiler::Importer> importer_;
  std::unique_ptr<pbuf::DynamicMessageFactory> factory_;
};

// TypeOf reads the fixture's own `@type` line.
const pbuf::Descriptor* TypeOf(const Schema& schema, const std::string& text) {
  auto doc = pxf::Parse(text);
  if (!doc.ok()) {
    // A fixture the parser refuses still names its type on a line of its own.
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("@type ", 0) == 0) return schema.Find(line.substr(6));
    }
    return nullptr;
  }
  return schema.Find(doc->type_url);
}

// DecodeBothWays runs the two public decoders. They must agree, and the
// returned status is theirs.
Status DecodeBothWays(const Schema& schema,
                      const std::string& text,
                      pxf::UnmarshalOptions opts = {}) {
  const pbuf::Descriptor* d = TypeOf(schema, text);
  if (d == nullptr) return Status::Error("fixture names no known @type");
  auto plain = schema.New(d);
  Status a = pxf::Unmarshal(text, plain.get(), opts);
  auto full = schema.New(d);
  auto b = pxf::UnmarshalFull(text, full.get(), opts);
  EXPECT_EQ(a.ok(), b.ok()) << "Unmarshal: " << a.ToString()
                            << "\nUnmarshalFull: " << b.status().ToString();
  return a.ok() ? b.status() : a;
}

// ParseVerdict says what the schema-less Parse of a directory must do.
enum class ParseVerdict { kSameAsDecoder, kAccepts };

void RunDecoderCorpus(const std::string& dir,
                      const std::vector<std::string>& protos,
                      ParseVerdict parse) {
  Schema schema(dir, protos);
  auto files = Fixtures(dir);
  ASSERT_FALSE(files.empty());
  int ran = 0;
  for (const auto& path : files) {
    SCOPED_TRACE(path.filename().string());
    std::string text = ReadFile(path);
    Status st = DecodeBothWays(schema, text);
    if (IsReject(path)) {
      EXPECT_FALSE(st.ok()) << "MUST be rejected";
    } else {
      EXPECT_TRUE(st.ok()) << st.ToString();
    }
    auto doc = pxf::Parse(text);
    if (parse == ParseVerdict::kAccepts || !IsReject(path)) {
      EXPECT_TRUE(doc.ok()) << doc.status().ToString();
    } else {
      EXPECT_FALSE(doc.ok()) << "Parse MUST reject";
    }
    ++ran;
  }
  EXPECT_EQ(ran, static_cast<int>(files.size()));
}

TEST(PxfCorpus, Blocks) {
  RunDecoderCorpus("blocks", {"blocks.proto"}, ParseVerdict::kSameAsDecoder);
}

TEST(PxfCorpus, Strings) {
  RunDecoderCorpus("strings", {"strings.proto"}, ParseVerdict::kSameAsDecoder);
}

TEST(PxfCorpus, Nulls) {
  RunDecoderCorpus("nulls",
                   {"nulls.proto", "nulls_proto2.proto", "nulls_editions.proto"},
                   ParseVerdict::kAccepts);
}

TEST(PxfCorpus, Duplicates) {
  RunDecoderCorpus("duplicates", {"duplicates.proto"}, ParseVerdict::kAccepts);
}

// BodyScope keeps the first 64 field indexes in a word and the rest in a
// vector; the rule has to hold on both sides of that line.
TEST(PxfCorpus, BoundTwicePastTheSixtyFourthField) {
  const std::string name = "wide.proto";
  {
    std::ofstream out(fs::path(protowire_test::FixtureDir()) / name);
    out << "syntax = \"proto3\";\npackage wide.v1;\nmessage M {\n";
    for (int i = 1; i <= 70; ++i) out << "  int32 f" << i << " = " << i << ";\n";
    out << "}\n";
  }
  pbuf::compiler::DiskSourceTree tree;
  tree.MapPath("", protowire_test::FixtureDir());
  CollectErrors errors;
  pbuf::compiler::Importer importer(&tree, &errors);
  ASSERT_NE(importer.Import(name), nullptr) << errors.last_;
  pbuf::DynamicMessageFactory factory(importer.pool());
  const pbuf::Descriptor* d = importer.pool()->FindMessageTypeByName("wide.v1.M");
  for (const char* f : {"f64", "f65", "f70"}) {
    SCOPED_TRACE(f);
    const std::string once = std::string(f) + " = 1\n";
    std::unique_ptr<pbuf::Message> a(factory.GetPrototype(d)->New());
    EXPECT_TRUE(pxf::Unmarshal(once, a.get()).ok());
    std::unique_ptr<pbuf::Message> b(factory.GetPrototype(d)->New());
    EXPECT_FALSE(pxf::Unmarshal(once + once, b.get()).ok());
  }
}

// Resolves a type URL to the message named after its last '/', as
// testdata/any/README.md specifies.
class LastSegmentResolver : public pxf::TypeResolver {
 public:
  explicit LastSegmentResolver(const Schema& schema) : schema_(schema) {}
  const pbuf::Descriptor* FindMessageByURL(std::string_view url) override {
    return schema_.Find(std::string(url.substr(url.rfind('/') + 1)));
  }

 private:
  const Schema& schema_;
};

TEST(PxfCorpus, Any) {
  // A parser cannot tell which blocks are Any, nor resolve a URL.
  const std::set<std::string> parse_accepts = {"err-not-any.pxf", "err-unresolved.pxf"};
  Schema schema("any", {"any.proto"});
  LastSegmentResolver resolver(schema);
  pxf::UnmarshalOptions opts;
  opts.type_resolver = &resolver;
  auto files = Fixtures("any");
  ASSERT_EQ(files.size(), 11u);
  for (const auto& path : files) {
    SCOPED_TRACE(path.filename().string());
    std::string text = ReadFile(path);
    Status st = DecodeBothWays(schema, text, opts);
    EXPECT_EQ(st.ok(), !IsReject(path)) << st.ToString();
    auto doc = pxf::Parse(text);
    const bool parses = !IsReject(path) || parse_accepts.count(path.filename().string()) > 0;
    EXPECT_EQ(doc.ok(), parses) << doc.status().ToString();
  }
}

// The fields inside an Any body belong to the packed message, so they say
// nothing about whether a root field of the same name is present.
TEST(PxfCorpus, AnyBodyDoesNotMarkRootPresence) {
  Schema schema("any", {"any.proto"});
  LastSegmentResolver resolver(schema);
  pxf::UnmarshalOptions opts;
  opts.type_resolver = &resolver;
  auto msg = schema.New(schema.Find("any.v1.Holder"));
  auto r = pxf::UnmarshalFull("payload { @type = \"any.v1.Leaf\" tag = \"t\" }\n", msg.get(), opts);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->IsSet("payload"));
  EXPECT_TRUE(r->IsAbsent("tag"));
}

// Every accept fixture in nulls/ binds null and nothing else: the message
// stays empty and a full decode reports the field present-but-null.
TEST(PxfCorpus, NullsLeaveTheFieldUnsetAndNull) {
  Schema schema("nulls", {"nulls.proto", "nulls_proto2.proto", "nulls_editions.proto"});
  int ran = 0;
  for (const auto& path : Fixtures("nulls")) {
    if (IsReject(path)) continue;
    SCOPED_TRACE(path.filename().string());
    std::string text = ReadFile(path);
    auto doc = pxf::Parse(text);
    ASSERT_TRUE(doc.ok()) << doc.status().ToString();
    ASSERT_EQ(doc->entries.size(), 1u);
    const auto& a = std::get<std::unique_ptr<pxf::Assignment>>(doc->entries[0]);
    auto msg = schema.New(schema.Find(doc->type_url));
    auto r = pxf::UnmarshalFull(text, msg.get());
    ASSERT_TRUE(r.ok()) << r.status().ToString();
    EXPECT_TRUE(r->IsNull(a->key));
    EXPECT_EQ(msg->ByteSizeLong(), 0u);
    ++ran;
  }
  EXPECT_EQ(ran, 9);
}

// An escaped line feed in a cell survives BindRow's trip through PXF text.
TEST(PxfCorpus, CellStringKeepsAnEscapedNewline) {
  Schema schema("dataset", {"test.proto"});
  auto doc = pxf::Parse("@dataset test.v1.AllTypes (string_field)\n(\"a\\nb\")\n");
  ASSERT_TRUE(doc.ok()) << doc.status().ToString();
  const pbuf::Descriptor* d = schema.Find("test.v1.AllTypes");
  auto msg = schema.New(d);
  Status st = pxf::BindRow(msg.get(), doc->datasets[0].columns, doc->datasets[0].rows[0]);
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(msg->GetReflection()->GetString(*msg, d->FindFieldByName("string_field")), "a\nb");
}

// --- @dataset ---------------------------------------------------------------

struct DatasetFixture {
  bool accept;
  // The issue that makes this port meet the fixture, while it does not.
  const char* gap = nullptr;
};

const std::map<std::string, DatasetFixture>& DatasetVerdicts() {
  static const auto* table = new std::map<std::string, DatasetFixture>{
      {"empty-rows.pxf", {true}},
      {"rows-end-at-directive.pxf", {true}},
      {"list-cell.pxf", {true}},
      {"block-cell.pxf", {true}},
      {"nested-cells.pxf", {true}},
      {"cell-framing.pxf", {true}},
      {"arity-long.pxf", {false}},
      {"arity-short.pxf", {false}},
      {"junk-between-rows.pxf", {false}},
      {"truncated-row-eof.pxf", {false}},
      {"truncated-row-then-rows.pxf", {false}},
      {"with-body.pxf", {false}},
      {"with-type.pxf", {false}},
      {"block-on-scalar.pxf", {false}},
      {"string-newline-in-cell.pxf", {false}},
      {"null-on-scalar-cell.pxf", {false}},
      {"null-on-optional-cell.pxf", {true}},
      {"dotted-column.pxf", {true}},
      {"dotted-grouping.pxf", {true}},
      {"dotted-null-leaf.pxf", {true}},
      {"dotted-oneof.pxf", {true}},
      {"dotted-required.pxf", {true}},
      {"dotted-with-value-cells.pxf", {true}},
      {"dotted-oneof-two-members.pxf", {false}},
      {"dotted-through-repeated.pxf", {false}},
      {"dotted-through-map.pxf", {false}},
      {"dotted-into-timestamp.pxf", {false}},
      {"dotted-into-wrapper.pxf", {false}},
      {"dotted-overlap.pxf", {false}},
      {"duplicate-column.pxf", {false}},
      {"dotted-required-missing.pxf", {false}},
      {"dotted-empty-segment.pxf", {false}},
      {"dotted-trailing-dot.pxf", {false}},
      {"dotted-unknown-segment.pxf", {false}},
      {"dotted-through-scalar.pxf", {false}},
      {"dotted-into-duration.pxf", {false}},
      {"dotted-into-bignum.pxf", {false}},
      {"dotted-overlap-deep.pxf", {false}},
      {"dotted-bad-header-empty-row.pxf", {false}},
      {"dotted-into-secret.pxf", {true}},
      {"dotted-into-any.pxf", {true}},
      {"dotted-bad-header-no-rows.pxf", {true}},
  };
  return *table;
}

// BindMaterialized is `pxf validate`: parse, then bind every row.
Status BindMaterialized(const Schema& schema, const std::string& text) {
  auto doc = pxf::Parse(text);
  if (!doc.ok()) return doc.status();
  for (const auto& ds : doc->datasets) {
    const pbuf::Descriptor* d = schema.Find(ds.type);
    if (d == nullptr) return Status::Error("unknown row type " + ds.type);
    for (const auto& row : ds.rows) {
      auto msg = schema.New(d);
      if (Status s = pxf::BindRow(msg.get(), ds.columns, row); !s.ok()) return s;
    }
  }
  return Status::OK();
}

// BindStreaming reads every dataset of the document through DatasetReader.
Status BindStreaming(const Schema& schema, const std::string& text) {
  auto input = std::make_unique<std::istringstream>(text);
  for (;;) {
    auto reader = pxf::DatasetReader::Create(input.get());
    if (!reader.ok()) return reader.status();
    const pbuf::Descriptor* d = schema.Find((*reader)->Type());
    if (d == nullptr) return Status::Error("unknown row type " + (*reader)->Type());
    for (;;) {
      auto msg = schema.New(d);
      if (Status s = (*reader)->Scan(msg.get()); !s.ok()) return s;
      if ((*reader)->Done()) break;
    }
    auto tail = (*reader)->Tail();
    std::string rest((std::istreambuf_iterator<char>(*tail)), std::istreambuf_iterator<char>());
    bool blank = true;
    std::istringstream lines(rest);
    std::string line;
    while (std::getline(lines, line)) {
      auto at = line.find_first_not_of(" \t\r");
      if (at != std::string::npos && line[at] != '#') blank = false;
    }
    if (blank) return Status::OK();
    if (rest.find("@dataset") == std::string::npos) {
      return Status::Error("junk after the rows: top-level field entries");
    }
    input = std::make_unique<std::istringstream>(std::move(rest));
  }
}

TEST(PxfCorpus, Dataset) {
  Schema schema("dataset", {"test.proto", "paths.proto"});
  auto files = Fixtures("dataset");
  ASSERT_EQ(files.size(), DatasetVerdicts().size());
  std::set<std::string> seen;
  for (const auto& path : files) {
    const std::string name = path.filename().string();
    SCOPED_TRACE(name);
    auto it = DatasetVerdicts().find(name);
    ASSERT_NE(it, DatasetVerdicts().end()) << "no verdict for this fixture";
    seen.insert(name);
    std::string text = ReadFile(path);
    Status materialized = BindMaterialized(schema, text);
    Status streamed = BindStreaming(schema, text);
    EXPECT_EQ(materialized.ok(), streamed.ok())
        << "materialized: " << materialized.ToString() << "\nstreamed: " << streamed.ToString();
    if (it->second.gap != nullptr) {
      // A closed gap fails here until its entry is removed.
      EXPECT_NE(materialized.ok(), it->second.accept)
          << "the gap is closed; drop it from the table: " << it->second.gap;
      continue;
    }
    EXPECT_EQ(materialized.ok(), it->second.accept) << materialized.ToString();
  }
  EXPECT_EQ(seen.size(), DatasetVerdicts().size());
}

// Deterministic bytes, so two messages compare whatever their map order.
std::string Bytes(const pbuf::Message& m) {
  std::string out;
  pbuf::io::StringOutputStream raw(&out);
  pbuf::io::CodedOutputStream coded(&raw);
  coded.SetSerializationDeterministic(true);
  m.SerializeToCodedStream(&coded);
  return out;
}

// A verdict says a row bound, not what it bound to. Each row here must
// bind to the message of the body it stands for, as the fixture's own
// comment states, on both the materializing and the streaming path.
TEST(PxfCorpus, DatasetRowsBindToTheirBodies) {
  struct Case {
    const char* file;
    const char* type;
    std::vector<const char*> bodies;
  };
  const std::vector<Case> cases = {
      {"list-cell.pxf",
       "test.v1.AllTypes",
       {R"(string_field = "row-1" repeated_string = ["alpha", "beta"])"}},
      {"block-cell.pxf",
       "test.v1.AllTypes",
       {R"(string_field = "row-1" nested_field { name = "inner" value = 99 })"}},
      {"nested-cells.pxf",
       "test.v1.AllTypes",
       {R"(string_field = "row-1" nested_field { name = "n1" value = 1 }
           repeated_string = ["a", "b"] repeated_nested = [{ name = "x" }, { name = "y" value = 2 }]
           string_map = { "k": "v" } int32_field = 1)",
        R"(string_field = "row-2" nested_field {} repeated_string = [] int32_field = 2)",
        R"(string_field = "row-3" int32_field = 3)",
        R"(string_field = "row-4" nested_field = null int32_field = 4)"}},
      {"dotted-grouping.pxf",
       "dataset.v1.Event",
       {R"(id = "r1" desc { energy = 0.5 tempo { min_bpm = 60 max_bpm = 120 } tags = ["x"] }
           plain = 1)",
        R"(id = "r2" desc { energy = 0.25 } plain = 2)",
        R"(id = "r3" plain = 3)",
        R"(id = "r4" desc { tempo { max_bpm = 9 } } plain = 4)"}},
      {"dotted-oneof.pxf",
       "dataset.v1.Event",
       {R"(a { name = "x" n = 1 })", R"(b { name = "y" })"}},
      {"dotted-required.pxf",
       "dataset.v1.Event",
       {R"(id = "absent")", R"(id = "set" req { n = 5 })"}},
      {"dotted-null-leaf.pxf", "dataset.v1.Event", {R"(id = "n" desc { label = null })"}},
      {"dotted-into-any.pxf",
       "dataset.v1.Event",
       {R"(id = "x" payload { type_url = "type.googleapis.com/dataset.v1.Pick" value = b"CgF4" })"}},
      {"dotted-into-secret.pxf",
       "dataset.v1.Event",
       {R"(id = "x" pw { value = "s3cret" hint = "db password" })"}},
  };
  Schema schema("dataset", {"test.proto", "paths.proto"});
  for (const Case& c : cases) {
    SCOPED_TRACE(c.file);
    const pbuf::Descriptor* d = schema.Find(c.type);
    ASSERT_NE(d, nullptr);
    std::string text = ReadFile(fs::path(TESTDATA_DIR) / "dataset" / c.file);
    auto doc = pxf::Parse(text);
    ASSERT_TRUE(doc.ok()) << doc.status().ToString();
    ASSERT_EQ(doc->datasets[0].rows.size(), c.bodies.size());
    std::istringstream in(text);
    auto reader = pxf::DatasetReader::Create(&in);
    ASSERT_TRUE(reader.ok()) << reader.status().ToString();
    for (size_t i = 0; i < c.bodies.size(); ++i) {
      SCOPED_TRACE(i);
      auto want = schema.New(d);
      auto r = pxf::UnmarshalFull(c.bodies[i], want.get());
      ASSERT_TRUE(r.ok()) << r.status().ToString();
      auto bound = schema.New(d);
      Status st = pxf::BindRow(bound.get(), doc->datasets[0].columns, doc->datasets[0].rows[i]);
      ASSERT_TRUE(st.ok()) << st.ToString();
      EXPECT_EQ(Bytes(*bound), Bytes(*want)) << bound->ShortDebugString();
      auto scanned = schema.New(d);
      st = (*reader)->Scan(scanned.get());
      ASSERT_TRUE(st.ok()) << st.ToString();
      EXPECT_EQ(Bytes(*scanned), Bytes(*want)) << scanned->ShortDebugString();
    }
  }
}

// A row names its column when a value fails, and reports the row's own
// line rather than a line of the synthetic body.
TEST(PxfCorpus, DatasetBindErrorNamesColumnAtRow) {
  Schema schema("dataset", {"test.proto"});
  auto doc = pxf::Parse(
      "@dataset test.v1.AllTypes (string_field, int32_field)\n(\"a\", 1)\n"
      "(\"b\", \"bad\")\n");
  ASSERT_TRUE(doc.ok()) << doc.status().ToString();
  const auto& ds = doc->datasets[0];
  auto msg = schema.New(schema.Find("test.v1.AllTypes"));
  Status st = pxf::BindRow(msg.get(), ds.columns, ds.rows[1]);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("column int32_field"), std::string::npos) << st.ToString();
  ASSERT_TRUE(st.position().has_value());
  EXPECT_EQ(st.position()->first, 3);
}

// Every entry point refuses a schema the spec refuses at bind time. Scan
// checks once per type; BindRow on every call unless told otherwise.
TEST(PxfCorpus, DatasetBindChecksTheSchema) {
  struct Case {
    const char* rule;
    const char* field;
  };
  const std::vector<Case> cases = {
      {"reserved name", "string null = 2;"},
      {"pxf.key placement", "string k = 2 [(pxf.key) = \"name\"];"},
      {"required with default", "string r = 2 [(pxf.required) = true, (pxf.default) = \"x\"];"},
  };
  int n = 0;
  for (const Case& c : cases) {
    SCOPED_TRACE(c.rule);
    const std::string name = "bad" + std::to_string(++n) + ".proto";
    {
      std::ofstream out(fs::path(protowire_test::FixtureDir()) / name);
      out << "syntax = \"proto3\";\npackage bad" << n << ";\nimport \"pxf/annotations.proto\";\n"
          << "message Row {\n  string id = 1;\n  " << c.field << "\n}\n";
    }
    pbuf::compiler::DiskSourceTree tree;
    tree.MapPath("", WKT_PROTO_DIR);
    tree.MapPath("", PROTO_DIR);
    tree.MapPath("", protowire_test::FixtureDir());
    CollectErrors errors;
    pbuf::compiler::Importer importer(&tree, &errors);
    ASSERT_NE(importer.Import(name), nullptr) << errors.last_;
    pbuf::DynamicMessageFactory factory(importer.pool());
    const pbuf::Descriptor* d =
        importer.pool()->FindMessageTypeByName("bad" + std::to_string(n) + ".Row");
    auto fresh = [&] { return std::unique_ptr<pbuf::Message>(factory.GetPrototype(d)->New()); };

    const std::string text = "@dataset x.Row (id)\n(\"a\")\n(\"b\")\n";
    auto doc = pxf::Parse(text);
    ASSERT_TRUE(doc.ok()) << doc.status().ToString();
    const auto& ds = doc->datasets[0];

    Status bound = pxf::BindRow(fresh().get(), ds.columns, ds.rows[0]);
    EXPECT_FALSE(bound.ok());
    EXPECT_NE(bound.message().find("PXF schema violations"), std::string::npos) << bound.ToString();

    std::istringstream in(text);
    auto reader = pxf::DatasetReader::Create(&in);
    ASSERT_TRUE(reader.ok()) << reader.status().ToString();
    Status scanned = (*reader)->Scan(fresh().get());
    EXPECT_FALSE(scanned.ok());
    EXPECT_NE(scanned.message().find("PXF schema violations"), std::string::npos)
        << scanned.ToString();

    pxf::UnmarshalOptions skip;
    skip.skip_validate = true;
    Status unchecked = pxf::BindRow(fresh().get(), ds.columns, ds.rows[0], skip);
    EXPECT_EQ(unchecked.message().find("PXF schema violations"), std::string::npos)
        << unchecked.ToString();
  }
}

}  // namespace

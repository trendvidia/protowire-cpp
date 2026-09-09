// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Keyed repeated fields (draft -01 §3.13; protowire#116, #18) against the
// spec repo's testdata/keyed/ fixtures, vendored under testdata/keyed/:
// accept fixtures decode and re-encode to their body, the anonymous form
// decodes to the same message and canonicalizes to the keyed form, the
// reject fixtures are refused with the expected error, the fmt pairs
// canonicalize byte-for-byte through CanonicalizeKeyed + FormatDocument
// and the expected files are fixed points, (pxf.key) placement is
// validated at bind time, and the quoted entry-name grammar round-trips.

#include "protowire/pxf.h"
#include "protowire/pxf/annotations.h"
#include "protowire/pxf/format.h"
#include "protowire/pxf/keyed.h"
#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/message_differencer.h>

namespace {

namespace pb = google::protobuf;
using protowire::pxf::Assignment;
using protowire::pxf::Block;
using protowire::pxf::CanonicalizeKeyed;
using protowire::pxf::FormatDocument;
using protowire::pxf::Parse;

class CollectErrors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

const std::string kDir = std::string(TESTDATA_DIR) + "/keyed/";

std::string ReadFixture(const std::string& name) {
  std::ifstream in(kDir + name, std::ios::binary);
  EXPECT_TRUE(in) << "cannot read " << kDir << name;
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// CleanFixture strips comment lines and collapses the blank runs they
// leave behind, yielding the byte-exact document the encoder is expected
// to reproduce (the fixtures' comments are documentation, not content).
std::string CleanFixture(const std::string& data) {
  std::string out;
  std::istringstream in(data);
  std::string line;
  while (std::getline(in, line)) {
    size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] == '#') continue;
    out += line;
    out += '\n';
  }
  while (out.find("\n\n\n") != std::string::npos) {
    out.replace(out.find("\n\n\n"), 3, "\n\n");
  }
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out + "\n";
}

class Keyed : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", kDir);
    source_tree_.MapPath("", PROTO_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("keyed.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
  }

  // Resolves the fixture's @type directive against the compiled schema.
  const pb::Descriptor* DescOf(const std::string& text) {
    auto doc = Parse(text);
    EXPECT_TRUE(doc.ok()) << doc.status().ToString();
    if (!doc.ok()) return nullptr;
    EXPECT_FALSE(doc->type_url.empty()) << "fixture missing @type directive";
    const pb::Descriptor* d = importer_->pool()->FindMessageTypeByName(doc->type_url);
    EXPECT_NE(d, nullptr) << doc->type_url;
    return d;
  }
  const pb::Descriptor* Desc(const char* name) {
    const pb::Descriptor* d = importer_->pool()->FindMessageTypeByName(name);
    EXPECT_NE(d, nullptr) << name;
    return d;
  }
  std::unique_ptr<pb::Message> New(const pb::Descriptor* d) {
    return std::unique_ptr<pb::Message>(factory_->GetPrototype(d)->New());
  }
  std::unique_ptr<pb::Message> Decode(const pb::Descriptor* d,
                                      const std::string& text,
                                      protowire::Status* st) {
    auto msg = New(d);
    *st = protowire::pxf::Unmarshal(text, msg.get());
    return msg;
  }
  std::string Marshal(const pb::Message& m, const std::string& type_url = "") {
    protowire::pxf::MarshalOptions opts;
    opts.type_url = type_url;
    auto out = protowire::pxf::Marshal(m, opts);
    EXPECT_TRUE(out.ok()) << out.status().ToString();
    return out.ok() ? *out : "";
  }
  // Fmt is the reference `pxf fmt` pipeline: parse, canonicalize against
  // the schema, format.
  std::string Fmt(const std::string& text, const pb::Descriptor* d) {
    auto doc = Parse(text);
    EXPECT_TRUE(doc.ok()) << doc.status().ToString();
    if (!doc.ok()) return "";
    CanonicalizeKeyed(&*doc, d);
    return FormatDocument(*doc);
  }
  static std::string Key(const pb::Message& elem, const char* field = "id") {
    std::string scratch;
    return elem.GetReflection()->GetStringReference(
        elem, elem.GetDescriptor()->FindFieldByName(field), &scratch);
  }

  pb::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
};

TEST_F(Keyed, FixturesRoundtrip) {
  for (const char* name : {"roundtrip-keyed.pxf", "roundtrip-quoted.pxf"}) {
    SCOPED_TRACE(name);
    const std::string data = ReadFixture(name);
    const pb::Descriptor* d = DescOf(data);
    ASSERT_NE(d, nullptr);
    protowire::Status st;
    auto msg = Decode(d, data, &st);
    ASSERT_TRUE(st.ok()) << st.ToString();
    EXPECT_EQ(Marshal(*msg, std::string(d->full_name())), CleanFixture(data))
        << "decode → encode must reproduce the body";
    // UnmarshalFull agrees.
    auto full = New(d);
    auto r = protowire::pxf::UnmarshalFull(data, full.get());
    EXPECT_TRUE(r.ok()) << r.status().ToString();
  }
}

TEST_F(Keyed, AnonymousEquivalence) {
  const std::string anon = ReadFixture("anonymous-equivalence.pxf");
  const std::string keyed = ReadFixture("roundtrip-keyed.pxf");
  const pb::Descriptor* d = DescOf(anon);
  ASSERT_NE(d, nullptr);
  protowire::Status st;
  auto anon_msg = Decode(d, anon, &st);
  ASSERT_TRUE(st.ok()) << st.ToString();
  auto keyed_msg = Decode(d, keyed, &st);
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_TRUE(pb::util::MessageDifferencer::Equals(*anon_msg, *keyed_msg))
      << "anonymous form must decode to the same message";
  // Encoding the anonymous-form decode canonicalizes to the keyed form.
  EXPECT_EQ(Marshal(*anon_msg, std::string(d->full_name())), CleanFixture(keyed));
  // fmt canonicalizes the document itself to the keyed form.
  auto reparsed = Parse(Fmt(anon, d));
  ASSERT_TRUE(reparsed.ok()) << reparsed.status().ToString();
  ASSERT_EQ(reparsed->entries.size(), 3u);
  auto* blk = std::get_if<std::unique_ptr<Block>>(&reparsed->entries[2]);
  ASSERT_NE(blk, nullptr) << "children must canonicalize to a keyed block";
  EXPECT_EQ((*blk)->name, "children");
  EXPECT_EQ((*blk)->entries.size(), 2u);
}

TEST_F(Keyed, RedundantKeyOK) {
  const std::string data = ReadFixture("redundant-key-ok.pxf");
  const pb::Descriptor* d = DescOf(data);
  ASSERT_NE(d, nullptr);
  protowire::Status st;
  auto msg = Decode(d, data, &st);
  ASSERT_TRUE(st.ok()) << st.ToString();
  const auto* fd = d->FindFieldByName("children");
  ASSERT_EQ(msg->GetReflection()->FieldSize(*msg, fd), 1);
  EXPECT_EQ(Key(msg->GetReflection()->GetRepeatedMessage(*msg, fd, 0)), "greeting");
  // Encode and fmt both drop the redundant agreeing assignment: the only
  // remaining `id =` is the root's own.
  auto count = [](const std::string& s) {
    size_t n = 0;
    for (size_t p = s.find("id = "); p != std::string::npos; p = s.find("id = ", p + 1)) ++n;
    return n;
  };
  EXPECT_EQ(count(Marshal(*msg)), 1u);
  EXPECT_EQ(count(Fmt(data, d)), 1u);
}

TEST_F(Keyed, AnonymousDuplicateStaysAnonymous) {
  const std::string data = ReadFixture("anonymous-duplicate-ok.pxf");
  const pb::Descriptor* d = DescOf(data);
  ASSERT_NE(d, nullptr);
  protowire::Status st;
  auto msg = Decode(d, data, &st);
  ASSERT_TRUE(st.ok()) << st.ToString();
  const auto* fd = d->FindFieldByName("children");
  ASSERT_EQ(msg->GetReflection()->FieldSize(*msg, fd), 2);
  for (int i = 0; i < 2; ++i) {
    EXPECT_EQ(Key(msg->GetReflection()->GetRepeatedMessage(*msg, fd, i)), "dup");
  }
  EXPECT_NE(Marshal(*msg).find("children = ["), std::string::npos)
      << "duplicate keys must stay in anonymous form";
  EXPECT_NE(Fmt(data, d).find("children = ["), std::string::npos)
      << "fmt must keep the anonymous form";
}

TEST_F(Keyed, RejectFixtures) {
  struct Case {
    const char* file;
    const char* want;
  };
  for (const Case& c : {Case{"err-duplicate-key.pxf", "duplicate key \"greeting\""},
                        Case{"err-duplicate-key-spelling.pxf", "duplicate key \"greeting\""},
                        Case{"err-key-conflict.pxf", "conflicts with entry name \"greeting\""},
                        Case{"err-empty-key.pxf", "empty entry name"},
                        Case{"err-empty-key-anonymous.pxf", "explicit empty-string assignment"},
                        Case{"err-quoted-name-unkeyed.pxf",
                             "quoted entry name \"a\" is only valid inside a keyed"}}) {
    SCOPED_TRACE(c.file);
    const std::string data = ReadFixture(c.file);
    const pb::Descriptor* d = DescOf(data);
    ASSERT_NE(d, nullptr);
    // The grammar accepts every reject fixture; the schema layer refuses it.
    EXPECT_TRUE(Parse(data).ok());
    protowire::Status st;
    Decode(d, data, &st);
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.message().find(c.want), std::string::npos) << st.ToString();
    auto full = New(d);
    EXPECT_FALSE(protowire::pxf::UnmarshalFull(data, full.get()).ok());
  }
}

TEST_F(Keyed, FmtFixtures) {
  for (const char* pair : {"fmt-unquote", "fmt-anonymous-to-keyed"}) {
    SCOPED_TRACE(pair);
    const std::string input = ReadFixture(std::string(pair) + ".pxf");
    const std::string expected = ReadFixture(std::string(pair) + ".expected.pxf");
    const pb::Descriptor* d = DescOf(input);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(Fmt(input, d), expected);
    // The expected file is a fmt fixed point, and the marshaller agrees
    // with the formatter on the canonical form.
    EXPECT_EQ(Fmt(expected, d), expected);
    protowire::Status st;
    auto msg = Decode(d, input, &st);
    ASSERT_TRUE(st.ok()) << st.ToString();
    EXPECT_EQ(Marshal(*msg, std::string(d->full_name())), expected);
  }
}

TEST_F(Keyed, DescriptorHelpers) {
  const pb::Descriptor* node = Desc("keyed.v1.Node");
  const pb::Descriptor* doc = Desc("keyed.v1.Doc");
  const auto* children = node->FindFieldByName("children");
  EXPECT_TRUE(protowire::pxf::IsKeyed(children));
  EXPECT_EQ(protowire::pxf::KeyField(children), node->FindFieldByName("id"));
  EXPECT_EQ(protowire::pxf::KeyFieldName(children).value_or(""), "id");
  const auto* items = doc->FindFieldByName("items");
  EXPECT_FALSE(protowire::pxf::IsKeyed(items));
  EXPECT_EQ(protowire::pxf::KeyField(items), nullptr);
  EXPECT_FALSE(protowire::pxf::KeyFieldName(items).has_value());
  EXPECT_FALSE(protowire::pxf::IsKeyed(node->FindFieldByName("id")));
  EXPECT_TRUE(protowire::pxf::ValidateFile(file_).empty());
}

TEST_F(Keyed, PlacementValidation) {
  const char* bad = R"(
syntax = "proto3";
package badkey.v1;
import "pxf/annotations.proto";
message Elem { string name = 1; int32 count = 2; repeated string names = 3; }
message Bad {
  repeated string tags = 1 [(pxf.key) = "name"];
  Elem single = 2 [(pxf.key) = "name"];
  repeated Elem missing = 3 [(pxf.key) = "nope"];
  repeated Elem non_string = 4 [(pxf.key) = "count"];
  repeated Elem non_singular = 5 [(pxf.key) = "names"];
  repeated Elem good = 6 [(pxf.key) = "name"];
}
)";
  std::string path = std::string(testing::TempDir()) + "/badkey.proto";
  FILE* f = std::fopen(path.c_str(), "w");
  ASSERT_NE(f, nullptr);
  std::fwrite(bad, 1, std::strlen(bad), f);
  std::fclose(f);
  source_tree_.MapPath("", testing::TempDir());
  const pb::FileDescriptor* fd = importer_->Import("badkey.proto");
  ASSERT_NE(fd, nullptr) << errors_.last_;
  const pb::Descriptor* bad_d = importer_->pool()->FindMessageTypeByName("badkey.v1.Bad");
  ASSERT_NE(bad_d, nullptr);

  auto violations = protowire::pxf::ValidateFile(fd);
  ASSERT_EQ(violations.size(), 5u);
  std::vector<std::string> elements;
  for (const auto& v : violations) {
    EXPECT_EQ(v.kind, protowire::pxf::ViolationKind::kKeyOption);
    EXPECT_FALSE(v.detail.empty());
    EXPECT_NE(v.ToString().find("(pxf.key)"), std::string::npos) << v.ToString();
    elements.push_back(v.element);
  }
  EXPECT_EQ(elements,
            (std::vector<std::string>{"badkey.v1.Bad.missing",
                                      "badkey.v1.Bad.non_singular",
                                      "badkey.v1.Bad.non_string",
                                      "badkey.v1.Bad.single",
                                      "badkey.v1.Bad.tags"}));
  // Invalid placements never resolve to a key field.
  for (const char* name : {"tags", "single", "missing", "non_string", "non_singular"}) {
    EXPECT_EQ(protowire::pxf::KeyField(bad_d->FindFieldByName(name)), nullptr) << name;
  }
  EXPECT_NE(protowire::pxf::KeyField(bad_d->FindFieldByName("good")), nullptr);
  // The per-decode bind check rejects the schema outright.
  protowire::Status st;
  Decode(bad_d, "good { a { count = 1 } }", &st);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("(pxf.key)"), std::string::npos) << st.ToString();
}

TEST_F(Keyed, AssignmentSpelling) {
  const pb::Descriptor* d = Desc("keyed.v1.Node");
  // `children = { ... }` is the unabbreviated spelling of the keyed block
  // form, and `name = { ... }` of an entry.
  const std::string input = R"(
id = "root"
children = {
  greeting = { type = "Label" }
  counter_row { type = "HBox" }
}
)";
  protowire::Status st;
  auto msg = Decode(d, input, &st);
  ASSERT_TRUE(st.ok()) << st.ToString();
  const auto* fd = d->FindFieldByName("children");
  ASSERT_EQ(msg->GetReflection()->FieldSize(*msg, fd), 2);
  EXPECT_EQ(Key(msg->GetReflection()->GetRepeatedMessage(*msg, fd, 0)), "greeting");
  EXPECT_EQ(Key(msg->GetReflection()->GetRepeatedMessage(*msg, fd, 1)), "counter_row");
  // A non-block value on a named entry is an error: the element type is a
  // message.
  Decode(d, "children { greeting = 42 }", &st);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("block value"), std::string::npos) << st.ToString();
  // fmt normalizes both assignment spellings to block form.
  std::string formatted = Fmt(input, d);
  EXPECT_NE(formatted.find("children {\n"), std::string::npos) << formatted;
  EXPECT_NE(formatted.find("greeting {\n"), std::string::npos) << formatted;
}

TEST_F(Keyed, ConcatenationAcrossBlocks) {
  const pb::Descriptor* d = Desc("keyed.v1.Node");
  // Two bindings concatenate, and duplicate detection is per block.
  protowire::Status st;
  auto msg = Decode(d,
                    "id = \"root\"\nchildren {\n  greeting { type = \"Label\" }\n}\n"
                    "children {\n  greeting { type = \"HBox\" }\n}\n",
                    &st);
  ASSERT_TRUE(st.ok()) << st.ToString();
  const auto* fd = d->FindFieldByName("children");
  ASSERT_EQ(msg->GetReflection()->FieldSize(*msg, fd), 2);
  EXPECT_EQ(Key(msg->GetReflection()->GetRepeatedMessage(*msg, fd, 0), "type"), "Label");
  EXPECT_EQ(Key(msg->GetReflection()->GetRepeatedMessage(*msg, fd, 1), "type"), "HBox");
}

TEST_F(Keyed, NestedBlocks) {
  const pb::Descriptor* d = Desc("keyed.v1.Node");
  protowire::Status st;
  auto msg = Decode(d,
                    "id = \"root\"\nchildren {\n  outer {\n    type = \"VBox\"\n"
                    "    children {\n      inner { type = \"Label\" }\n    }\n  }\n}\n",
                    &st);
  ASSERT_TRUE(st.ok()) << st.ToString();
  const auto* fd = d->FindFieldByName("children");
  const auto& outer = msg->GetReflection()->GetRepeatedMessage(*msg, fd, 0);
  EXPECT_EQ(Key(outer), "outer");
  const auto& inner = outer.GetReflection()->GetRepeatedMessage(outer, fd, 0);
  EXPECT_EQ(Key(inner), "inner");
  // Nested keyed fields re-emit in keyed form, key fields omitted.
  std::string out = Marshal(*msg);
  EXPECT_EQ(out.find("id = \"outer\""), std::string::npos) << out;
  EXPECT_EQ(out.find("id = \"inner\""), std::string::npos) << out;
  EXPECT_NE(out.find("inner {"), std::string::npos) << out;
}

TEST(KeyedGrammar, QuotedEntryNamesRoundTrip) {
  // Quoted entry names parse everywhere (grammar is schema-independent)
  // and round-trip through FormatDocument with their spelling intact.
  const std::string input = "regions {\n  \"us-east-1\" {\n    replicas = 3\n  }\n}\n";
  auto doc = Parse(input);
  ASSERT_TRUE(doc.ok()) << doc.status().ToString();
  auto* regions = std::get_if<std::unique_ptr<Block>>(&doc->entries[0]);
  ASSERT_NE(regions, nullptr);
  auto* entry = std::get_if<std::unique_ptr<Block>>(&(*regions)->entries[0]);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ((*entry)->name, "us-east-1");
  EXPECT_TRUE((*entry)->name_quoted);
  EXPECT_EQ(FormatDocument(*doc), input);

  // Assignment spelling with a quoted key.
  doc = Parse("\"us-east-1\" = { replicas = 3 }\n");
  ASSERT_TRUE(doc.ok());
  auto* a = std::get_if<std::unique_ptr<Assignment>>(&doc->entries[0]);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ((*a)->key, "us-east-1");
  EXPECT_TRUE((*a)->key_quoted);

  // Integer keys remain invalid at entry-name position.
  EXPECT_FALSE(Parse("42 = 1\n").ok());
  EXPECT_FALSE(Parse("42 { }\n").ok());
}

}  // namespace

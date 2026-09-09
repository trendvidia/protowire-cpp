// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Map-key spellings against the spec repo's testdata/map-keys/ fixtures
// (vendored under testdata/map-keys/; draft -01 § Entries and Keys):
//
//   - fmt pairs (protowire#306, #27): a formatter keeps the quotes on a
//     string key spelled like a keyword or an integer, unquotes a quoted
//     identifier-safe key, and never adds quotes to a key the document
//     wrote bare. Each <name>.pxf formats to exactly <name>.expected.pxf,
//     and the expected file is a fixed point.
//   - bind rules (protowire#284): a bool key is true, false, 0, 1, "true"
//     or "false" and nothing else; on a string K the bare keyword is an
//     error, the quoted one a string.
//   - the marshaller writes a string key bare under the same
//     identifier-safe test the formatter uses.

#include "protowire/pxf.h"
#include "protowire/pxf/format.h"
#include "protowire/pxf/parser.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
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

const std::string kDir = std::string(TESTDATA_DIR) + "/map-keys/";

std::string ReadFixture(const std::string& name) {
  std::ifstream in(kDir + name, std::ios::binary);
  EXPECT_TRUE(in) << "cannot read " << kDir << name;
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

class MapKeys : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", kDir);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    const pb::FileDescriptor* file = importer_->Import("bool-keys.proto");
    ASSERT_NE(file, nullptr) << errors_.last_;
    flags_ = importer_->pool()->FindMessageTypeByName("mapkeys.v1.Flags");
    labels_ = importer_->pool()->FindMessageTypeByName("mapkeys.v1.Labels");
    ASSERT_NE(flags_, nullptr);
    ASSERT_NE(labels_, nullptr);
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
  }

  std::unique_ptr<pb::Message> New(const pb::Descriptor* d) {
    return std::unique_ptr<pb::Message>(factory_->GetPrototype(d)->New());
  }

  // Binds `text` against `d` and returns the map entries as
  // formatted-key → value, using the key's proto value (not spelling).
  std::map<std::string, std::string> Bind(const pb::Descriptor* d,
                                          const std::string& text,
                                          protowire::Status* status) {
    auto msg = New(d);
    *status = protowire::pxf::Unmarshal(text, msg.get());
    std::map<std::string, std::string> out;
    if (!status->ok()) return out;
    const auto* fd = d->field(0);
    const auto* r = msg->GetReflection();
    const auto* key_fd = fd->message_type()->map_key();
    const auto* val_fd = fd->message_type()->map_value();
    for (int i = 0; i < r->FieldSize(*msg, fd); ++i) {
      const auto& e = r->GetRepeatedMessage(*msg, fd, i);
      const auto* er = e.GetReflection();
      std::string k;
      if (key_fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_BOOL) {
        k = er->GetBool(e, key_fd) ? "true" : "false";
      } else {
        std::string scratch;
        k = er->GetStringReference(e, key_fd, &scratch);
      }
      std::string scratch;
      out[k] = er->GetStringReference(e, val_fd, &scratch);
    }
    return out;
  }

  pb::compiler::DiskSourceTree source_tree_;
  CollectErrors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::Descriptor* flags_ = nullptr;
  const pb::Descriptor* labels_ = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
};

// --- fmt pairs ---------------------------------------------------------------

std::string Format(const std::string& text) {
  auto doc = protowire::pxf::Parse(text);
  EXPECT_TRUE(doc.ok()) << doc.status().ToString();
  if (!doc.ok()) return {};
  return protowire::pxf::FormatDocument(*doc);
}

TEST_F(MapKeys, FmtKeywordKeysPair) {
  const std::string expected = ReadFixture("fmt-keyword-keys.expected.pxf");
  EXPECT_EQ(Format(ReadFixture("fmt-keyword-keys.pxf")), expected);
  EXPECT_EQ(Format(expected), expected) << "expected file is not a fixed point";
}

// The identifier production admits '.', so a quoted "a.b" canonicalizes
// to bare and a bare c.d stays bare, as keyed entry names already did;
// ".e" and "1.5" fail ident-start and stay quoted (protowire#313, #36).
TEST_F(MapKeys, FmtDottedKeysPair) {
  const std::string input = ReadFixture("fmt-dotted-keys.pxf");
  const std::string expected = ReadFixture("fmt-dotted-keys.expected.pxf");
  EXPECT_EQ(Format(input), expected);
  EXPECT_EQ(Format(expected), expected) << "expected file is not a fixed point";
  // Both documents bind to the four string keys, and the marshaller's
  // spellings match the expected file.
  for (const std::string* doc : {&input, &expected}) {
    protowire::Status st;
    auto got = Bind(labels_, *doc, &st);
    ASSERT_TRUE(st.ok()) << st.ToString();
    std::map<std::string, std::string> want = {
        {"a.b", "quoted dotted"},
        {"c.d", "bare dotted"},
        {".e", "leading dot"},
        {"1.5", "float-shaped"},
    };
    EXPECT_EQ(got, want);
  }
  auto msg = New(labels_);
  ASSERT_TRUE(protowire::pxf::Unmarshal(input, msg.get()).ok());
  protowire::pxf::MarshalOptions opts;
  opts.type_url = "mapkeys.v1.Labels";
  auto out = protowire::pxf::Marshal(*msg, opts);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_EQ(*out,
            "@type mapkeys.v1.Labels\n\nby_label = {\n  \".e\": \"leading dot\"\n"
            "  \"1.5\": \"float-shaped\"\n  a.b: \"quoted dotted\"\n  c.d: \"bare dotted\"\n}\n");
}

TEST_F(MapKeys, FmtBareKeysPair) {
  const std::string expected = ReadFixture("fmt-bare-keys.expected.pxf");
  EXPECT_EQ(Format(ReadFixture("fmt-bare-keys.pxf")), expected);
  EXPECT_EQ(Format(expected), expected) << "expected file is not a fixed point";
}

// A MapEntry built in code carries no document spelling: bare when the key
// lexes as one bare map-key token, quoted otherwise, so "" and "my key"
// never produce a document that does not parse.
TEST_F(MapKeys, FormatEntryBuiltInCode) {
  using protowire::pxf::Assignment;
  using protowire::pxf::BlockVal;
  using protowire::pxf::Document;
  using protowire::pxf::MapEntry;
  using protowire::pxf::StringVal;
  Document doc;
  auto block = std::make_unique<BlockVal>();
  for (const char* key : {"plain", "true", "123", "", "my key", "null", "a.b"}) {
    auto m = std::make_unique<MapEntry>();
    m->key = key;
    auto v = std::make_unique<StringVal>();
    v->value = "v";
    m->value = std::move(v);
    block->entries.emplace_back(std::move(m));
  }
  auto a = std::make_unique<Assignment>();
  a->key = "by_label";
  a->value = std::move(block);
  doc.entries.emplace_back(std::move(a));
  EXPECT_EQ(protowire::pxf::FormatDocument(doc),
            "by_label = {\n"
            "  plain: \"v\"\n"
            "  true: \"v\"\n"
            "  123: \"v\"\n"
            "  \"\": \"v\"\n"
            "  \"my key\": \"v\"\n"
            "  \"null\": \"v\"\n"
            "  a.b: \"v\"\n"
            "}\n");
}

// --- bind rules --------------------------------------------------------------

TEST_F(MapKeys, KeywordKeysBindToSixStringKeys) {
  protowire::Status st;
  auto got = Bind(labels_, ReadFixture("fmt-keyword-keys.pxf"), &st);
  ASSERT_TRUE(st.ok()) << st.ToString();
  std::map<std::string, std::string> want = {
      {"true", "quoted keyword"},
      {"false", "quoted keyword"},
      {"null", "quoted keyword"},
      {"123", "quoted integer"},
      {"plain", "quoted identifier-safe key"},
      {"bare", "bare identifier"},
  };
  EXPECT_EQ(got, want);
}

TEST_F(MapKeys, BareKeywordOnStringKeyIsAnError) {
  protowire::Status st;
  Bind(labels_, "by_label = { true: \"v\" }", &st);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("the keyword true is a bool key"), std::string::npos)
      << st.ToString();
}

TEST_F(MapKeys, BoolKeysBindInEverySpelling) {
  for (const char* f :
       {"bool-keys.pxf", "bool-keys-keyword.pxf", "bool-keys-integer.pxf", "fmt-bare-keys.pxf"}) {
    SCOPED_TRACE(f);
    protowire::Status st;
    auto got = Bind(flags_, ReadFixture(f), &st);
    ASSERT_TRUE(st.ok()) << st.ToString();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_TRUE(got.count("true"));
    EXPECT_TRUE(got.count("false"));
  }
}

TEST_F(MapKeys, InvalidBoolKeySpellingsAreRejected) {
  int n = 0;
  for (const auto& entry : std::filesystem::directory_iterator(kDir + "invalid")) {
    const std::string name = "invalid/" + entry.path().filename().string();
    SCOPED_TRACE(name);
    protowire::Status st;
    Bind(flags_, ReadFixture(name), &st);
    EXPECT_FALSE(st.ok()) << name << " bound";
    if (!st.ok()) {
      EXPECT_NE(st.message().find("by_flag"), std::string::npos) << st.ToString();
    }
    ++n;
  }
  EXPECT_EQ(n, 13) << "fixture count drifted from the spec repo";
}

// --- marshaller --------------------------------------------------------------

TEST_F(MapKeys, MarshalQuotesKeywordAndIntegerStringKeys) {
  auto msg = New(labels_);
  const auto* fd = labels_->field(0);
  const auto* r = msg->GetReflection();
  const auto* key_fd = fd->message_type()->map_key();
  const auto* val_fd = fd->message_type()->map_value();
  for (const char* key : {"true", "false", "null", "123", "plain", "", "my key", "_ok9"}) {
    auto* e = r->AddMessage(msg.get(), fd);
    e->GetReflection()->SetString(e, key_fd, key);
    e->GetReflection()->SetString(e, val_fd, "v");
  }
  auto text = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(text.ok()) << text.status().ToString();
  EXPECT_EQ(*text,
            "by_label = {\n"
            "  \"\": \"v\"\n"
            "  \"123\": \"v\"\n"
            "  _ok9: \"v\"\n"
            "  \"false\": \"v\"\n"
            "  \"my key\": \"v\"\n"
            "  \"null\": \"v\"\n"
            "  plain: \"v\"\n"
            "  \"true\": \"v\"\n"
            "}\n");
  // What the marshaller writes, the decoder reads back to the same keys,
  // and the formatter leaves alone.
  protowire::Status st;
  auto got = Bind(labels_, *text, &st);
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(got.size(), 8u);
  EXPECT_EQ(Format(*text), *text);
}

TEST_F(MapKeys, MarshalWritesBoolKeysBare) {
  auto msg = New(flags_);
  const auto* fd = flags_->field(0);
  const auto* r = msg->GetReflection();
  for (bool b : {true, false}) {
    auto* e = r->AddMessage(msg.get(), fd);
    e->GetReflection()->SetBool(e, fd->message_type()->map_key(), b);
    e->GetReflection()->SetString(e, fd->message_type()->map_value(), b ? "t" : "f");
  }
  auto text = protowire::pxf::Marshal(*msg);
  ASSERT_TRUE(text.ok());
  EXPECT_EQ(*text, "by_flag = {\n  false: \"f\"\n  true: \"t\"\n}\n");
  EXPECT_EQ(Format(*text), *text);
}

}  // namespace

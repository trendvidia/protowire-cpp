// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// What protowire-go does that a library detail could easily undo here. The
// differential run (scripts/go_differential.sh) compares the two ports over a
// corpus; these keep the sharpest cases in this repo's own tests.

#include "protowire/detail/base64.h"
#include "protowire/detail/go_quote.h"
#include "protowire/detail/rfc3339.h"
#include "protowire/pxf.h"
#include "protowire/pxf/go_wire.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;
using protowire::Status;
using protowire::detail::GoIllegalRune;
using protowire::detail::GoQuote;

class CollectErrors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

class GoParity : public ::testing::Test {
 protected:
  void SetUp() override {
    tree_.MapPath("", WKT_PROTO_DIR);
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", std::string(TESTDATA_DIR) + "/differential");
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    ASSERT_NE(importer_->Import("diff.proto"), nullptr) << errors_.last_;
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
  }

  std::unique_ptr<pb::Message> New(const char* name) {
    const pb::Descriptor* d = importer_->pool()->FindMessageTypeByName(name);
    EXPECT_NE(d, nullptr) << name;
    return std::unique_ptr<pb::Message>(factory_->GetPrototype(d)->New());
  }

  // Marshal of a document.
  std::string RoundTrip(const char* name, const std::string& text) {
    auto msg = New(name);
    Status st = pxf::Unmarshal(text, msg.get());
    EXPECT_TRUE(st.ok()) << st.ToString();
    auto out = pxf::Marshal(*msg);
    EXPECT_TRUE(out.ok());
    return out.ok() ? *out : "";
  }

  pb::compiler::DiskSourceTree tree_;
  CollectErrors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
};

TEST(GoQuoteTest, MatchesStrconvQuote) {
  EXPECT_EQ(GoQuote("a\"b\\c"), R"("a\"b\\c")");
  EXPECT_EQ(GoQuote("\a\b\f\n\r\t\v"), R"("\a\b\f\n\r\t\v")");
  EXPECT_EQ(GoQuote(std::string_view("\x00\x1f\x7f", 3)), R"("\x00\x1f\x7f")");
  EXPECT_EQ(GoQuote("é日😀"), "\"é日😀\"");
  EXPECT_EQ(GoQuote("\xC2\xA0"), "\"\\u00a0\"");              // no-break space
  EXPECT_EQ(GoQuote("\xE2\x80\x8B"), "\"\\u200b\"");          // zero-width space
  EXPECT_EQ(GoQuote("\xF3\xA0\x80\x81"), "\"\\U000e0001\"");  // language tag
  EXPECT_EQ(GoQuote("\xEF\xBF\xBD"), "\"\xEF\xBF\xBD\"");     // U+FFFD itself
  EXPECT_EQ(GoQuote("\xFF"), R"("\xff")");
  EXPECT_EQ(GoQuote("\xC0\xAF"), R"("\xc0\xaf")");          // overlong
  EXPECT_EQ(GoQuote("\xED\xA0\x80"), R"("\xed\xa0\x80")");  // surrogate
  EXPECT_EQ(GoQuote("\xE2\x82"), R"("\xe2\x82")");          // truncated
}

TEST(GoQuoteTest, IllegalRuneNamesTheCharacter) {
  size_t width = 0;
  EXPECT_EQ(GoIllegalRune(";x", &width), "unexpected character ';'");
  EXPECT_EQ(width, 1u);
  EXPECT_EQ(GoIllegalRune("'", &width), R"(unexpected character '\'')");
  EXPECT_EQ(GoIllegalRune("μs", &width), "unexpected character 'μ'");
  EXPECT_EQ(width, 2u);
  EXPECT_EQ(GoIllegalRune("\xFF", &width), "unexpected byte 0xff");
  EXPECT_EQ(width, 1u);
}

TEST(GoBase64, LenientTakesEveryAlphabet) {
  size_t offset = 0;
  using protowire::detail::Base64DecodeLenient;
  EXPECT_EQ(Base64DecodeLenient("AQI=", &offset).value(), (std::vector<uint8_t>{1, 2}));
  EXPECT_EQ(Base64DecodeLenient("AQI", &offset).value(), (std::vector<uint8_t>{1, 2}));
  EXPECT_EQ(Base64DecodeLenient("-_-_", &offset).value(), (std::vector<uint8_t>{0xfb, 0xff, 0xbf}));
  EXPECT_EQ(Base64DecodeLenient("-_8", &offset).value(), (std::vector<uint8_t>{0xfb, 0xff}));
  EXPECT_FALSE(Base64DecodeLenient("a$b=", &offset).has_value());
  EXPECT_EQ(offset, 1u);
}

TEST(GoTimestamp, RefusesWhatTimeParseRefuses) {
  using protowire::detail::ParseRFC3339;
  EXPECT_FALSE(ParseRFC3339("2026-02-30T00:00:00Z").has_value());
  EXPECT_FALSE(ParseRFC3339("2025-02-29T00:00:00Z").has_value());
  EXPECT_TRUE(ParseRFC3339("2024-02-29T00:00:00Z").has_value());
  EXPECT_FALSE(ParseRFC3339("2026-06-30T23:59:60Z").has_value());
}

TEST_F(GoParity, FloatsAreShortest) {
  EXPECT_EQ(RoundTrip("diff.v1.Numbers", "f = 0.1\nd = 0.1\n"), "f = 0.1\nd = 0.1\n");
  EXPECT_EQ(RoundTrip("diff.v1.Numbers", "d = 100000\n"), "d = 100000\n");
  EXPECT_EQ(RoundTrip("diff.v1.Numbers", "d = 1000000\n"), "d = 1e+06\n");
  EXPECT_EQ(RoundTrip("diff.v1.Numbers", "d = 0.0001\n"), "d = 0.0001\n");
  EXPECT_EQ(RoundTrip("diff.v1.Numbers", "d = 0.00001\n"), "d = 1e-05\n");
  EXPECT_EQ(RoundTrip("diff.v1.Numbers", "d = 1234567\n"), "d = 1.234567e+06\n");
  EXPECT_EQ(RoundTrip("diff.v1.Numbers", "d = -inf\n"), "d = -inf\n");
  EXPECT_EQ(RoundTrip("diff.v1.Numbers", "d = nan\n"), "d = nan\n");
}

TEST_F(GoParity, MarshalLayout) {
  EXPECT_EQ(RoundTrip("diff.v1.Root", "leaf { after = \"x\" }\nstrs = [\"a\", \"b\"]\n"),
            "leaf {\n  after = \"x\"\n}\nstrs = [\n  \"a\",\n  \"b\"\n]\n");
  EXPECT_EQ(RoundTrip("diff.v1.Root", "by_num = { 10: {} 9: {} }\n"),
            "by_num = {\n  10: {\n  }\n  9: {\n  }\n}\n");
  EXPECT_EQ(RoundTrip("diff.v1.Root", "color = 5\n"), "color = 5\n");
}

TEST_F(GoParity, EmitDefaultsEndsOnARecursiveMessage) {
  auto msg = New("diff.v1.Leaf");
  pxf::MarshalOptions opts;
  opts.emit_defaults = true;
  auto out = pxf::Marshal(*msg, opts);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_EQ(out->find("child"), std::string::npos) << *out;
}

TEST_F(GoParity, NullsAtEveryLevel) {
  const std::string text =
      "leaf { label = null child { wrapped = null } }\n"
      "items = [ { wrapped = null } ]\n"
      "by_key = { a: { label = null } }\n";
  // Plain has no _null mask, so the Result carries them.
  auto msg = New("diff.v1.Plain");
  auto r = pxf::UnmarshalFull("leaf { label = null }\nitems = [ { wrapped = null } ]\n", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  pxf::MarshalOptions opts;
  opts.null_fields = &*r;
  auto out = pxf::Marshal(*msg, opts);
  ASSERT_TRUE(out.ok());
  EXPECT_EQ(*out, "leaf {\n  label = null\n}\nitems = [\n  {\n    wrapped = null\n  }\n]\n");
  // In Root's own mask, only paths outside elements are recorded.
  auto root = New("diff.v1.Root");
  auto full = pxf::UnmarshalFull(text, root.get());
  ASSERT_TRUE(full.ok()) << full.status().ToString();
  auto written = pxf::Marshal(*root);
  ASSERT_TRUE(written.ok());
  EXPECT_NE(written->find("label = null"), std::string::npos) << *written;
  EXPECT_NE(written->find("child {\n    wrapped = null"), std::string::npos) << *written;
}

TEST_F(GoParity, PresenceNamesWhatALiteralSets) {
  auto msg = New("diff.v1.Root");
  auto r = pxf::UnmarshalFull("leaf { wrapped = \"w\" at = 2026-01-01T00:00:00Z }\n", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->IsSet("leaf.wrapped.value"));
  EXPECT_TRUE(r->IsSet("leaf.at.seconds"));
  EXPECT_TRUE(r->IsSet("leaf.at.nanos"));
}

TEST_F(GoParity, OneofMembersSerializeLast) {
  auto msg = New("diff.v1.Leaf");
  ASSERT_TRUE(pxf::Unmarshal("number = 3\nafter = \"y\"\n", msg.get()).ok());
  // after (field 7) before number (field 6): libprotobuf has it the other way.
  EXPECT_EQ(pxf::SerializeGoOrder(*msg), std::string("\x3a\x01y\x30\x03", 5));
  auto copy = New("diff.v1.Leaf");
  ASSERT_TRUE(copy->ParseFromString(pxf::SerializeGoOrder(*msg)));
  EXPECT_EQ(copy->ShortDebugString(), msg->ShortDebugString());
}

TEST_F(GoParity, SecretHookTakesEveryForm) {
  std::vector<std::pair<std::string, std::string>> got;
  pxf::UnmarshalOptions opts;
  opts.on_secret_field = [&](std::string_view path, std::string_view value) {
    got.emplace_back(path, value);
    return Status::OK();
  };
  auto msg = New("diff.v1.Vault");
  auto r = pxf::UnmarshalFull(
      "must = \"m\"\n"
      "pw { value = \"a\" hint = \"h\" }\n"
      "keys = [\"b\", { value = \"c\" }]\n"
      "tenant = { acme: \"d\" }\n"
      "nested = [ { must = \"n\" pw = \"e\" } ]\n",
      msg.get(),
      opts);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(got,
            (std::vector<std::pair<std::string, std::string>>{{"pw", "a"},
                                                              {"keys[0]", "b"},
                                                              {"keys[1]", "c"},
                                                              {"tenant[\"acme\"]", "d"},
                                                              {"nested[0].pw", "e"}}));
  EXPECT_TRUE(r->IsSet("pw.value"));
  EXPECT_TRUE(r->IsSet("pw.hint"));
  // The value never reached the message; the hint did.
  EXPECT_EQ(msg->ShortDebugString().find("\"a\""), std::string::npos) << msg->ShortDebugString();
  EXPECT_NE(msg->ShortDebugString().find("hint"), std::string::npos) << msg->ShortDebugString();
}

TEST_F(GoParity, SecretHookErrorAbortsTheDecode) {
  pxf::UnmarshalOptions opts;
  opts.on_secret_field = [](std::string_view, std::string_view) {
    return Status::Error("vault is sealed");
  };
  auto msg = New("diff.v1.Vault");
  Status st = pxf::Unmarshal("pw = \"x\"\n", msg.get(), opts);
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.message(), "pxf.Secret hook for \"pw\": vault is sealed");
  EXPECT_EQ(st.position()->first, 1);
}

TEST_F(GoParity, SkipPostDecodeLeavesTheRulesToTheCaller) {
  auto msg = New("diff.v1.Vault");
  EXPECT_FALSE(pxf::UnmarshalFull("pw = \"x\"\n", msg.get()).ok()) << "must is required";
  pxf::UnmarshalOptions opts;
  opts.skip_post_decode = true;
  auto skipped = New("diff.v1.Vault");
  auto r = pxf::UnmarshalFull("pw = \"x\"\n", skipped.get(), opts);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->IsAbsent("label"));
  EXPECT_EQ(skipped->ShortDebugString().find("unnamed"), std::string::npos)
      << "the default must stay unapplied";
}

}  // namespace

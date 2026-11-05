// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// A template id is unique only within a schema (draft -01 § SBE
// Annotations). Ported from protowire-go's encoding/sbe/template_id_test.go
// (protowire-go#253, #257).

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "protowire/sbe.h"
#include "sbe_files.h"

namespace {

namespace pb = google::protobuf;
using protowire::sbe::Codec;
using protowire_test::SbeFiles;

// IdFiles compiles one file per body, a.proto, b.proto, ..., each in its own
// package ids.a, ids.b, ... and with the schema id given for it.
bool IdFiles(SbeFiles* files,
             const std::vector<int>& schema_ids,
             const std::vector<std::string>& bodies) {
  std::vector<std::pair<std::string, std::string>> srcs;
  for (size_t i = 0; i < bodies.size(); ++i) {
    const char c = static_cast<char>('a' + i);
    srcs.emplace_back(std::string(1, c) + ".proto",
                      "syntax = \"proto3\";\npackage ids." + std::string(1, c) +
                          ";\nimport \"sbe/annotations.proto\";\noption (sbe.schema_id) = " +
                          std::to_string(schema_ids[i]) + ";\noption (sbe.version) = 0;\n" +
                          bodies[i] + "\n");
  }
  return files->Compile(srcs);
}

uint16_t Le16(const std::vector<uint8_t>& b, size_t at) {
  return static_cast<uint16_t>(b[at] | (b[at + 1] << 8U));
}

// TestTemplateIDUniqueInSchema: "sbe.template_id MUST be unique among
// messages within a schema". Before the fix Codec::New bound both messages,
// the second replaced the first in the id lookup, and NewView read A's bytes
// as B's.
TEST(SbeTemplateID, UniqueInSchema) {
  struct Case {
    const char* name;
    std::vector<int> schema_ids;
    std::vector<std::string> bodies;
    const char* reject;  // empty: binds
  };
  const std::vector<Case> cases = {
      {"two messages in one file",
       {1},
       {"message A { option (sbe.template_id) = 7; uint32 x = 1; } message B { option "
        "(sbe.template_id) = 7; uint64 y = 1; }"},
       "sbe: messages ids.a.A and ids.a.B share (sbe.template_id) = 7 in schema 1; it must be "
       "unique within a schema"},
      {"two files of one schema",
       {1, 1},
       {"message A { option (sbe.template_id) = 7; uint32 x = 1; }",
        "message B { option (sbe.template_id) = 7; uint64 y = 1; }"},
       "sbe: messages ids.a.A and ids.b.B share (sbe.template_id) = 7 in schema 1"},
      {"a nested message",
       {1},
       {"message A { option (sbe.template_id) = 7; uint32 x = 1; message N { option "
        "(sbe.template_id) = 7; uint32 z = 1; } }"},
       "sbe: messages ids.a.A and ids.a.A.N share (sbe.template_id) = 7 in schema 1"},
      {"distinct ids",
       {1},
       {"message A { option (sbe.template_id) = 7; uint32 x = 1; } message B { option "
        "(sbe.template_id) = 8; uint64 y = 1; }"},
       ""},
  };
  for (const Case& tc : cases) {
    SCOPED_TRACE(tc.name);
    SbeFiles files;
    if (!IdFiles(&files, tc.schema_ids, tc.bodies)) {
      ADD_FAILURE() << files.error();
      continue;
    }
    auto codec = Codec::New(files.files());
    if (std::string(tc.reject).empty()) {
      EXPECT_TRUE(codec.ok()) << codec.status().ToString();
      continue;
    }
    if (codec.ok()) {
      ADD_FAILURE() << "bound; want " << tc.reject;
      continue;
    }
    EXPECT_NE(codec.status().message().find(tc.reject), std::string::npos)
        << codec.status().message();
  }
}

// TestTemplateIDSameFileTwice: a file passed twice registers the same
// messages again, which is not a duplicate.
TEST(SbeTemplateID, SameFileTwice) {
  SbeFiles files;
  ASSERT_TRUE(IdFiles(&files, {1}, {"message A { option (sbe.template_id) = 7; uint32 x = 1; }"}))
      << files.error();
  const pb::FileDescriptor* fd = files.files()[0];
  auto codec = Codec::New({fd, fd});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();

  const pb::Descriptor* md = files.Message("ids.a.A");
  auto msg = files.New(md);
  msg->GetReflection()->SetUInt32(msg.get(), md->FindFieldByName("x"), 42);
  auto data = codec->Marshal(*msg);
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  auto v = codec->NewView(*data);
  ASSERT_TRUE(v.ok()) << v.status().ToString();
  EXPECT_EQ(v->Uint("x"), 42u);
}

// TestTemplateIDAcrossSchemas: two schemas may each use template 7 and both
// bind. NewView picks the template from the header's schemaId and templateId
// together. Before, it looked the id up alone and found whichever message
// was registered last.
TEST(SbeTemplateID, AcrossSchemas) {
  SbeFiles files;
  ASSERT_TRUE(IdFiles(&files,
                      {1, 2},
                      {"message A { option (sbe.template_id) = 7; uint32 x = 1; message Only1 { "
                       "option (sbe.template_id) = 9; uint32 z = 1; } }",
                       "message B { option (sbe.template_id) = 7; uint64 y = 1; }"}))
      << files.error();
  const pb::Descriptor* md_a = files.Message("ids.a.A");
  const pb::Descriptor* md_b = files.Message("ids.b.B");
  auto a = files.New(md_a);
  a->GetReflection()->SetUInt32(a.get(), md_a->FindFieldByName("x"), 0xA);
  auto b = files.New(md_b);
  b->GetReflection()->SetUInt64(b.get(), md_b->FindFieldByName("y"), 0xB);

  // Registration order does not matter: each schema's bytes reach their own
  // message whichever file came last.
  const std::vector<std::vector<const pb::FileDescriptor*>> orders = {
      files.files(), {files.files()[1], files.files()[0]}};
  for (const auto& order : orders) {
    auto codec = Codec::New(order);
    ASSERT_TRUE(codec.ok()) << "two schemas may each use template 7: " << codec.status().ToString();
    auto data_a = codec->Marshal(*a);
    ASSERT_TRUE(data_a.ok()) << data_a.status().ToString();
    auto data_b = codec->Marshal(*b);
    ASSERT_TRUE(data_b.ok()) << data_b.status().ToString();
    EXPECT_EQ(Le16(*data_a, 4), 1) << "A's header names schema 1";
    EXPECT_EQ(Le16(*data_b, 4), 2) << "B's header names schema 2";

    auto v = codec->NewView(*data_a);
    ASSERT_TRUE(v.ok()) << v.status().ToString();
    EXPECT_EQ(v->Uint("x"), 0xAu);
    v = codec->NewView(*data_b);
    ASSERT_TRUE(v.ok()) << v.status().ToString();
    EXPECT_EQ(v->Uint("y"), 0xBu);
  }

  // Template 9 exists in schema 1 only. A header naming it in schema 2 is
  // refused, where an id-only lookup would have found it.
  auto codec = Codec::New(files.files());
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  auto only1 = files.New(files.Message("ids.a.A.Only1"));
  auto data9 = codec->Marshal(*only1);
  ASSERT_TRUE(data9.ok()) << data9.status().ToString();
  EXPECT_TRUE(codec->NewView(*data9).ok());
  (*data9)[4] = 2;
  auto v = codec->NewView(*data9);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(v.status().message(), "sbe: unknown template ID 9 in schema 2");
}

// TestUnmarshalChecksSchemaID: Unmarshal refuses bytes whose header names
// another schema than the target message's, even when the template id
// matches. A's block is no shorter than B's, so before the check A's bytes
// decoded as B with no error, B.y holding the low half of A.x.
TEST(SbeTemplateID, UnmarshalChecksSchemaID) {
  SbeFiles files;
  ASSERT_TRUE(IdFiles(&files,
                      {1, 2},
                      {"message A { option (sbe.template_id) = 7; uint64 x = 1; }",
                       "message B { option (sbe.template_id) = 7; uint32 y = 1; }"}))
      << files.error();
  const pb::Descriptor* md_a = files.Message("ids.a.A");
  const pb::Descriptor* md_b = files.Message("ids.b.B");
  auto codec = Codec::New(files.files());
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();

  auto a = files.New(md_a);
  a->GetReflection()->SetUInt64(a.get(), md_a->FindFieldByName("x"), 0xA0000000AULL);
  auto data_a = codec->Marshal(*a);
  ASSERT_TRUE(data_a.ok()) << data_a.status().ToString();

  const std::string want = "sbe: schema ID mismatch: got 1, want 2 for template 7";
  auto into_b = files.New(md_b);
  EXPECT_EQ(codec->Unmarshal(*data_a, into_b.get()).message(), want);
  auto got = codec->UnmarshalDescriptor(*data_a, md_b);
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.status().message(), want);

  // Its own message still reads it, and so does B once the header names
  // schema 2: the check is the header's schema id, not the layout.
  got = codec->UnmarshalDescriptor(*data_a, md_a);
  ASSERT_TRUE(got.ok()) << got.status().ToString();
  EXPECT_EQ((*got)->GetReflection()->GetUInt64(**got, md_a->FindFieldByName("x")), 0xA0000000AULL);
  (*data_a)[4] = 2;
  got = codec->UnmarshalDescriptor(*data_a, md_b);
  ASSERT_TRUE(got.ok()) << got.status().ToString();
  EXPECT_EQ((*got)->GetReflection()->GetUInt32(**got, md_b->FindFieldByName("y")), 0xAu);

  // A header with schema id 0, as an emitter that does not set it writes, is
  // refused for a schema that declares another id.
  (*data_a)[4] = 0;
  auto into_a = files.New(md_a);
  EXPECT_EQ(codec->Unmarshal(*data_a, into_a.get()).message(),
            "sbe: schema ID mismatch: got 0, want 1 for template 7");
}

}  // namespace

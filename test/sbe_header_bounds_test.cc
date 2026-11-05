// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Every SBE header field is a uint16, so each value a schema declares, and
// each block it lays out, must fit in 65535. Ported from protowire-go's
// encoding/sbe/header_bounds_test.go (protowire-go#250).

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protowire/sbe.h"
#include "sbe_files.h"

namespace {

namespace pb = google::protobuf;
using protowire::sbe::Codec;
using protowire_test::SbeFiles;

bool BoundsFile(SbeFiles* files,
                const std::string& schema_id,
                const std::string& version,
                const std::string& body) {
  return files->Compile(
      {{"bounds.proto",
        "syntax = \"proto3\";\npackage bounds.v1;\nimport \"sbe/annotations.proto\";\noption "
        "(sbe.schema_id) = " +
            schema_id + ";\noption (sbe.version) = " + version + ";\n" + body + "\n"}});
}

uint16_t Le16(const std::vector<uint8_t>& b, size_t at) {
  return static_cast<uint16_t>(b[at] | (b[at + 1] << 8U));
}

// TestNewCodecHeaderBounds pairs each schema the SBE header cannot describe
// with the same shape at the bound, which must bind and round-trip.
TEST(SbeHeaderBounds, NewCodec) {
  struct Case {
    const char* name;
    std::string schema_id;
    std::string version;
    std::string body;
    std::string reject;       // a substring of the error; empty means it must bind
    uint16_t block_size = 0;  // the root blockLength when it binds
  };
  const std::vector<Case> cases = {
      {"length past 65535",
       "",
       "",
       "message M { option (sbe.template_id) = 1; bytes b = 1 [(sbe.length) = 65537]; uint32 t = "
       "2; }",
       "field bounds.v1.M.b: (sbe.length) = 65537 is past 65535"},
      {"length at 65535",
       "",
       "",
       "message M { option (sbe.template_id) = 1; bytes b = 1 [(sbe.length) = 65535]; }",
       "",
       65535},
      {"string length past 65535",
       "",
       "",
       "message M { option (sbe.template_id) = 1; string s = 1 [(sbe.length) = 70000]; }",
       "field bounds.v1.M.s: (sbe.length) = 70000 is past 65535"},
      {"root block past 65535",
       "",
       "",
       "message M { option (sbe.template_id) = 1; bytes a = 1 [(sbe.length) = 30000]; bytes b = "
       "2 [(sbe.length) = 30000]; bytes c = 3 [(sbe.length) = 30000]; uint32 t = 4; }",
       "field bounds.v1.M.c: ends at byte 90000 of its block, past 65535"},
      {"root block one byte past 65535",
       "",
       "",
       "message M { option (sbe.template_id) = 1; bytes a = 1 [(sbe.length) = 65532]; uint32 t = "
       "2; }",
       "field bounds.v1.M.t: ends at byte 65536 of its block"},
      {"root block at 65535",
       "",
       "",
       "message M { option (sbe.template_id) = 1; bytes a = 1 [(sbe.length) = 65531]; uint32 t = "
       "2; }",
       "",
       65535},
      {"group block past 65535",
       "",
       "",
       "message E { bytes a = 1 [(sbe.length) = 40000]; bytes b = 2 [(sbe.length) = 40000]; } "
       "message M { option (sbe.template_id) = 1; repeated E e = 1; }",
       "group field bounds.v1.E.b: ends at byte 80000 of its block"},
      {"group block at 65535",
       "",
       "",
       "message E { bytes a = 1 [(sbe.length) = 40000]; bytes b = 2 [(sbe.length) = 25535]; } "
       "message M { option (sbe.template_id) = 1; repeated E e = 1; }",
       ""},
      {"composite past 65535",
       "",
       "",
       "message C { bytes a = 1 [(sbe.length) = 40000]; bytes b = 2 [(sbe.length) = 40000]; } "
       "message M { option (sbe.template_id) = 1; C c = 1; }",
       "composite bounds.v1.M.c: composite field bounds.v1.C.b: ends at byte 80000"},
      {"composite fits, block past 65535",
       "",
       "",
       "message C { bytes a = 1 [(sbe.length) = 40000]; } message M { option (sbe.template_id) = "
       "1; C c = 1; C d = 2; }",
       "composite bounds.v1.M.d: ends at byte 80000"},
      {"composite in group past 65535",
       "",
       "",
       "message C { bytes a = 1 [(sbe.length) = 40000]; } message E { C c = 1; C d = 2; } message "
       "M { option (sbe.template_id) = 1; repeated E e = 1; }",
       "composite in group bounds.v1.E.d: ends at byte 80000"},
      {"nested composite past 65535",
       "",
       "",
       "message D { bytes a = 1 [(sbe.length) = 40000]; } message C { D x = 1; D y = 2; } message "
       "M { option (sbe.template_id) = 1; C c = 1; }",
       "composite field bounds.v1.C.y: ends at byte 80000"},
      {"template_id past 65535",
       "",
       "",
       "message M { option (sbe.template_id) = 65537; uint32 t = 1; }",
       "message bounds.v1.M: (sbe.template_id) = 65537 is past 65535"},
      {"template_id at 65535",
       "",
       "",
       "message M { option (sbe.template_id) = 65535; uint32 t = 1; }",
       "",
       4},
      {"schema_id past 65535",
       "65536",
       "",
       "message M { option (sbe.template_id) = 1; uint32 t = 1; }",
       "file bounds.proto: (sbe.schema_id) = 65536 is past 65535"},
      {"schema_id at 65535",
       "65535",
       "",
       "message M { option (sbe.template_id) = 1; uint32 t = 1; }",
       "",
       4},
      {"version past 65535",
       "",
       "65536",
       "message M { option (sbe.template_id) = 1; uint32 t = 1; }",
       "file bounds.proto: (sbe.version) = 65536 is past 65535"},
      {"version at 65535",
       "",
       "65535",
       "message M { option (sbe.template_id) = 1; uint32 t = 1; }",
       "",
       4},
  };
  for (const Case& tc : cases) {
    SCOPED_TRACE(tc.name);
    const std::string schema_id = tc.schema_id.empty() ? "1" : tc.schema_id;
    const std::string version = tc.version.empty() ? "0" : tc.version;
    SbeFiles files;
    if (!BoundsFile(&files, schema_id, version, tc.body)) {
      ADD_FAILURE() << files.error();
      continue;
    }
    auto codec = Codec::New(files.files());
    if (!tc.reject.empty()) {
      if (codec.ok()) {
        ADD_FAILURE() << "bound; want " << tc.reject;
        continue;
      }
      EXPECT_NE(codec.status().message().find(tc.reject), std::string::npos)
          << codec.status().message();
      continue;
    }
    if (!codec.ok()) {
      ADD_FAILURE() << codec.status().ToString();
      continue;
    }

    const pb::Descriptor* md = files.Message("bounds.v1.M");
    auto msg = files.New(md);
    if (const pb::FieldDescriptor* e = md->FindFieldByName("e"); e != nullptr) {
      msg->GetReflection()->AddMessage(msg.get(), e);
    }
    auto data = codec->Marshal(*msg);
    if (!data.ok()) {
      ADD_FAILURE() << data.status().ToString();
      continue;
    }

    // The header states exactly what the schema declared.
    if (data->size() < 8) {
      ADD_FAILURE() << "short header";
      continue;
    }
    if (tc.block_size != 0) EXPECT_EQ(Le16(*data, 0), tc.block_size) << "blockLength";
    EXPECT_EQ(std::to_string(Le16(*data, 4)), schema_id) << "schemaId";
    EXPECT_EQ(std::to_string(Le16(*data, 6)), version) << "version";

    auto back = codec->UnmarshalDescriptor(*data, md);
    EXPECT_TRUE(back.ok()) << back.status().ToString();
  }
}

// TestMarshalGroupCountBound: numInGroup is a uint16. Before the fix 65536
// entries were written with a count of 0, and the message read back with no
// entries and no error.
TEST(SbeHeaderBounds, MarshalGroupCount) {
  SbeFiles files;
  ASSERT_TRUE(BoundsFile(
      &files,
      "1",
      "0",
      "message E { uint32 a = 1; } message M { option (sbe.template_id) = 1; repeated E e = 1; }"))
      << files.error();
  auto codec = Codec::New(files.files());
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  const pb::Descriptor* md = files.Message("bounds.v1.M");
  const pb::FieldDescriptor* efd = md->FindFieldByName("e");

  const auto with_entries = [&](int n) {
    auto msg = files.New(md);
    for (int i = 0; i < n; ++i) msg->GetReflection()->AddMessage(msg.get(), efd);
    return msg;
  };

  auto data = codec->Marshal(*with_entries(65535));
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  EXPECT_EQ(Le16(*data, 8 + 2), 65535) << "numInGroup";
  auto back = codec->UnmarshalDescriptor(*data, md);
  ASSERT_TRUE(back.ok()) << back.status().ToString();
  EXPECT_EQ((*back)->GetReflection()->FieldSize(**back, efd), 65535);

  auto past = codec->Marshal(*with_entries(65536));
  ASSERT_FALSE(past.ok());
  EXPECT_NE(past.status().message().find("field bounds.v1.M.e has 65536 entries, past 65535"),
            std::string::npos)
      << past.status().message();
}

}  // namespace

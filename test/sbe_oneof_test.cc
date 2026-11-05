// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// A oneof has no place in an SBE block, so Codec::New refuses a field in a
// real oneof in a message or a composite, as protowire-go's template.go
// does. A proto3 optional field sits in a synthetic oneof and binds. A group
// entry has no such check in Go, so a oneof there binds too (#141).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "protowire/sbe.h"
#include "sbe_files.h"

namespace {

using protowire::sbe::Codec;
using protowire_test::SbeFiles;

TEST(SbeOneof, NewCodec) {
  struct Case {
    const char* name;
    std::string body;
    std::string reject;  // the whole error; empty means it must bind
  };
  const std::vector<Case> cases = {
      {"oneof in message",
       "message M { option (sbe.template_id) = 1; uint32 t = 1; oneof o { uint32 a = 2; uint64 "
       "b = 3; } }",
       "sbe: oneof field oneof.v1.M.a not supported"},
      {"oneof in composite",
       "message C { oneof o { uint32 a = 1; uint64 b = 2; } } message M { option "
       "(sbe.template_id) = 1; C c = 1; }",
       "sbe: composite oneof.v1.M.c: composite oneof.v1.C contains oneof field a"},
      {"oneof in nested composite",
       "message D { oneof o { uint32 a = 1; } } message C { D d = 1; } message M { option "
       "(sbe.template_id) = 1; C c = 1; }",
       "sbe: composite oneof.v1.M.c: composite oneof.v1.D contains oneof field a"},
      {"oneof in composite in group",
       "message C { oneof o { uint32 a = 1; } } message E { C c = 1; } message M { option "
       "(sbe.template_id) = 1; repeated E e = 1; }",
       "sbe: composite in group oneof.v1.E.c: composite oneof.v1.C contains oneof field a"},
      {"oneof in group entry binds",
       "message E { oneof o { uint32 a = 1; uint64 b = 2; } } message M { option "
       "(sbe.template_id) = 1; repeated E e = 1; }",
       ""},
      {"proto3 optional in message binds",
       "message M { option (sbe.template_id) = 1; optional uint32 a = 1; }",
       ""},
      {"proto3 optional in composite binds",
       "message C { optional uint32 a = 1; } message M { option (sbe.template_id) = 1; C c = 1; }",
       ""},
  };
  for (const Case& tc : cases) {
    SCOPED_TRACE(tc.name);
    SbeFiles files;
    if (!files.Compile({{"oneof.proto",
                         "syntax = \"proto3\";\npackage oneof.v1;\nimport "
                         "\"sbe/annotations.proto\";\noption (sbe.schema_id) = 1;\n" +
                             tc.body + "\n"}})) {
      ADD_FAILURE() << files.error();
      continue;
    }
    auto codec = Codec::New(files.files());
    if (tc.reject.empty()) {
      EXPECT_TRUE(codec.ok()) << codec.status().ToString();
      continue;
    }
    if (codec.ok()) {
      ADD_FAILURE() << "bound; want " << tc.reject;
      continue;
    }
    EXPECT_EQ(codec.status().message(), tc.reject);
  }
}

}  // namespace

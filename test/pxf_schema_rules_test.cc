// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// The bind-time rules of draft -01 §annotation-extensions ("Default
// Placement", "Oneof Members") and the retired option numbers of STABILITY.md
// promise 3. Descriptors are built from raw option bytes, so each annotation
// rule runs in both spellings: the bracket options at 1314 / 1315, and the
// `@required` / `@default` carrier at 1327, which no protoc here can compile.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/unknown_field_set.h>

#include "protowire/pxf.h"
#include "protowire/pxf/schema.h"

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;
using pxf::Violation;
using pxf::ViolationKind;

std::string Varint(uint64_t v) {
  std::string out;
  while (v >= 0x80) {
    out.push_back(static_cast<char>(v | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
  return out;
}
std::string Bytes(int num, const std::string& v) {
  return Varint(uint64_t(num) << 3 | 2) + Varint(v.size()) + v;
}

enum class Spelling { kBracket, kCarrier };

void SetRequired(pb::FieldOptions* o, Spelling s) {
  auto* u = o->GetReflection()->MutableUnknownFields(o);
  if (s == Spelling::kBracket) {
    u->AddVarint(1314, 1);
  } else {
    u->AddLengthDelimited(1327, Bytes(1, Bytes(1, "protowire.schema.v1.required")));
  }
}

void SetDefault(pb::FieldOptions* o, Spelling s, const std::string& lit) {
  auto* u = o->GetReflection()->MutableUnknownFields(o);
  if (s == Spelling::kBracket) {
    u->AddLengthDelimited(1315, lit);
  } else {
    u->AddLengthDelimited(
        1327, Bytes(1, Bytes(1, "protowire.schema.v1.default") + Bytes(2, Bytes(10, lit))));
  }
}

pb::FieldDescriptorProto* AddField(pb::DescriptorProto* m,
                                   const std::string& name,
                                   pb::FieldDescriptorProto::Type type,
                                   const std::string& type_name = "") {
  auto* f = m->add_field();
  f->set_name(name);
  f->set_number(m->field_size());
  f->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
  f->set_type(type);
  if (!type_name.empty()) f->set_type_name(type_name);
  return f;
}

constexpr auto kStr = pb::FieldDescriptorProto::TYPE_STRING;
constexpr auto kMsg = pb::FieldDescriptorProto::TYPE_MESSAGE;

// Base is a file holding message M, with google/protobuf/timestamp.proto
// available to import.
pb::FileDescriptorProto Base(pb::DescriptorProto** m) {
  pb::FileDescriptorProto f;
  f.set_name("rules.proto");
  f.set_package("rules.v1");
  f.set_syntax("proto3");
  f.add_dependency("google/protobuf/timestamp.proto");
  *m = f.add_message_type();
  (*m)->set_name("M");
  return f;
}

class Pool {
 public:
  Pool() {
    pool_.AllowUnknownDependencies();
    pb::FileDescriptorProto ts;
    pb::Timestamp::descriptor()->file()->CopyTo(&ts);
    EXPECT_NE(pool_.BuildFile(ts), nullptr);
  }
  const pb::FileDescriptor* Build(const pb::FileDescriptorProto& f) {
    const pb::FileDescriptor* fd = pool_.BuildFile(f);
    EXPECT_NE(fd, nullptr);
    return fd;
  }

 private:
  pb::DescriptorPool pool_;
};

// Of returns the violations of one kind.
std::vector<Violation> Of(const pb::FileDescriptor* fd, ViolationKind k) {
  std::vector<Violation> out;
  for (Violation& v : pxf::ValidateFile(fd)) {
    if (v.kind == k) out.push_back(std::move(v));
  }
  return out;
}

class Rules : public ::testing::TestWithParam<Spelling> {
 protected:
  const char* DefaultName() const {
    return GetParam() == Spelling::kBracket ? "(pxf.default)" : "@default";
  }
  const char* RequiredName() const {
    return GetParam() == Spelling::kBracket ? "(pxf.required)" : "@required";
  }
  void ExpectDefaultViolation(const pb::FileDescriptorProto& f,
                              const std::string& element,
                              const std::string& detail) {
    Pool pool;
    const auto vs = Of(pool.Build(f), ViolationKind::kDefaultOption);
    ASSERT_EQ(vs.size(), 1u);
    EXPECT_EQ(vs[0].element, element);
    EXPECT_NE(vs[0].detail.find(detail), std::string::npos) << vs[0].detail;
    EXPECT_NE(vs[0].ToString().find(std::string("invalid ") + DefaultName() + " = \"7\""),
              std::string::npos)
        << vs[0].ToString();
  }
};

INSTANTIATE_TEST_SUITE_P(BothSpellings,
                         Rules,
                         ::testing::Values(Spelling::kBracket, Spelling::kCarrier),
                         [](const auto& info) {
                           return info.param == Spelling::kBracket ? "Bracket" : "Carrier";
                         });

TEST_P(Rules, DefaultOnRepeatedField) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  auto* f = AddField(m, "tags", kStr);
  f->set_label(pb::FieldDescriptorProto::LABEL_REPEATED);
  SetDefault(f->mutable_options(), GetParam(), "7");
  ExpectDefaultViolation(file, "rules.v1.M.tags", "not valid on repeated fields");
}

TEST_P(Rules, DefaultOnMapField) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  auto* entry = m->add_nested_type();
  entry->set_name("LabelsEntry");
  entry->mutable_options()->set_map_entry(true);
  AddField(entry, "key", kStr);
  AddField(entry, "value", kStr);
  auto* f = AddField(m, "labels", kMsg, ".rules.v1.M.LabelsEntry");
  f->set_label(pb::FieldDescriptorProto::LABEL_REPEATED);
  SetDefault(f->mutable_options(), GetParam(), "7");
  ExpectDefaultViolation(file, "rules.v1.M.labels", "not valid on map fields");
}

TEST_P(Rules, DefaultOnGroupField) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  file.set_syntax("proto2");
  auto* group = m->add_nested_type();
  group->set_name("G");
  auto* f = AddField(m, "g", pb::FieldDescriptorProto::TYPE_GROUP, ".rules.v1.M.G");
  SetDefault(f->mutable_options(), GetParam(), "7");
  ExpectDefaultViolation(file, "rules.v1.M.g", "not valid on group fields");
}

TEST_P(Rules, DefaultOnMessageWithNoLiteral) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  file.add_message_type()->set_name("Plain");
  m = file.mutable_message_type(0);
  auto* f = AddField(m, "p", kMsg, ".rules.v1.Plain");
  SetDefault(f->mutable_options(), GetParam(), "7");
  ExpectDefaultViolation(file, "rules.v1.M.p", "not valid on message type rules.v1.Plain");
}

TEST_P(Rules, DefaultOnLiteralMessageAndScalarIsAccepted) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  auto* t = AddField(m, "t", kMsg, ".google.protobuf.Timestamp");
  SetDefault(t->mutable_options(), GetParam(), "2026-01-02T03:04:05Z");
  auto* s = AddField(m, "s", kStr);
  SetDefault(s->mutable_options(), GetParam(), "x");
  Pool pool;
  const auto vs = pxf::ValidateFile(pool.Build(file));
  EXPECT_TRUE(vs.empty()) << vs[0].ToString();
}

TEST_P(Rules, AtMostOneOneofMemberCarriesADefault) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  m->add_oneof_decl()->set_name("pick");
  for (const char* name : {"a", "b"}) {
    auto* f = AddField(m, name, kStr);
    f->set_oneof_index(0);
    SetDefault(f->mutable_options(), GetParam(), "7");
  }
  AddField(m, "c", kStr)->set_oneof_index(0);
  Pool pool;
  const auto vs = Of(pool.Build(file), ViolationKind::kDefaultOption);
  ASSERT_EQ(vs.size(), 2u);  // each annotation to remove, not the oneof once
  EXPECT_EQ(vs[0].element, "rules.v1.M.a");
  EXPECT_EQ(vs[1].element, "rules.v1.M.b");
  EXPECT_EQ(vs[0].detail, "at most one member of oneof \"pick\" may carry a default; 2 do (a, b)");
}

TEST_P(Rules, OneDefaultInEachOneofIsAccepted) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  m->add_oneof_decl()->set_name("one");
  m->add_oneof_decl()->set_name("two");
  auto* a = AddField(m, "a", kStr);
  a->set_oneof_index(0);
  SetDefault(a->mutable_options(), GetParam(), "7");
  auto* b = AddField(m, "b", kStr);
  b->set_oneof_index(1);
  SetDefault(b->mutable_options(), GetParam(), "7");
  Pool pool;
  EXPECT_TRUE(pxf::ValidateFile(pool.Build(file)).empty());
}

TEST_P(Rules, RequiredOnOneofMember) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  m->add_oneof_decl()->set_name("pick");
  auto* f = AddField(m, "a", kStr);
  f->set_oneof_index(0);
  SetRequired(f->mutable_options(), GetParam());
  AddField(m, "b", kStr)->set_oneof_index(0);
  Pool pool;
  const auto vs = Of(pool.Build(file), ViolationKind::kRequiredOption);
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].element, "rules.v1.M.a");
  EXPECT_EQ(vs[0].name, "pick");
  EXPECT_NE(vs[0].ToString().find(std::string(RequiredName()) + " is not valid on a member of"),
            std::string::npos)
      << vs[0].ToString();
}

// A proto3 `optional` field sits in a synthetic oneof, and keeps plain
// per-field presence.
TEST_P(Rules, RequiredOnProto3OptionalIsAccepted) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  m->add_oneof_decl()->set_name("_a");
  auto* f = AddField(m, "a", kStr);
  f->set_oneof_index(0);
  f->set_proto3_optional(true);
  SetRequired(f->mutable_options(), GetParam());
  Pool pool;
  EXPECT_TRUE(pxf::ValidateFile(pool.Build(file)).empty());
}

// A bad placement fails every decode, and skip_validate reaches ApplyDefault,
// which must refuse rather than hand a scalar to a repeated field's setter.
TEST_P(Rules, BadPlacementFailsDecodeAndSkipValidateDoesNotAbort) {
  pb::DescriptorProto* m;
  auto file = Base(&m);
  auto* f = AddField(m, "tags", kStr);
  f->set_label(pb::FieldDescriptorProto::LABEL_REPEATED);
  SetDefault(f->mutable_options(), GetParam(), "7");
  Pool pool;
  const pb::FileDescriptor* fd = pool.Build(file);
  pb::DynamicMessageFactory factory;
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(fd->message_type(0))->New());

  EXPECT_FALSE(pxf::Unmarshal("", msg.get()).ok());
  pxf::UnmarshalOptions skip;
  skip.skip_validate = true;
  auto r = pxf::UnmarshalFull("", msg.get(), skip);
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("repeated or map field \"tags\""), std::string::npos)
      << r.status().ToString();
}

// --- retired option numbers ------------------------------------------------

void AddVarint(pb::Message* o, int num) {
  o->GetReflection()->MutableUnknownFields(o)->AddVarint(num, 1);
}

// Stale builds a file with one message, field, oneof and enum.
pb::FileDescriptorProto Stale(const std::vector<std::string>& deps) {
  pb::FileDescriptorProto f;
  f.set_name("stale.proto");
  f.set_package("stale.v1");
  f.set_syntax("proto3");
  for (const std::string& d : deps) f.add_dependency(d);
  auto* m = f.add_message_type();
  m->set_name("M");
  m->add_oneof_decl()->set_name("o");
  AddField(m, "x", kStr)->set_oneof_index(0);
  auto* e = f.add_enum_type();
  e->set_name("E");
  auto* v = e->add_value();
  v->set_name("E_ZERO");
  v->set_number(0);
  return f;
}

TEST(RetiredNumbers, EachIsDiagnosedOnItsOptionsKind) {
  auto f = Stale({"pxf/annotations.proto"});
  AddVarint(f.mutable_options(), 50100);
  AddVarint(f.mutable_options(), 50404);
  auto* m = f.mutable_message_type(0);
  AddVarint(m->mutable_options(), 50200);
  auto* field_opts = m->mutable_field(0)->mutable_options();
  AddVarint(field_opts, 50000);
  AddVarint(field_opts, 50001);
  AddVarint(field_opts, 51000);
  AddVarint(m->mutable_oneof_decl(0)->mutable_options(), 50400);
  AddVarint(f.mutable_enum_type(0)->mutable_options(), 50400);
  AddVarint(f.mutable_enum_type(0)->mutable_value(0)->mutable_options(), 50400);

  Pool pool;
  const pb::FileDescriptor* fd = pool.Build(f);
  const auto vs = pxf::ValidateFile(fd);
  ASSERT_EQ(vs.size(), 9u);
  struct Want {
    const char* element;
    const char* number;
    const char* detail;
  };
  const std::vector<Want> wants = {
      {"stale.proto", "50100", "(sbe.schema_id) before protowire v1.12.0 and is 1319 now"},
      {"stale.proto", "50404", "source_map carrier before protowire v1.12.0 and is 1331 now"},
      {"stale.v1.M", "50200", "(sbe.template_id) before protowire v1.12.0 and is 1321 now"},
      {"stale.v1.M.x", "50000", "(pxf.required) before protowire v1.12.0 and is 1314 now"},
      {"stale.v1.M.x", "50001", "(pxf.default) before protowire v1.12.0 and is 1315 now"},
      {"stale.v1.M.x", "51000", "protocheck field constraint"},
      {"stale.v1.M.o", "50400", "oneof_annotations carrier"},
      {"stale.v1.E", "50400", "*_annotations carrier"},
      {"stale.v1.E_ZERO", "50400", "*_annotations carrier"},
  };
  for (const Want& w : wants) {
    SCOPED_TRACE(w.element + std::string("#") + w.number);
    bool found = false;
    for (const Violation& v : vs) {
      if (v.element != w.element || v.name != w.number) continue;
      found = true;
      EXPECT_EQ(v.kind, ViolationKind::kRetiredNumber);
      EXPECT_NE(v.detail.find(w.detail), std::string::npos) << v.detail;
      EXPECT_NE(v.detail.find("must be recompiled"), std::string::npos);
      EXPECT_NE(v.ToString().find("STABILITY.md promise 3"), std::string::npos);
    }
    EXPECT_TRUE(found);
  }

  // A bind failure is the point: the alternative is a schema that silently
  // stopped enforcing (pxf.required).
  pb::DynamicMessageFactory factory;
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(fd->message_type(0))->New());
  protowire::Status st = pxf::Unmarshal("x = \"v\"\n", msg.get());
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("50000"), std::string::npos) << st.ToString();
}

// The same bytes without a protowire import are a third party's options in
// the range everybody uses. So are unallocated numbers, and a number on an
// Options kind its allocation never used.
TEST(RetiredNumbers, TheGateKeepsOtherProjectsOut) {
  {
    auto f = Stale({});
    AddVarint(f.mutable_options(), 50100);
    AddVarint(f.mutable_message_type(0)->mutable_field(0)->mutable_options(), 50000);
    Pool pool;
    EXPECT_TRUE(pxf::ValidateFile(pool.Build(f)).empty());
  }
  {
    auto f = Stale({"pxf/annotations.proto"});
    AddVarint(f.mutable_options(), 50999);
    AddVarint(f.mutable_options(), 50000);  // a field number, on FileOptions
    AddVarint(f.mutable_message_type(0)->mutable_field(0)->mutable_options(), 50050);
    Pool pool;
    EXPECT_TRUE(pxf::ValidateFile(pool.Build(f)).empty());
  }
}

// Like every bind-time check, this one runs over the import closure.
TEST(RetiredNumbers, AStaleImportIsDiagnosed) {
  auto stale = Stale({"pxf/annotations.proto"});
  AddVarint(stale.mutable_message_type(0)->mutable_field(0)->mutable_options(), 50000);
  pb::FileDescriptorProto uses;
  uses.set_name("uses.proto");
  uses.set_package("uses.v1");
  uses.set_syntax("proto3");
  uses.add_dependency("stale.proto");
  auto* u = uses.add_message_type();
  u->set_name("U");
  AddField(u, "m", kMsg, ".stale.v1.M");

  Pool pool;
  pool.Build(stale);
  const auto vs = pxf::ValidateDescriptor(pool.Build(uses)->message_type(0));
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].file, "stale.proto");
  EXPECT_EQ(vs[0].name, "50000");
}

}  // namespace

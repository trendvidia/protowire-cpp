// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// The annotation carrier (protowire-cpp#55). `@required` and `@default` lower
// to one AnnotationList at extension 1327 on a field's options. These tests
// build descriptors from raw carrier bytes, since no protoc here understands
// the `@` spelling, and assert the binder answers as it does for the bracket
// options.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/unknown_field_set.h>

#include "protowire/pxf.h"
#include "protowire/pxf/annotations.h"

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;

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
std::string Int(int num, uint64_t v) {
  return Varint(uint64_t(num) << 3) + Varint(v);
}
std::string Double(int num, double d) {
  char raw[8];
  std::memcpy(raw, &d, 8);
  return Varint(uint64_t(num) << 3 | 1) + std::string(raw, 8);
}

// Field numbers of protowire/proto/schema/v1/descriptor.proto.
std::string Required() {
  return Bytes(1, Bytes(1, "protowire.schema.v1.required"));
}
std::string Default(const std::string& arg) {
  return Bytes(1, Bytes(1, "protowire.schema.v1.default") + Bytes(2, arg));
}
std::string Other() {
  return Bytes(1, Bytes(1, "protowire.schema.v1.description") + Bytes(2, Bytes(10, "docs")));
}

struct FieldSpec {
  std::string name;
  pb::FieldDescriptorProto::Type type;
  std::string carrier;
  std::string bracket_default;  // (pxf.default) at extension 1315
  bool bracket_required = false;
  std::string type_name;
};

// Schema builds one message M in a pool of its own.
class Schema {
 public:
  explicit Schema(const std::vector<FieldSpec>& fields) {
    pb::FileDescriptorProto file;
    file.set_name("carrier.proto");
    file.set_package("carrier.v1");
    file.set_syntax("proto3");
    auto* m = file.add_message_type();
    m->set_name("M");
    int number = 0;
    for (const FieldSpec& s : fields) {
      auto* f = m->add_field();
      f->set_name(s.name);
      f->set_number(++number);
      f->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
      f->set_type(s.type);
      if (!s.type_name.empty()) f->set_type_name(s.type_name);
      auto* opts = f->mutable_options();
      auto* unknown = opts->GetReflection()->MutableUnknownFields(opts);
      if (s.bracket_required) unknown->AddVarint(1314, 1);
      if (!s.bracket_default.empty()) unknown->AddLengthDelimited(1315, s.bracket_default);
      if (!s.carrier.empty()) unknown->AddLengthDelimited(1327, s.carrier);
    }
    const pb::FileDescriptor* fd = pool_.BuildFile(file);
    EXPECT_NE(fd, nullptr);
    desc_ = fd->message_type(0);
    factory_ = std::make_unique<pb::DynamicMessageFactory>(&pool_);
  }

  const pb::Descriptor* desc() const { return desc_; }
  std::unique_ptr<pb::Message> New() const {
    return std::unique_ptr<pb::Message>(factory_->GetPrototype(desc_)->New());
  }

 private:
  pb::DescriptorPool pool_;
  const pb::Descriptor* desc_ = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
};

constexpr auto kString = pb::FieldDescriptorProto::TYPE_STRING;
constexpr auto kInt32 = pb::FieldDescriptorProto::TYPE_INT32;
constexpr auto kUint64 = pb::FieldDescriptorProto::TYPE_UINT64;
constexpr auto kDouble = pb::FieldDescriptorProto::TYPE_DOUBLE;
constexpr auto kBool = pb::FieldDescriptorProto::TYPE_BOOL;
constexpr auto kBytes = pb::FieldDescriptorProto::TYPE_BYTES;

TEST(PxfCarrier, RequiredIsEnforced) {
  Schema s({{"a", kString, Required()}, {"b", kString}});
  EXPECT_TRUE(pxf::IsRequired(s.desc()->FindFieldByName("a")));
  EXPECT_FALSE(pxf::IsRequired(s.desc()->FindFieldByName("b")));
  auto missing = s.New();
  auto r = pxf::UnmarshalFull("b = \"x\"\n", missing.get());
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("required field \"a\" is absent"), std::string::npos)
      << r.status().ToString();
  auto present = s.New();
  EXPECT_TRUE(pxf::UnmarshalFull("a = \"x\"\n", present.get()).ok());
}

TEST(PxfCarrier, DefaultIsApplied) {
  Schema s({{"name", kString, Default(Bytes(10, "viewer"))},
            {"n", kInt32, Default(Int(11, 7))},
            {"on", kBool, Default(Int(13, 1))}});
  auto msg = s.New();
  auto r = pxf::UnmarshalFull("", msg.get());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  const auto* refl = msg->GetReflection();
  EXPECT_EQ(refl->GetString(*msg, s.desc()->FindFieldByName("name")), "viewer");
  EXPECT_EQ(refl->GetInt32(*msg, s.desc()->FindFieldByName("n")), 7);
  EXPECT_TRUE(refl->GetBool(*msg, s.desc()->FindFieldByName("on")));
}

// Each argument member reduces to the literal the bracket form would carry.
TEST(PxfCarrier, ArgumentsReduceToBracketLiterals) {
  struct Case {
    const char* what;
    pb::FieldDescriptorProto::Type type;
    std::string arg;
    const char* want;
  };
  const std::vector<Case> cases = {
      {"string", kString, Bytes(10, "x"), "x"},
      {"named argument", kString, Bytes(1, "value") + Bytes(10, "x"), "x"},
      {"int", kInt32, Int(11, 5), "5"},
      {"negative int", kInt32, Int(11, uint64_t(-5)), "-5"},
      {"unsigned through int64", kUint64, Int(11, ~uint64_t{0}), "18446744073709551615"},
      {"double", kDouble, Double(12, 1.5), "1.5"},
      {"bool", kBool, Int(13, 1), "true"},
      {"bytes", kBytes, Bytes(14, std::string("\x01\x02\x03", 3)), "AQID"},
      {"enum literal", kString, Bytes(15, Bytes(1, Bytes(2, "ACTIVE"))), "ACTIVE"},
      {"big int", kString, Bytes(16, Bytes(1, std::string("\x01\x00", 2)) + Int(2, 1)), "-256"},
      {"decimal", kString, Bytes(17, Bytes(1, "\x7b") + Int(2, 2)), "1.23"},
      {"big float",
       kString,
       Bytes(18, Bytes(1, "\x03") + Int(2, uint64_t(-1)) + Int(3, 53)),
       "1.5p53"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.what);
    Schema s({{"f", c.type, Default(c.arg)}});
    auto def = pxf::GetDefault(s.desc()->FindFieldByName("f"));
    ASSERT_TRUE(def.has_value());
    EXPECT_EQ(*def, c.want);
  }
}

// A pxf.BigInt literal is positional, so a double argument expands.
TEST(PxfCarrier, DoubleOnABigIntIsPositional) {
  pb::DescriptorPool pool;
  pb::FileDescriptorProto bignum;
  bignum.set_name("pxf/bignum.proto");
  bignum.set_package("pxf");
  bignum.set_syntax("proto3");
  bignum.add_message_type()->set_name("BigInt");
  ASSERT_NE(pool.BuildFile(bignum), nullptr);
  pb::FileDescriptorProto file;
  file.set_name("c.proto");
  file.set_syntax("proto3");
  file.add_dependency("pxf/bignum.proto");
  auto* m = file.add_message_type();
  m->set_name("M");
  auto* f = m->add_field();
  f->set_name("b");
  f->set_number(1);
  f->set_label(pb::FieldDescriptorProto::LABEL_OPTIONAL);
  f->set_type(pb::FieldDescriptorProto::TYPE_MESSAGE);
  f->set_type_name(".pxf.BigInt");
  auto* opts = f->mutable_options();
  opts->GetReflection()->MutableUnknownFields(opts)->AddLengthDelimited(1327,
                                                                        Default(Double(12, 1e20)));
  const pb::FileDescriptor* fd = pool.BuildFile(file);
  ASSERT_NE(fd, nullptr);
  auto def = pxf::GetDefault(fd->message_type(0)->FindFieldByName("b"));
  ASSERT_TRUE(def.has_value());
  EXPECT_EQ(*def, "100000000000000000000");
}

TEST(PxfCarrier, AnnotationsItDoesNotEnforceAreSkipped) {
  Schema s({{"a", kString, Other() + Required()}});
  EXPECT_TRUE(pxf::IsRequired(s.desc()->FindFieldByName("a")));
  EXPECT_TRUE(pxf::ValidateDescriptor(s.desc()).empty());
}

TEST(PxfCarrier, RequiredWithDefaultIsAViolationInEverySpelling) {
  struct Case {
    const char* what;
    FieldSpec field;
    const char* quoted;
  };
  const std::vector<Case> cases = {
      {"both annotation", {"f", kString, Required() + Default(Bytes(10, "x"))}, "@required"},
      {"@required, (pxf.default)", {"f", kString, Required(), "x"}, "@required"},
      {"(pxf.required), @default",
       {"f", kString, Default(Bytes(10, "x")), "", true},
       "(pxf.required)"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.what);
    Schema s({c.field});
    auto vs = pxf::ValidateDescriptor(s.desc());
    ASSERT_EQ(vs.size(), 1u);
    EXPECT_EQ(vs[0].kind, pxf::ViolationKind::kRequiredOption);
    EXPECT_EQ(vs[0].name, "x");
    EXPECT_NE(vs[0].ToString().find(c.quoted), std::string::npos) << vs[0].ToString();
    auto msg = s.New();
    EXPECT_FALSE(pxf::Unmarshal("", msg.get()).ok());
  }
}

TEST(PxfCarrier, SurfacesThatDisagreeAreRejected) {
  Schema differ({{"f", kString, Default(Bytes(10, "a")), "b"}});
  auto vs = pxf::ValidateDescriptor(differ.desc());
  ASSERT_EQ(vs.size(), 1u);
  EXPECT_EQ(vs[0].kind, pxf::ViolationKind::kDefaultOption);
  EXPECT_NE(vs[0].ToString().find("different values"), std::string::npos) << vs[0].ToString();

  Schema same({{"f", kString, Default(Bytes(10, "a")), "a"}});
  EXPECT_TRUE(pxf::ValidateDescriptor(same.desc()).empty());

  Schema same_double({{"f", kDouble, Default(Double(12, 1.5)), "1.5"}});
  EXPECT_TRUE(pxf::ValidateDescriptor(same_double.desc()).empty());
}

TEST(PxfCarrier, UnusableDefaultIsAViolationNotDropped) {
  const std::vector<std::string> args = {
      "",                                          // no value at all
      Bytes(15, Bytes(2, "")),                     // a list or message literal
      Bytes(18, Bytes(1, "\x01")),                 // a big float with no precision
      Bytes(17, Bytes(1, "\x01") + Int(2, 5000)),  // a Decimal past the scale bound
  };
  for (const std::string& arg : args) {
    Schema s({{"f", kString, Default(arg)}});
    auto vs = pxf::ValidateDescriptor(s.desc());
    ASSERT_EQ(vs.size(), 1u);
    EXPECT_EQ(vs[0].kind, pxf::ViolationKind::kDefaultOption);
    EXPECT_NE(vs[0].ToString().find("invalid @default"), std::string::npos) << vs[0].ToString();
  }
}

// A garbled tail keeps what was read before it: dropping a @required because
// a later byte is bad would be the silent non-enforcement this path ends.
TEST(PxfCarrier, MalformedBytesAreSurvivable) {
  Schema truncated({{"a", kString, Required() + std::string("\x0a\xff", 2)}});
  EXPECT_TRUE(pxf::IsRequired(truncated.desc()->FindFieldByName("a")));
  Schema garbage({{"a", kString, std::string("\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 11)}});
  EXPECT_FALSE(pxf::IsRequired(garbage.desc()->FindFieldByName("a")));
}

}  // namespace

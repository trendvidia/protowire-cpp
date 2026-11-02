// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// The validator seam, UnmarshalDescriptor and ApplyDefault. Mirrors protowire-go
// check_test.go; scripts/go_differential.sh compares the text with Go's.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include "protowire/check.h"
#include "protowire/pxf.h"

namespace {

namespace gpb = google::protobuf;
namespace check = protowire::check;
namespace pxf = protowire::pxf;
using protowire::Status;
using protowire::StatusOr;

class Fake : public check::Validator {
 public:
  explicit Fake(StatusOr<check::Report> result) : result_(std::move(result)) {}
  StatusOr<check::Report> Validate(const gpb::Message& m) override {
    seen = m.GetDescriptor()->full_name();
    ++calls;
    return result_;
  }
  std::string seen;
  int calls = 0;

 private:
  StatusOr<check::Report> result_;
};

check::Report Bad() {
  return check::Report{{{"name", "test.rule", "too short"}}};
}

class Seam : public ::testing::Test {
 protected:
  void SetUp() override {
    gpb::FileDescriptorProto file;
    file.set_name("seam.proto");
    file.set_package("seam.v1");
    file.set_syntax("proto3");
    auto* m = file.add_message_type();
    m->set_name("M");
    auto* f = m->add_field();
    f->set_name("name");
    f->set_number(1);
    f->set_type(gpb::FieldDescriptorProto::TYPE_STRING);
    f->set_label(gpb::FieldDescriptorProto::LABEL_OPTIONAL);
    auto* n = m->add_field();
    n->set_name("n");
    n->set_number(2);
    n->set_type(gpb::FieldDescriptorProto::TYPE_INT32);
    n->set_label(gpb::FieldDescriptorProto::LABEL_OPTIONAL);
    ASSERT_NE(pool_.BuildFile(file), nullptr);
    desc_ = pool_.FindMessageTypeByName("seam.v1.M");
    ASSERT_NE(desc_, nullptr);
  }
  std::unique_ptr<gpb::Message> New() {
    return std::unique_ptr<gpb::Message>(factory_.GetPrototype(desc_)->New());
  }
  gpb::DescriptorPool pool_;
  gpb::DynamicMessageFactory factory_;
  const gpb::Descriptor* desc_ = nullptr;
};

}  // namespace

TEST(CheckError, Text) {
  EXPECT_EQ(check::ErrorMessage(check::Report{}), "check: validation failed");
  EXPECT_EQ(check::ErrorMessage(check::Report{{{"", "", "bad"}}}), "check: bad");
  EXPECT_EQ(check::ErrorMessage(check::Report{{{"a.b", "r.x", "bad"}}}), "check: a.b: bad (r.x)");
  EXPECT_EQ(check::ErrorMessage(check::Report{{{"a", "", "one"}, {"", "r", "two"}}}),
            "check: 2 violations; first: a: one");
}

TEST(CheckError, ReportOK) {
  EXPECT_TRUE(check::Report{}.OK());
  EXPECT_FALSE(Bad().OK());
}

TEST_F(Seam, CleanPassRunsAndKeepsTheReport) {
  Fake v{check::Report{}};
  pxf::UnmarshalOptions opts;
  opts.validator = &v;
  auto m = New();
  EXPECT_TRUE(pxf::Unmarshal("name = \"x\"", m.get(), opts).ok());
  EXPECT_EQ(v.calls, 1);
  EXPECT_EQ(v.seen, "seam.v1.M");
  auto full = pxf::UnmarshalFull("name = \"x\"", New().get(), opts);
  ASSERT_TRUE(full.ok());
  ASSERT_NE(full->Report(), nullptr);
  EXPECT_TRUE(full->Report()->OK());
}

TEST_F(Seam, NoValidatorNoReport) {
  auto full = pxf::UnmarshalFull("name = \"x\"", New().get());
  ASSERT_TRUE(full.ok());
  EXPECT_EQ(full->Report(), nullptr);
}

TEST_F(Seam, ViolationFailsAndHandsBackTheReport) {
  Fake v{Bad()};
  check::Report got;
  pxf::UnmarshalOptions opts;
  opts.validator = &v;
  opts.report = &got;
  auto m = New();
  Status st = pxf::Unmarshal("name = \"x\"", m.get(), opts);
  EXPECT_EQ(st.message(), "check: name: too short (test.rule)");
  ASSERT_EQ(got.violations.size(), 1u);
  EXPECT_EQ(got.violations[0].rule_id, "test.rule");
  // The message stays decoded.
  EXPECT_EQ(m->GetReflection()->GetString(*m, desc_->FindFieldByName("name")), "x");

  check::Report full_got;
  opts.report = &full_got;
  auto full = pxf::UnmarshalFull("name = \"x\"", New().get(), opts);
  EXPECT_EQ(full.status().message(), "check: name: too short (test.rule)");
  EXPECT_EQ(full_got.violations.size(), 1u);
}

TEST_F(Seam, EngineFailureIsReturnedAsItIs) {
  Fake v{Status::Error("no such schema")};
  check::Report got = Bad();
  pxf::UnmarshalOptions opts;
  opts.validator = &v;
  opts.report = &got;
  EXPECT_EQ(pxf::Unmarshal("n = 1", New().get(), opts).message(), "no such schema");
  EXPECT_EQ(got.violations.size(), 1u);  // left alone
}

TEST_F(Seam, DecodeFailureSkipsTheValidator) {
  Fake v{check::Report{}};
  pxf::UnmarshalOptions opts;
  opts.validator = &v;
  EXPECT_FALSE(pxf::Unmarshal("n = \"x\"", New().get(), opts).ok());
  EXPECT_EQ(v.calls, 0);
}

TEST_F(Seam, UnmarshalDescriptorOwnsItsMessage) {
  std::shared_ptr<gpb::Message> kept;
  {
    auto m = pxf::UnmarshalDescriptor("name = \"x\"", desc_);
    ASSERT_TRUE(m.ok());
    kept = *m;
  }
  EXPECT_EQ(kept->GetReflection()->GetString(*kept, desc_->FindFieldByName("name")), "x");
  EXPECT_FALSE(pxf::UnmarshalDescriptor("nope = 1", desc_).ok());
}

TEST_F(Seam, UnmarshalFullDescriptorReportsPresence) {
  auto r = pxf::UnmarshalFullDescriptor("n = 1\n", desc_);
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->result.IsSet("n"));
  EXPECT_TRUE(r->result.IsAbsent("name"));
  EXPECT_EQ(r->result.PresentFields(), std::vector<std::string>{"n"});
}

TEST_F(Seam, ApplyDefaultReadsLikeStrconv) {
  auto m = New();
  const auto* n = desc_->FindFieldByName("n");
  EXPECT_TRUE(pxf::ApplyDefault(m.get(), n, "+5").ok());
  EXPECT_EQ(m->GetReflection()->GetInt32(*m, n), 5);
  EXPECT_EQ(pxf::ApplyDefault(m.get(), n, "x").message(),
            "invalid default int32 \"x\" for field \"n\": strconv.ParseInt: parsing \"x\": "
            "invalid syntax");
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// DatasetReader::Scan binds a row under the options the reader was built
// with, as BindRow binds it under the same options (#166). protowire-go's
// Scan ignored skip_validate, skip_post_decode, discard_unknown,
// on_secret_field and validator; it was changed to honour them
// (protowire-go#280), so the two ports agree. Each case binds one row
// through both paths, with the option off and on, and requires the paths
// to agree. The protos, rows and error texts are protowire-go's
// (encoding/pxf/dataset_scan_options_test.go).

#include "protowire/check_seam.h"
#include "protowire/pxf.h"
#include "protowire/pxf/dataset_reader.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/message_differencer.h>

namespace {

namespace pb = google::protobuf;
namespace pxf = protowire::pxf;
using protowire::Status;

constexpr const char* kSchema = R"src(
syntax = "proto3";
package so.v1;

import "pxf/annotations.proto";
import "pxf/secret.proto";

message Inner { int32 n = 1; }
message Row {
  string id = 1;
  int32 req = 2 [(pxf.required) = true];
  string dflt = 3 [(pxf.default) = "z"];
  Inner inner = 4;
  pxf.Secret pw = 5;
}
)src";

// A field named null breaks a bind-time schema rule (draft §3.13). It is in
// a file of its own, because the schema check reads the whole file.
constexpr const char* kBadSchema = R"src(
syntax = "proto3";
package sobad.v1;

message Bad {
  string id = 1;
  string null = 2;
}
)src";

class Errors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
           ": " + std::string(msg);
  }
  std::string last;
};

class DenyAll : public protowire::check::Validator {
 public:
  protowire::StatusOr<protowire::check::Report> Validate(const pb::Message&) override {
    protowire::check::Report r;
    r.violations.push_back({"id", "test.deny", "denied"});
    return r;
  }
};

// One row bound both ways: the message, or the error both paths gave.
struct Bound {
  std::unique_ptr<pb::Message> msg;
  std::string err;  // empty when the row bound
};

class DatasetScanOptions : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream(protowire_test::FixtureDir() + "/so.proto", std::ios::binary) << kSchema;
    std::ofstream(protowire_test::FixtureDir() + "/sobad.proto", std::ios::binary) << kBadSchema;
    tree_.MapPath("", protowire_test::FixtureDir());
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&tree_, &errors_);
    const pb::FileDescriptor* fd = importer_->Import("so.proto");
    ASSERT_NE(fd, nullptr) << errors_.last;
    row_ = fd->FindMessageTypeByName("Row");
    const pb::FileDescriptor* bad = importer_->Import("sobad.proto");
    ASSERT_NE(bad, nullptr) << errors_.last;
    bad_ = bad->FindMessageTypeByName("Bad");
  }

  // BindUnder binds the one row of table under opts, through Scan and
  // through Parse + BindRow, and requires the two to agree.
  Bound BindUnder(const pxf::UnmarshalOptions& opts,
                  const pb::Descriptor* desc,
                  const std::string& table) {
    Bound out;
    auto doc = pxf::Parse(table);
    EXPECT_TRUE(doc.ok()) << doc.status().ToString();
    if (!doc.ok()) return out;
    const auto& ds = doc->datasets[0];
    EXPECT_EQ(ds.rows.size(), 1u);
    std::unique_ptr<pb::Message> via_bind(factory_.GetPrototype(desc)->New());
    const Status berr = pxf::BindRow(via_bind.get(), ds.columns, ds.rows[0], opts);

    std::istringstream src(table);
    auto tr = pxf::DatasetReader::Create(&src, opts);
    EXPECT_TRUE(tr.ok()) << tr.status().ToString();
    if (!tr.ok()) return out;
    std::unique_ptr<pb::Message> via_scan(factory_.GetPrototype(desc)->New());
    const Status serr = (*tr)->Scan(via_scan.get());
    EXPECT_EQ(berr.ok() ? "" : berr.ToString(), serr.ok() ? "" : serr.ToString())
        << "BindRow vs Scan";
    if (!berr.ok()) {
      out.err = berr.ToString();
      return out;
    }
    EXPECT_FALSE((*tr)->Done()) << "Scan read no row";
    EXPECT_TRUE(pb::util::MessageDifferencer::Equals(*via_bind, *via_scan))
        << "BindRow " << via_bind->ShortDebugString() << ", Scan " << via_scan->ShortDebugString();
    std::unique_ptr<pb::Message> after(factory_.GetPrototype(desc)->New());
    EXPECT_TRUE((*tr)->Scan(after.get()).ok());
    EXPECT_TRUE((*tr)->Done()) << "a second row was read";
    out.msg = std::move(via_scan);
    return out;
  }

  static std::string Str(const pb::Message& m, const char* name) {
    return m.GetReflection()->GetString(m, m.GetDescriptor()->FindFieldByName(name));
  }
  static const pb::Message& Sub(const pb::Message& m, const char* name) {
    return m.GetReflection()->GetMessage(m, m.GetDescriptor()->FindFieldByName(name));
  }

  pb::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  pb::DynamicMessageFactory factory_;
  const pb::Descriptor* row_ = nullptr;
  const pb::Descriptor* bad_ = nullptr;
};

TEST_F(DatasetScanOptions, SkipPostDecode) {
  const std::string table = "@dataset so.v1.Row ( id )\n(\"x\")\n";
  EXPECT_EQ(BindUnder({}, row_, table).err, "2:1: required field \"req\" is absent");

  pxf::UnmarshalOptions o;
  o.skip_post_decode = true;
  Bound b = BindUnder(o, row_, table);
  ASSERT_EQ(b.err, "");
  // No post-decode pass: the required field is not checked, and the
  // default is not applied.
  EXPECT_EQ(Str(*b.msg, "dflt"), "");
}

TEST_F(DatasetScanOptions, DiscardUnknown) {
  const std::string table =
      "@dataset so.v1.Row ( id, req, inner )\n(\"x\", 1, { n = 2 nope = 3 })\n";
  EXPECT_EQ(BindUnder({}, row_, table).err,
            "2:1: column inner: unknown field \"nope\" in so.v1.Inner");

  pxf::UnmarshalOptions o;
  o.discard_unknown = true;
  Bound b = BindUnder(o, row_, table);
  ASSERT_EQ(b.err, "");
  const pb::Message& inner = Sub(*b.msg, "inner");
  EXPECT_EQ(inner.GetReflection()->GetInt32(inner, inner.GetDescriptor()->FindFieldByName("n")), 2);
}

TEST_F(DatasetScanOptions, OnSecretField) {
  const std::string table = "@dataset so.v1.Row ( id, req, pw )\n(\"x\", 1, \"s3cret\")\n";
  Bound plain = BindUnder({}, row_, table);
  ASSERT_EQ(plain.err, "");
  EXPECT_EQ(Str(Sub(*plain.msg, "pw"), "value"), "s3cret");

  // BindUnder binds the row twice, so the hook fires once per path.
  std::vector<std::string> got;
  pxf::UnmarshalOptions o;
  o.on_secret_field = [&got](std::string_view path, std::string_view value) {
    got.push_back(std::string(path) + "=" + std::string(value));
    return Status::OK();
  };
  Bound b = BindUnder(o, row_, table);
  ASSERT_EQ(b.err, "");
  EXPECT_EQ(got, (std::vector<std::string>{"pw=s3cret", "pw=s3cret"}));
  EXPECT_EQ(Str(Sub(*b.msg, "pw"), "value"), "");
}

// A hook's error is a decode error: it carries the row's position and names
// the column, as protowire-go gives it.
TEST_F(DatasetScanOptions, OnSecretFieldError) {
  pxf::UnmarshalOptions o;
  o.on_secret_field = [](std::string_view, std::string_view) {
    return Status::Error("hook says no");
  };
  for (const char* table : {"@dataset so.v1.Row ( id, req, pw )\n(\"x\", 1, \"s3cret\")\n",
                            "@dataset so.v1.Row ( pw, id, req )\n(\"s3cret\", \"x\", 1)\n"}) {
    EXPECT_EQ(BindUnder(o, row_, table).err,
              "2:1: column pw: pxf.Secret hook for \"pw\": hook says no")
        << table;
  }
}

// A validator's error is returned as it is, with no position: protowire-go
// moves only a decode error to the row.
TEST_F(DatasetScanOptions, Validator) {
  const std::string table = "@dataset so.v1.Row ( id, req )\n(\"x\", 1)\n";
  EXPECT_EQ(BindUnder({}, row_, table).err, "");

  DenyAll deny;
  protowire::check::Report report;
  pxf::UnmarshalOptions o;
  o.validator = &deny;
  o.report = &report;
  EXPECT_EQ(BindUnder(o, row_, table).err, "check: id: denied (test.deny)");
  ASSERT_EQ(report.violations.size(), 1u);
  EXPECT_EQ(report.violations[0].rule_id, "test.deny");
}

// A schema violation is returned as it is, with no position, through
// BindRow as through Scan.
TEST_F(DatasetScanOptions, SkipValidate) {
  const std::string table = "@dataset sobad.v1.Bad ( id )\n(\"x\")\n";
  EXPECT_EQ(BindUnder({}, bad_, table).err,
            "PXF schema violations:\n  sobad.proto: message field \"sobad.v1.Bad.null\" uses "
            "PXF-reserved name \"null\" (draft §3.13)");

  pxf::UnmarshalOptions o;
  o.skip_validate = true;
  Bound b = BindUnder(o, bad_, table);
  ASSERT_EQ(b.err, "");
  EXPECT_EQ(Str(*b.msg, "id"), "x");
}

// The column paths are checked before the schema, as protowire-go checks
// them: a row that breaks both gets the column's error.
TEST_F(DatasetScanOptions, ColumnPathsBeforeSchema) {
  EXPECT_EQ(BindUnder({}, bad_, "@dataset sobad.v1.Bad ( nope )\n(\"x\")\n").err,
            "2:1: @dataset column \"nope\": sobad.v1.Bad has no field \"nope\"");
}

}  // namespace

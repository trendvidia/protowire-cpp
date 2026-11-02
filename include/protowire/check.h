// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// The validation seam. Mirrors protowire-go's check package.
//
// A decoder takes an optional Validator, which runs after a successful decode
// and reports data violations. This header owns only the seam: the interface,
// the report and the error text. Validation engines live elsewhere and plug in
// here.
//
//   pxf::UnmarshalOptions opts;
//   opts.validator = &my_validator;
//   check::Report report;
//   opts.report = &report;  // for the violations, since a Status has only text
//   Status st = pxf::Unmarshal(data, msg, opts);
//   if (!st.ok()) for (const auto& v : report.violations) { ... }

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/message.h>

#include "protowire/detail/status.h"

namespace protowire::check {

// Violation describes one failed validation rule.
struct Violation {
  // The dotted path of the offending field, in the scheme of pxf presence
  // tracking: "name", "db.password", "keys[0]". Empty for message-level rules.
  std::string path;
  // The violated rule, namespaced by engine: "buf.validate.string.min_len".
  std::string rule_id;
  // A description for a person.
  std::string message;
};

// Report is the outcome of a validation pass.
struct Report {
  std::vector<Violation> violations;
  bool OK() const { return violations.empty(); }
};

// Validator runs data validation over a decoded message. The returned Report
// carries data violations; a non-OK status is reserved for engine failure (an
// unresolvable schema, a bad constraint expression). A clean pass returns an
// empty Report. Implementations must be safe for concurrent use: decoders may
// share one validator across threads.
class Validator {
 public:
  virtual ~Validator() = default;
  virtual StatusOr<Report> Validate(const google::protobuf::Message& decoded) = 0;
};

// ErrorMessage is the text of the failure a report with violations produces.
inline std::string ErrorMessage(const Report& report) {
  if (report.violations.empty()) return "check: validation failed";
  const Violation& v = report.violations[0];
  std::string msg = v.message;
  if (!v.path.empty()) msg = v.path + ": " + msg;
  if (!v.rule_id.empty()) msg += " (" + v.rule_id + ")";
  if (report.violations.size() == 1) return "check: " + msg;
  return "check: " + std::to_string(report.violations.size()) + " violations; first: " + msg;
}

// Validate applies v with the semantics every decoder shares: a null validator
// is a no-op, an engine failure propagates as it is, and a report with
// violations fails with ErrorMessage's text. *report, if not null, receives the
// report in every case but an engine failure. has_report, if not null, says
// whether it did.
inline Status Validate(Validator* v,
                       const google::protobuf::Message& decoded,
                       Report* report,
                       bool* has_report = nullptr) {
  if (has_report != nullptr) *has_report = false;
  if (v == nullptr) return Status::OK();
  auto rep = v->Validate(decoded);
  if (!rep.ok()) return rep.status();
  Report r = std::move(rep).consume();
  Status st = r.OK() ? Status::OK() : Status::Error(ErrorMessage(r));
  if (report != nullptr) *report = std::move(r);
  if (has_report != nullptr) *has_report = true;
  return st;
}

}  // namespace protowire::check

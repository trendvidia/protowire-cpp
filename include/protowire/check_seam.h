// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// The validation seam, without protobuf: protowire_pb does not link it, and
// declaring Message is all this needs. Most callers want protowire/check.h.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "protowire/detail/status.h"

namespace google::protobuf {
class Message;
}  // namespace google::protobuf

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

// TypeTag names a C++ type without RTTI: a pb struct has no descriptor to say
// what it is, and pb.h is used in builds with -fno-rtti. TypeTagOf<T>() gives
// one address per type. A shared library that hides its symbols has its own
// copy, so compare tags only for types decoded in the same library as the
// validator.
using TypeTag = const void*;

template <class T>
TypeTag TypeTagOf() {
  static const char tag = 0;
  return &tag;
}

// Validator runs data validation over a decoded value: a message for the pxf
// and sbe decoders, a struct for the pb decoder. The returned Report carries
// data violations; a non-OK status is reserved for engine failure (an
// unresolvable schema, a bad constraint expression, a value it cannot validate).
// A clean pass returns an empty Report. Implementations must be safe for
// concurrent use: decoders may share one validator across threads.
class Validator {
 public:
  virtual ~Validator() = default;
  virtual StatusOr<Report> Validate(const google::protobuf::Message& decoded) = 0;
  // The pb decoder's value: decoded points at a T with type == TypeTagOf<T>().
  // An engine for messages alone need not override this.
  virtual StatusOr<Report> ValidateStruct(const void* decoded, TypeTag type) {
    (void)decoded;
    (void)type;
    return Status::Error("check: cannot validate a struct: not a protobuf message");
  }
};

// StructValidator is a Validator for one pb struct, which hides the cast.
template <class T>
class StructValidator : public Validator {
 public:
  virtual StatusOr<Report> ValidateValue(const T& decoded) = 0;

  StatusOr<Report> Validate(const google::protobuf::Message&) override {
    return Status::Error("check: cannot validate a message: not the struct this validates");
  }
  StatusOr<Report> ValidateStruct(const void* decoded, TypeTag type) override {
    if (type != TypeTagOf<T>()) {
      return Status::Error("check: cannot validate a struct of another type");
    }
    return ValidateValue(*static_cast<const T*>(decoded));
  }
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

namespace detail {

// Finish gives a validator's result the semantics every decoder shares.
inline Status Finish(StatusOr<Report> rep, Report* report, bool* has_report) {
  if (!rep.ok()) return rep.status();
  Report r = std::move(rep).consume();
  Status st = r.OK() ? Status::OK() : Status::Error(ErrorMessage(r));
  if (report != nullptr) *report = std::move(r);
  if (has_report != nullptr) *has_report = true;
  return st;
}

}  // namespace detail

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
  return detail::Finish(v->Validate(decoded), report, has_report);
}

// ValidateStruct is Validate for a decoded pb struct.
template <class T>
Status ValidateStruct(Validator* v, const T& decoded, Report* report) {
  if (v == nullptr) return Status::OK();
  return detail::Finish(v->ValidateStruct(&decoded, TypeTagOf<T>()), report, nullptr);
}

}  // namespace protowire::check

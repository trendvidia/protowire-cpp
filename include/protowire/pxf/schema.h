// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// PXF schema-level conformance check per draft §3.13. A protobuf schema
// bound for PXF use MUST NOT declare a message field, oneof, or enum
// value whose name is case-sensitively equal to a PXF value keyword
// (`null` / `true` / `false`) — such a name lexes as the keyword, so
// the declared element is unreachable from PXF surface syntax.
//
// Enforcement runs at descriptor-bind time inside Unmarshal /
// UnmarshalFull, over the bound file's import closure (draft -01 § Scope
// of Bind-Time Checks). Callers that have already validated their descriptors
// (typically via ValidateDescriptor in a one-time codegen or registry-
// load pass) may set UnmarshalOptions::skip_validate to bypass the
// per-call recheck.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/descriptor.h>

namespace protowire::pxf {

// ViolationKind identifies which kind of schema element collides with a
// reserved PXF value keyword.
enum class ViolationKind : uint8_t {
  kField = 1,
  kOneof,
  kEnumValue,
  // A (pxf.key) annotation whose placement draft -01 §3.13 forbids: on a
  // field that is not a repeated message-typed field, naming a field the
  // element message lacks, or naming one that is not a singular string.
  kKeyOption,
};

const char* ViolationKindName(ViolationKind k);

// Violation describes one schema element whose name collides with a
// reserved PXF keyword. Returned by ValidateDescriptor / ValidateFile.
struct Violation {
  std::string file;     // .proto file path the offending element is declared in
  std::string element;  // fully-qualified protobuf name (e.g. "trades.v1.Side.null")
  std::string name;     // bare reserved identifier, or the (pxf.key) value for kKeyOption
  ViolationKind kind = ViolationKind::kField;
  std::string detail;  // human-readable explanation; set for kKeyOption

  // One-line human-readable description, e.g.
  //   "trades.proto: message field \"trades.v1.X.null\" uses PXF-reserved name \"null\" (draft
  //   §3.13)"
  std::string ToString() const;
};

// ValidateDescriptor walks the file containing `desc` together with its
// transitive imports, and returns every bind-time violation in that
// closure: reserved-name collisions among messages, oneofs, and enum
// values, and invalid (pxf.key) placements (draft -01 §3.13). The
// returned vector is sorted by declaring file path and then by element
// fully-qualified name, for stable output. An empty vector means the
// schema is conformant.
//
// Scope is the import closure, per draft -01 § Scope of Bind-Time
// Checks: a misplaced annotation on a message type declared in an
// imported .proto is reported here, whether or not any field of `desc`
// refers to that type. Violation::file names the file that declares the
// offending element, which need not be desc's own. The closure of any
// schema using the annotations includes pxf/annotations.proto and,
// through it, google/protobuf/descriptor.proto; callers that decode the
// same schema repeatedly and have validated it once can set
// UnmarshalOptions::skip_validate.
//
// The check is case-sensitive: identifiers such as "NULL" or "True"
// lex as ordinary identifiers and are accepted.
std::vector<Violation> ValidateDescriptor(const google::protobuf::Descriptor* desc);

// ValidateFile walks `fd` and its transitive imports, and returns every
// bind-time violation in that closure. See ValidateDescriptor for the
// rules and the scope. Each file in the closure is checked once (the
// diamond A → B, C → D reports D once).
std::vector<Violation> ValidateFile(const google::protobuf::FileDescriptor* fd);

// IsFutureReservedDirective returns true when `name` is one of the
// directive names the spec reserves for future allocation (draft
// §3.4.6): "table", "datasource", "view", "procedure", "function",
// "permissions". v1 decoders MUST reject these as unknown reserved
// directives. The names with their own production (`type`, `dataset`,
// `proto`) and the spec-registered `entry` aren't covered here —
// they're already handled either by the lexer or the named_directive
// shape.
bool IsFutureReservedDirective(std::string_view name);

}  // namespace protowire::pxf

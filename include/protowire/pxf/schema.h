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
// UnmarshalFull. Callers that have already validated their descriptors
// (typically via ValidateDescriptor in a one-time codegen or registry-
// load pass) may set UnmarshalOptions::skip_validate to bypass the
// per-call recheck.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>

namespace protowire::pxf {

// ViolationKind identifies which kind of schema element collides with a
// reserved PXF value keyword.
enum class ViolationKind : uint8_t {
  kField = 1,
  kOneof,
  kEnumValue,
};

const char* ViolationKindName(ViolationKind k);

// Violation describes one schema element whose name collides with a
// reserved PXF keyword. Returned by ValidateDescriptor / ValidateFile.
struct Violation {
  std::string file;     // .proto file path the offending element is declared in
  std::string element;  // fully-qualified protobuf name (e.g. "trades.v1.Side.null")
  std::string name;     // bare reserved identifier ("null" / "true" / "false")
  ViolationKind kind = ViolationKind::kField;

  // One-line human-readable description, e.g.
  //   "trades.proto: message field \"trades.v1.X.null\" uses PXF-reserved name \"null\" (draft
  //   §3.13)"
  std::string ToString() const;
};

// ValidateDescriptor walks the file containing `desc` and returns every
// reserved-name collision among messages, oneofs, and enum values
// reachable from that file. The returned vector is sorted by element
// fully-qualified name for stable output. An empty vector means the
// schema is conformant.
//
// The check is case-sensitive: identifiers such as "NULL" or "True"
// lex as ordinary identifiers and are accepted.
std::vector<Violation> ValidateDescriptor(const google::protobuf::Descriptor* desc);

// ValidateFile walks `fd` and returns every reserved-name collision in
// the file. See ValidateDescriptor for the rule and semantics.
std::vector<Violation> ValidateFile(const google::protobuf::FileDescriptor* fd);

}  // namespace protowire::pxf

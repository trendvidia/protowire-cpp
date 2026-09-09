// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <string>
#include <string_view>

#include <google/protobuf/descriptor.h>

namespace protowire::pxf {

// Resolves Any type URLs to message descriptors. Plug in by deriving from
// this interface and passing the instance via UnmarshalOptions / MarshalOptions.
class TypeResolver {
 public:
  virtual ~TypeResolver() = default;
  // Returns the message descriptor for the given type URL, or nullptr if not
  // resolvable. The returned pointer must outlive the codec.
  virtual const google::protobuf::Descriptor* FindMessageByURL(std::string_view type_url) = 0;
};

struct UnmarshalOptions {
  // If non-null, used to resolve google.protobuf.Any type URLs.
  TypeResolver* type_resolver = nullptr;
  // When true, unknown fields are silently ignored instead of returning an
  // error.
  bool discard_unknown = false;
  // When true, skip the per-call schema reserved-name check (draft §3.13).
  // Callers that have already validated their descriptors (typically via
  // ValidateDescriptor in a one-time codegen or registry-load pass) can
  // set this to bypass the per-call recheck.
  bool skip_validate = false;

  // HARDENING.md § Mandatory limits, each configurable per call; 0 selects
  // the default in protowire/limits.h. max_message_size caps the input to
  // this call and is checked before the first token is read;
  // max_nesting_depth caps block / list nesting (the root is depth 0, every
  // `{` or `[` one descent); max_numeric_literal_digits the digit count of
  // a literal bound to pxf.BigInt / Decimal / BigFloat;
  // max_bytes_literal_length the decoded length of any b"…" literal,
  // refused from the literal's length before it is decoded;
  // max_repeated_count the element count of any repeated or map field.
  int max_message_size = 0;
  int max_nesting_depth = 0;
  int max_numeric_literal_digits = 0;
  int max_bytes_literal_length = 0;
  int max_repeated_count = 0;
};

// ParseOptions carries the limits Parse enforces — the subset of
// UnmarshalOptions a schema-free parse can check. 0 selects the default in
// protowire/limits.h.
struct ParseOptions {
  int max_message_size = 0;
  int max_nesting_depth = 0;
  int max_bytes_literal_length = 0;
};

struct MarshalOptions {
  std::string indent = "  ";
  bool emit_defaults = false;
  std::string type_url;  // emit @type directive if non-empty
  TypeResolver* type_resolver = nullptr;
};

}  // namespace protowire::pxf

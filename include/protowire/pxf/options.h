// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <google/protobuf/descriptor.h>

#include "protowire/detail/status.h"

namespace protowire::pxf {

class Result;

// Resolves Any type URLs to message descriptors. Plug in by deriving from
// this interface and passing the instance via UnmarshalOptions / MarshalOptions.
class TypeResolver {
 public:
  virtual ~TypeResolver() = default;
  // Returns the message descriptor for the given type URL, or nullptr if not
  // resolvable. The returned pointer must outlive the codec.
  virtual const google::protobuf::Descriptor* FindMessageByURL(std::string_view type_url) = 0;
};

// SecretFieldHook receives the path and value of a pxf.Secret. A non-OK status
// aborts the decode, and its message is carried in the decode's error.
using SecretFieldHook = std::function<Status(std::string_view path, std::string_view value)>;

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
  // When true, UnmarshalFull leaves out the post-decode pass: required fields
  // are not checked and defaults are not applied, in the message or in an Any
  // body. For a caller that merges several documents and runs its own pass;
  // the Result carries what it needs. Unmarshal never runs that pass.
  bool skip_post_decode = false;
  // If set, called for each pxf.Secret's value in every form it can be
  // written: `pw = "x"`, list elements, map values, and the block form. The
  // value is then not stored in the message, and the caller routes it to
  // memory it controls. Presence is still recorded, so Result::IsSet reports
  // `pw.value` as set. `hint` and `fingerprint` go to the message as usual.
  //
  // The path is the dotted field path: "pw", "db.password", "servers[0].pw",
  // "by_host[\"a\"].pw", "payload.pw" in an Any body, "keys[1]" for a list
  // element. value is a view of a temporary, valid for the call. The plaintext
  // also stays in the input for the length of the decode.
  SecretFieldHook on_secret_field;

  // HARDENING.md § Mandatory limits, each configurable per call; 0 selects
  // the default in protowire/limits.h. max_message_size caps the input to
  // this call and is checked before the first token is read;
  // max_nesting_depth caps block / list nesting (the root is depth 0, every
  // `{` or `[` one descent); max_numeric_literal_digits the digit count of
  // a literal bound to pxf.BigInt / Decimal / BigFloat;
  // max_bytes_literal_length the decoded length of any b"…" literal,
  // refused from the literal's length before it is decoded;
  // max_repeated_count the element count of any repeated or map field;
  // max_big_float_exponent the magnitude of a pxf.BigFloat's binary exponent.
  int max_message_size = 0;
  int max_nesting_depth = 0;
  int max_numeric_literal_digits = 0;
  int max_bytes_literal_length = 0;
  int max_repeated_count = 0;
  int max_big_float_exponent = 0;
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
  // Fields to write as `field = null`, for a message with no _null mask set.
  // The Result must outlive the call.
  const Result* null_fields = nullptr;
};

}  // namespace protowire::pxf

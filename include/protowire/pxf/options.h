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
  virtual const google::protobuf::Descriptor* FindMessageByURL(
      std::string_view type_url) = 0;
};

struct UnmarshalOptions {
  // If non-null, used to resolve google.protobuf.Any type URLs.
  TypeResolver* type_resolver = nullptr;
  // When true, unknown fields are silently ignored instead of returning an
  // error.
  bool discard_unknown = false;
};

struct MarshalOptions {
  std::string indent = "  ";
  bool emit_defaults = false;
  std::string type_url;          // emit @type directive if non-empty
  TypeResolver* type_resolver = nullptr;
};

}  // namespace protowire::pxf

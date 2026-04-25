// PXF AST. Mirrors the Go module's encoding/pxf/ast.go.
//
// Entry and Value are sum types implemented with `std::variant` over node
// pointers. The variant alternatives are owned via std::unique_ptr so that
// recursive types (Block contains Entries; ListVal contains Values) work.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "protowire/pxf/token.h"

namespace protowire::pxf {

struct Comment {
  Position pos;
  std::string text;  // raw text including the comment prefix
};

// --- Forward declarations of node types (heap-allocated via unique_ptr) ----
struct StringVal;
struct IntVal;
struct FloatVal;
struct BoolVal;
struct BytesVal;
struct NullVal;
struct IdentVal;
struct TimestampVal;
struct DurationVal;
struct ListVal;
struct BlockVal;

// `Value` is a tagged pointer-variant; use std::unique_ptr to own the data
// and std::variant to discriminate.
using ValuePtr = std::variant<std::unique_ptr<StringVal>,
                              std::unique_ptr<IntVal>,
                              std::unique_ptr<FloatVal>,
                              std::unique_ptr<BoolVal>,
                              std::unique_ptr<BytesVal>,
                              std::unique_ptr<NullVal>,
                              std::unique_ptr<IdentVal>,
                              std::unique_ptr<TimestampVal>,
                              std::unique_ptr<DurationVal>,
                              std::unique_ptr<ListVal>,
                              std::unique_ptr<BlockVal>>;

struct StringVal {
  Position pos;
  std::string value;
};
struct IntVal {
  Position pos;
  std::string raw;  // raw text — decoded later by schema
};
struct FloatVal {
  Position pos;
  std::string raw;
};
struct BoolVal {
  Position pos;
  bool value = false;
};
struct BytesVal {
  Position pos;
  std::vector<uint8_t> value;
};
struct NullVal {
  Position pos;
};
struct IdentVal {
  Position pos;
  std::string name;
};
struct TimestampVal {
  Position pos;
  int64_t seconds = 0;
  int32_t nanos = 0;
  std::string raw;
};
struct DurationVal {
  Position pos;
  int64_t seconds = 0;
  int32_t nanos = 0;
  std::string raw;
};
struct ListVal {
  Position pos;
  std::vector<ValuePtr> elements;
};

// --- Entry types ----
struct Assignment;
struct MapEntry;
struct Block;

using EntryPtr = std::variant<std::unique_ptr<Assignment>,
                              std::unique_ptr<MapEntry>,
                              std::unique_ptr<Block>>;

struct Assignment {
  Position pos;
  std::string key;
  ValuePtr value;
  std::vector<Comment> leading_comments;
  std::string trailing_comment;
};
struct MapEntry {
  Position pos;
  std::string key;
  ValuePtr value;
  std::vector<Comment> leading_comments;
  std::string trailing_comment;
};
struct Block {
  Position pos;
  std::string name;
  std::vector<EntryPtr> entries;
  std::vector<Comment> leading_comments;
};
struct BlockVal {
  Position pos;
  std::vector<EntryPtr> entries;
};

struct Document {
  std::string type_url;             // empty if no @type directive
  std::vector<EntryPtr> entries;
  std::vector<Comment> leading_comments;
};

Position EntryPos(const EntryPtr& e);
Position ValuePos(const ValuePtr& v);

}  // namespace protowire::pxf

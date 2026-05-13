// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// PXF AST. Mirrors the Go module's encoding/pxf/ast.go.
//
// Entry and Value are sum types implemented with `std::variant` over node
// pointers. The variant alternatives are owned via std::unique_ptr so that
// recursive types (Block contains Entries; ListVal contains Values) work.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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

using EntryPtr =
    std::variant<std::unique_ptr<Assignment>, std::unique_ptr<MapEntry>, std::unique_ptr<Block>>;

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

// Directive is a top-of-document `@<name> *(<prefix-id>) [{ ... }]`
// entry. The canonical use is side-channel metadata that sits alongside
// the schema-typed body — e.g. chameleon's
// `@header chameleon.v1.LayerHeader { id = "x" }` — but the grammar is
// open-ended: any name not in the spec-reserved set (draft §3.4.6) is
// parsed as a generic Directive. Prefix identifiers are positional
// and per-directive.
//
// Specific registrations:
//   - One prefix (v0.72.0 conventional shape) — names the inner block's
//     message type (dotted), e.g. `@header chameleon.v1.LayerHeader { ... }`.
//   - `@entry` (draft §3.4.3) — zero, one, or two prefix identifiers
//     (label, type); a single prefix is disambiguated by the presence
//     of a `.` (dotted ⇒ type; bare ⇒ label).
//
// `body` holds the raw bytes between the opening `{` and matching `}`
// (both exclusive) — empty when the directive has no inline block.
struct Directive {
  Position pos;
  std::string name;                   // e.g. "header"; never a spec-reserved name (§3.4.6)
  std::vector<std::string> prefixes;  // identifiers between @<name> and the optional `{ ... }`
  // Back-compat: when exactly one prefix identifier was supplied, `type`
  // holds it (matching v0.72.0's single-Type shape). Empty otherwise.
  std::string type;
  std::string body;  // raw inner bytes of the block; empty if no `{ ... }`
  bool has_body = false;
  std::vector<Comment> leading_comments;
};

// DatasetRow is one parenthesized cell tuple in a `@dataset` directive.
// `cells` is the same length as the containing DatasetDirective.columns.
// A `std::nullopt` cell denotes an absent field (the "empty cell"
// between two commas); a non-empty optional holding a `NullVal` denotes
// a present-but-null field; any other Value denotes a present field.
struct DatasetRow {
  Position pos;
  std::vector<std::optional<ValuePtr>> cells;
};

// DatasetDirective is a `@dataset <type> ( col1, col2, ... ) row*` entry
// at document root (draft §3.4.4). It carries many instances of one
// message type in a single document — the protowire-native CSV
// replacement.
//
// Per draft §3.4.4, a document with any DatasetDirective MUST NOT have
// a @type directive or any top-level field entries: the @dataset header
// IS the document's type declaration. Decoders enforce this in Parse.
//
// `type` MAY be empty when an anonymous `@proto` directive (§3.4.5)
// precedes the dataset in document order; the anonymous schema is
// consumed as the row message type.
struct DatasetDirective {
  Position pos;
  std::string type;                  // row message type, e.g. "trades.v1.Trade"
  std::vector<std::string> columns;  // top-level field names on `type`; len >= 1
  std::vector<DatasetRow> rows;      // zero or more rows
  std::vector<Comment> leading_comments;
};

// ProtoShape distinguishes the four body shapes of a @proto directive
// (draft §3.4.5).
enum class ProtoShape : uint8_t {
  // `@proto { <message-body> }` — defines an unnamed message used by
  // the next typed directive in document order.
  kAnonymous = 0,
  // `@proto <dotted-name> { <message-body> }` — sugar for a single named
  // message; `type_name` carries the dotted name.
  kNamed,
  // `@proto """<proto-source>"""` — complete .proto source file.
  kSource,
  // `@proto b"<base64-FileDescriptorSet>"` — base64-encoded
  // google.protobuf.FileDescriptorSet bytes.
  kDescriptor,
};

const char* ProtoShapeName(ProtoShape s);

// ProtoDirective is a `@proto <body>` entry at document root
// (draft §3.4.5). It carries an embedded protobuf schema, making the
// PXF document self-describing.
//
// `body` carries raw bytes per shape:
//   - kAnonymous, kNamed: bytes between the opening `{` and matching
//     `}` (both exclusive). The bytes are protobuf message-body source.
//   - kSource: contents of the triple-quoted string (with leading-LF
//     stripping and common-prefix dedent applied). The bytes are a
//     complete .proto source file.
//   - kDescriptor: base64-decoded bytes of the bytes literal. The
//     bytes are a serialised google.protobuf.FileDescriptorSet.
//
// `type_name` is non-empty only when `shape == kNamed`.
struct ProtoDirective {
  Position pos;
  ProtoShape shape = ProtoShape::kAnonymous;
  std::string type_name;
  std::string body;
  std::vector<Comment> leading_comments;
};

struct Document {
  std::string type_url;  // empty if no @type directive
  std::vector<Directive> directives;  // @<name> directives in source order; excludes spec-defined
  std::vector<DatasetDirective> datasets;  // @dataset directives in source order (draft §3.4.4)
  std::vector<ProtoDirective> protos;      // @proto directives in source order (draft §3.4.5)
  int body_offset =
      0;  // byte offset where the schema-typed body begins (after all leading directives)
  std::vector<EntryPtr> entries;
  std::vector<Comment> leading_comments;
};

Position EntryPos(const EntryPtr& e);
Position ValuePos(const ValuePtr& v);

}  // namespace protowire::pxf

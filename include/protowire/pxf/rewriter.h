// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Format-preserving edits to a PXF document. C++ port of protowire-go's
// encoding/pxf/rewrite.go and rewrite_keyed.go.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "protowire/detail/status.h"
#include "protowire/pxf/ast.h"

namespace protowire::pxf {

// Rewriter applies targeted, format-preserving edits to a PXF document. Unlike
// FormatDocument, which normalizes the whole document, a Rewriter splices
// replacement bytes into the original source, so everything outside the edited
// spans (comments, blank lines, key ordering, indentation, number and string
// formatting quirks) round-trips byte for byte. This is the write path for
// tools that machine-edit documents a human also hand-edits.
//
//   auto r = pxf::Rewriter::New(src);
//   if (!r.ok()) return r.status();
//   (*r)->Set("server.port", ValuePtr(std::make_unique<IntVal>(IntVal{.raw = "9090"})));
//   (*r)->Remove("server.debug");
//   StatusOr<std::string> out = (*r)->Bytes();
//
// Fields are addressed by dotted path. Each segment names an Assignment key,
// Block name or MapEntry key in the enclosing scope, and the first match wins.
// Keys that themselves contain a dot cannot be addressed. Edits are computed
// against the document as originally parsed: staging an edit does not make new
// fields addressable by later calls, and two Sets that both create the same
// missing intermediate block will create it twice (stage one Set with a
// BlockVal value instead).
//
// Keyed collections (draft -01 §3.13) have their own editors, SetKeyed,
// RemoveKeyed, InsertKeyedElement, RemoveKeyedElement, RenameKeyedElement and
// MoveKeyedElement, that take the element key as an opaque atom, since entry
// names may contain dots and need not be identifier-shaped. They cover a field
// bound more than once (§3.13 concatenation) by searching every binding, and
// the anonymous list form (`f = [ { k = "x" } ]`) once its key field is named
// with KeyedByField. The keyed-block form needs no declaration.
//
// A value or entry argument is rendered, or copied, before the call returns,
// so it may be a temporary or a node of Document(). A default-constructed
// ValuePtr or EntryPtr holds a null pointer and is Go's nil.
//
// A Rewriter is not safe for concurrent use.
class Rewriter {
 public:
  // New parses src strictly, via Parse, and returns a Rewriter over it. The
  // source must be valid PXF: a broken document has no stable spans to splice
  // into. Use ParseTolerant for read-side tooling on broken buffers.
  static StatusOr<std::unique_ptr<Rewriter>> New(std::string src);

  Rewriter(const Rewriter&) = delete;
  Rewriter& operator=(const Rewriter&) = delete;
  ~Rewriter();

  // Document returns the parsed document the Rewriter addresses paths against.
  // It reflects the original source, not staged edits. It lives as long as the
  // Rewriter does.
  const pxf::Document* Document() const { return &doc_; }

  // Set stages an upsert of the field at path to the given value. If the path
  // resolves to an existing Assignment or MapEntry, only that entry's value
  // span is replaced: the key, separator, alignment and any trailing comment
  // stay put. If the path (or a suffix of it) does not exist, the missing chain
  // is inserted at the end of the deepest existing enclosing block, matching
  // its sibling indentation.
  //
  // Setting a path that resolves to a Block is an error: replacing a block
  // wholesale would discard the comments inside it. Set its leaf fields
  // individually, or Remove it and Set a BlockVal.
  //
  // A Rewriter works on syntax alone. An inserted leaf uses the ':' map form
  // when an existing sibling is a map entry and the '=' field form otherwise,
  // so inserting the first entry of a map into an empty block writes '=' where
  // the schema may expect ':'. Prefer seeding empty map blocks with a BlockVal
  // of MapEntry values.
  //
  // Calling Set again with the same path replaces the previously staged edit
  // for that path.
  Status Set(std::string_view path, const ValuePtr& v);

  // Remove stages the deletion of the entry at path. When the entry sits alone
  // on its line(s), the whole lines are removed, including a trailing comment
  // on the entry's last line. Leading comment lines above the entry are kept
  // (they may describe the surrounding group). When the entry shares a line
  // with other content, only its exact span is removed. Removing a path that
  // does not exist is an error.
  Status Remove(std::string_view path);

  // ReplaceValue stages a replacement of the source span of an existing value
  // node with the rendering of with. Unlike Set, which addresses a value by a
  // first-match dotted path, ReplaceValue targets the exact node the caller
  // holds from Document(), so it can change one node among siblings that share
  // a key.
  //
  // old must be a value parsed from this Rewriter's source (it carries the span
  // to replace). A hand-built value or a BadVal has no span and is rejected.
  // Continuation lines of a multi-line replacement are re-indented to old's
  // line, matching Set. Calling ReplaceValue again for the same node replaces
  // the previously staged edit.
  Status ReplaceValue(const ValuePtr& old, const ValuePtr& with);

  // SetSpan stages a replacement of src[start:end) with text, the raw escape
  // hatch beneath ReplaceValue and Set. start == end is a pure insertion and an
  // empty text a pure deletion. Routing edits through the Rewriter keeps overlap
  // detection, edit batching and the reparse safety net in Bytes. text is
  // copied. Staging another edit over the identical span replaces this one.
  Status SetSpan(int start, int end, std::string_view text);

  // AppendEntry stages the insertion of e as the last entry of block, formatted
  // to match the block's existing entries. Multi-line blocks get a new line
  // indented like the siblings (or the block's own indent plus one level when
  // the block is empty), placed just before the closing brace. An inline block
  // gets the entry spliced inline before the brace. block must be a node from
  // this Rewriter's Document(). Repeated calls into one block accumulate in
  // call order.
  Status AppendEntry(const Block* block, const EntryPtr& e);

  // Bytes applies the staged edits and returns the rewritten document. Edits
  // must not overlap (a Remove of a block combined with a Set inside that
  // block, say). Overlapping edits are reported as an error. As a safety net
  // for writer bugs, the result is reparsed before being returned: a rewrite
  // that produces invalid PXF is an error.
  StatusOr<std::string> Bytes() const;

  // KeyedByField declares that the keyed collection at field_path uses
  // key_field as its (pxf.key) field. This lets the keyed editors also address
  // a collection written in the anonymous list form, where an element's key is
  // the value of the key field rather than an entry name. The keyed-block form
  // needs no declaration. Declarations are matched by exact field_path, and a
  // later call for the same field_path replaces the earlier one. Returns the
  // Rewriter for chaining.
  Rewriter& KeyedByField(std::string_view field_path, std::string_view key_field);

  // SetKeyed stages an upsert of the field at subpath inside the element named
  // key of the keyed collection at field_path: the keyed analogue of Set. When
  // no element named key exists, a new element is appended to the collection's
  // last binding, in its form. As with Set, edits are computed against the
  // document as parsed, so two SetKeyed calls that both create the same missing
  // element create it twice. Calling SetKeyed again with the same field_path,
  // key and subpath replaces the previously staged edit.
  Status SetKeyed(std::string_view field_path,
                  std::string_view key,
                  std::string_view subpath,
                  const ValuePtr& v);

  // RemoveKeyed stages the deletion of the field at subpath inside the element
  // named key: the keyed analogue of Remove.
  Status RemoveKeyed(std::string_view field_path, std::string_view key, std::string_view subpath);

  // RemoveKeyedElement stages the deletion of the whole element named key. In
  // the keyed-block form the element is removed like any entry. In the
  // anonymous list form the element and its separating comma are removed.
  Status RemoveKeyedElement(std::string_view field_path, std::string_view key);

  // InsertKeyedElement stages the insertion of a new element named key with the
  // given body entries. With an empty before the element is appended to the
  // last binding. Otherwise it is inserted immediately before the existing
  // element named before, in whatever binding holds it and in that binding's
  // form. Inserting a key that already names an element, anywhere across the
  // field's bindings, is an error.
  Status InsertKeyedElement(std::string_view field_path,
                            std::string_view key,
                            std::string_view before,
                            const std::vector<EntryPtr>& body);

  // RenameKeyedElement stages a retargeting of the element named old_key to
  // new_key. In the keyed-block form only the name token is replaced, quoted or
  // unquoted per the canonical rule (§3.13). In the anonymous list form the key
  // field's value literal is replaced instead. Renaming to a key that already
  // names a sibling element is an error. Renaming an element to its current key
  // stages nothing.
  Status RenameKeyedElement(std::string_view field_path,
                            std::string_view old_key,
                            std::string_view new_key);

  // MoveKeyedElement stages a reorder of the element named key: its source text
  // moves verbatim. In the keyed-block form its glued leading `#` and `//`
  // comment lines travel with it. In the anonymous list form the element value
  // and one separating comma move. With an empty before the element moves to
  // the end of the collection. Otherwise it lands immediately before the
  // existing element named before. An element and its anchor must share the
  // same surface form. Moving an element before its current successor
  // reproduces the input.
  Status MoveKeyedElement(std::string_view field_path,
                          std::string_view key,
                          std::string_view before);

 private:
  // SpanEdit replaces src[start:end) with text. start == end is a pure
  // insertion and an empty text a pure deletion.
  struct SpanEdit {
    std::string path;
    int start = 0;
    int end = 0;
    std::string text;
  };
  // Container is what Go reads from the entry owning a scope: where that entry
  // starts and where the scope's closing '}' sits. Go's anonymous elements
  // build a synthetic Assignment over a BlockVal for this. An Assignment here
  // owns its value, so the scope is carried as these two offsets instead.
  struct Container {
    int pos = 0;
    int close_off = 0;
  };
  struct Target;
  struct KeyedBinding;
  struct KeyedElem;
  struct Insertion {
    int at = 0;
    std::string text;
  };
  struct RemovalSpan {
    int start = 0;
    int end = 0;
    bool whole_lines = false;
  };
  using Entries = std::vector<EntryPtr>;

  Rewriter(std::string src, pxf::Document doc) : src_(std::move(src)), doc_(std::move(doc)) {}

  int Len() const { return static_cast<int>(src_.size()); }
  std::string_view Slice(int start, int end) const;

  Status SetResolved(const std::string& op,
                     const std::string& path_key,
                     const Target& t,
                     const ValuePtr& v);
  RemovalSpan EntryRemovalSpan(const EntryPtr& e) const;
  Insertion RenderAppendEntry(int scope_start,
                              int close_off,
                              const Entries& siblings,
                              const EntryPtr& e) const;
  void StageInsertEdit(int off, std::string text);
  void Stage(SpanEdit e);
  StatusOr<Target> Resolve(std::string_view path) const;
  StatusOr<Target> ResolveIn(const Entries& entries,
                             std::optional<Container> container,
                             std::string_view path) const;
  Status StageInsert(const std::string& op,
                     const std::string& path_key,
                     const Target& t,
                     const ValuePtr& v);
  std::string RenderChain(const std::vector<std::string>& missing,
                          const std::string& base_indent,
                          const std::string& step,
                          bool map_form,
                          const ValuePtr& v,
                          bool is_inline) const;
  std::string RenderValue(const ValuePtr& v,
                          const std::string& line_prefix,
                          const std::string& step) const;
  std::string SiblingIndent(const Target& t) const;
  std::string ChildIndent(const Entries& entries, int container_pos) const;
  std::string StepFor(const Entries& entries) const;
  std::string IndentStep() const;
  std::string ScanIndentStep(const Entries& entries) const;
  std::string LineIndentAt(int off) const;
  std::string LineLeadingIndent(int off) const;

  StatusOr<std::vector<KeyedBinding>> KeyedBindings(const std::string& op,
                                                    std::string_view field_path) const;
  Status KeyedScopeEntries(std::string_view field_path,
                           const Entries** scope,
                           std::string* key) const;
  Insertion RenderAppendKeyedElement(const KeyedBinding& b,
                                     std::string_view key,
                                     Entries body) const;
  Insertion RenderInsertBlockElementBefore(const KeyedBinding& b,
                                           const EntryPtr& anchor,
                                           const EntryPtr& nb) const;
  Status MoveBlockElement(const std::string& op,
                          std::string_view field_path,
                          const KeyedElem& elem,
                          const KeyedElem* anchor);
  Status MoveListElement(const std::string& op,
                         std::string_view field_path,
                         const KeyedElem& elem,
                         const KeyedElem* anchor);
  bool ListMultiline(const ListVal& lv) const;
  std::string ListElementIndent(const ListVal& lv, int scope_start) const;
  std::pair<int, int> ListElementRemovalSpan(const ListVal& lv, int idx) const;
  int TrailingListPad(int off) const;
  std::string RenderListElement(const KeyedBinding& b, const ValuePtr& bv) const;
  Insertion SpliceListElementAppend(const KeyedBinding& b, const std::string& elem_text) const;
  Insertion SpliceListElementBefore(const KeyedBinding& b,
                                    int idx,
                                    const std::string& elem_text) const;
  int ExtendOverLeadingComments(int start) const;

  std::string src_;
  pxf::Document doc_;
  std::vector<SpanEdit> edits_;
  // insert_seq_ gives each AppendEntry a distinct edit identity so repeated
  // appends into one block accumulate rather than deduplicating.
  int insert_seq_ = 0;
  // key_fields_ records the (pxf.key) field name declared for a keyed
  // collection at a dotted field path via KeyedByField.
  std::map<std::string, std::string, std::less<>> key_fields_;
};

}  // namespace protowire::pxf

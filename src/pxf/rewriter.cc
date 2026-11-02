// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// C++ port of protowire-go's encoding/pxf/rewrite.go and rewrite_keyed.go.
// Function for function, in Go's order, so a later change in Go maps across.
// Error text matches Go byte for byte; scripts/go_differential.sh compares it.

#include "protowire/pxf/rewriter.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "format_internal.h"
#include "protowire/detail/go_quote.h"
#include "protowire/pxf/format.h"
#include "protowire/pxf/keyed.h"
#include "protowire/pxf/parser.h"

namespace protowire::pxf {

namespace {

using Entries = std::vector<EntryPtr>;
using detail::GoQuote;

template <class T>
const T* As(const ValuePtr& v) {
  const auto* p = std::get_if<std::unique_ptr<T>>(&v);
  return p ? p->get() : nullptr;
}

template <class T>
const T* As(const EntryPtr& e) {
  const auto* p = std::get_if<std::unique_ptr<T>>(&e);
  return p ? p->get() : nullptr;
}

bool IsNil(const ValuePtr& v) {
  return std::visit([](const auto& p) { return p == nullptr; }, v);
}

bool IsNil(const EntryPtr& e) {
  return std::visit([](const auto& p) { return p == nullptr; }, e);
}

ValuePtr CloneValue(const ValuePtr& v);

Entries CloneEntries(const Entries& entries);

// The AST owns its children, where Go's shares them, so a value or entry going
// into a freshly built node is copied.
EntryPtr CloneEntry(const EntryPtr& e) {
  if (IsNil(e)) return EntryPtr{};
  if (const auto* a = As<Assignment>(e)) {
    auto n = std::make_unique<Assignment>();
    n->pos = a->pos;
    n->end = a->end;
    n->key = a->key;
    n->key_quoted = a->key_quoted;
    n->value = CloneValue(a->value);
    n->leading_comments = a->leading_comments;
    n->trailing_comment = a->trailing_comment;
    return n;
  }
  if (const auto* m = As<MapEntry>(e)) {
    auto n = std::make_unique<MapEntry>();
    n->pos = m->pos;
    n->end = m->end;
    n->key = m->key;
    n->key_quoted = m->key_quoted;
    n->value = CloneValue(m->value);
    n->leading_comments = m->leading_comments;
    n->trailing_comment = m->trailing_comment;
    return n;
  }
  if (const auto* b = As<Block>(e)) {
    auto n = std::make_unique<Block>();
    n->pos = b->pos;
    n->end = b->end;
    n->name = b->name;
    n->name_quoted = b->name_quoted;
    n->entries = CloneEntries(b->entries);
    n->leading_comments = b->leading_comments;
    return n;
  }
  return std::make_unique<TypeEntry>(*As<TypeEntry>(e));
}

Entries CloneEntries(const Entries& entries) {
  Entries out;
  out.reserve(entries.size());
  for (const auto& e : entries) out.push_back(CloneEntry(e));
  return out;
}

ValuePtr CloneValue(const ValuePtr& v) {
  if (IsNil(v)) return ValuePtr{};
  if (const auto* l = As<ListVal>(v)) {
    auto n = std::make_unique<ListVal>();
    n->pos = l->pos;
    n->end = l->end;
    n->elements.reserve(l->elements.size());
    for (const auto& e : l->elements) n->elements.push_back(CloneValue(e));
    return n;
  }
  if (const auto* b = As<BlockVal>(v)) {
    auto n = std::make_unique<BlockVal>();
    n->pos = b->pos;
    n->end = b->end;
    n->entries = CloneEntries(b->entries);
    return n;
  }
  return std::visit(
      [](const auto& p) -> ValuePtr {
        using T = typename std::decay_t<decltype(p)>::element_type;
        if constexpr (std::is_same_v<T, ListVal> || std::is_same_v<T, BlockVal>) {
          return ValuePtr{};  // returned above
        } else {
          return std::make_unique<T>(*p);
        }
      },
      v);
}

// --- rewrite.go helpers ----------------------------------------------------

// SpanPath is the dedup key for a span-addressed edit. The NUL prefix keeps it
// distinct from every dotted path.
std::string SpanPath(int start, int end) {
  std::string out(1, '\0');
  out += "span:" + std::to_string(start) + ":" + std::to_string(end);
  return out;
}

// EntryKey returns the key (assignment / map entry) or name (block) an entry is
// addressed by.
std::string_view EntryKey(const EntryPtr& e) {
  if (const auto* n = As<Assignment>(e)) return n->key;
  if (const auto* n = As<MapEntry>(e)) return n->key;
  if (const auto* n = As<Block>(e)) return n->name;
  return "";
}

// EntryValue returns the value of a leaf entry, or nullptr for a Block, which
// has entries rather than a value.
const ValuePtr* EntryValue(const EntryPtr& e) {
  if (const auto* n = As<Assignment>(e)) return &n->value;
  if (const auto* n = As<MapEntry>(e)) return &n->value;
  return nullptr;
}

// FindEntry returns the first entry in entries whose key or block name is key.
const EntryPtr* FindEntry(const Entries& entries, std::string_view key) {
  for (const auto& e : entries) {
    if (EntryKey(e) == key) return &e;
  }
  return nullptr;
}

bool EntryContainsBadVal(const EntryPtr& e);

// ContainsBadVal reports whether v is or contains a BadVal placeholder anywhere
// in its value tree.
bool ContainsBadVal(const ValuePtr& v) {
  if (As<BadVal>(v) != nullptr) return true;
  if (const auto* n = As<ListVal>(v)) {
    for (const auto& e : n->elements) {
      if (ContainsBadVal(e)) return true;
    }
  }
  if (const auto* n = As<BlockVal>(v)) {
    for (const auto& e : n->entries) {
      if (EntryContainsBadVal(e)) return true;
    }
  }
  return false;
}

// IndentAt returns base plus depth levels of step.
std::string IndentAt(const std::string& base, const std::string& step, int depth) {
  std::string out = base;
  for (int i = 0; i < depth; ++i) out += step;
  return out;
}

void ReplaceNewlines(std::string* s, const std::string& line_prefix) {
  std::string out;
  out.reserve(s->size());
  for (char c : *s) {
    out.push_back(c);
    if (c == '\n') out += line_prefix;
  }
  *s = std::move(out);
}

// RenderEntryLines formats a single entry for insertion into a multi-line
// block: fully formatted (nested blocks indented by step per level), every line
// prefixed with base_indent, and no trailing newline.
std::string RenderEntryLines(const EntryPtr& e,
                             const std::string& base_indent,
                             const std::string& step) {
  std::string out;
  AppendEntryIndented(&out, e, step);
  if (!out.empty() && out.back() == '\n') out.pop_back();
  if (!base_indent.empty()) {
    ReplaceNewlines(&out, base_indent);
    out.insert(0, base_indent);
  }
  return out;
}

// WriteEntryInline renders an entry on a single line for insertion into an
// inline block, e.g. `a = 1`, `"k": "v"`, or `n { a = 1 }`.
void WriteEntryInline(std::string* buf, const EntryPtr& e) {
  if (IsNil(e)) return;
  if (const auto* n = As<Assignment>(e)) {
    *buf += n->key_quoted ? GoQuote(n->key) : n->key;
    *buf += " = ";
    AppendValue(buf, n->value);
  } else if (const auto* n = As<MapEntry>(e)) {
    *buf += MapKeyQuoted(*n) ? GoQuote(n->key) : n->key;
    *buf += ": ";
    AppendValue(buf, n->value);
  } else if (const auto* n = As<TypeEntry>(e)) {
    *buf += "@type = " + GoQuote(n->url);
  } else if (const auto* n = As<Block>(e)) {
    *buf += n->name_quoted ? GoQuote(n->name) : n->name;
    *buf += " {";
    for (const auto& c : n->entries) {
      buf->push_back(' ');
      WriteEntryInline(buf, c);
    }
    *buf += " }";
  }
}

// EntryContainsBadVal reports whether e holds a BadVal anywhere in its value
// tree, nested block entries included.
bool EntryContainsBadVal(const EntryPtr& e) {
  if (const auto* n = As<Assignment>(e)) return ContainsBadVal(n->value);
  if (const auto* n = As<MapEntry>(e)) return ContainsBadVal(n->value);
  if (const auto* n = As<Block>(e)) {
    for (const auto& c : n->entries) {
      if (EntryContainsBadVal(c)) return true;
    }
  }
  return false;
}

// HasMapEntry reports whether any sibling is a ':'-form map entry, which
// decides the separator for inserted leaves.
bool HasMapEntry(const Entries& entries) {
  for (const auto& e : entries) {
    if (As<MapEntry>(e) != nullptr) return true;
  }
  return false;
}

// LineStartOffset returns the offset of the first byte of the line holding off.
int LineStartOffset(std::string_view src, int off) {
  int i = off;
  while (i > 0 && src[i - 1] != '\n') --i;
  return i;
}

// IsBlank reports whether b is all spaces and tabs.
bool IsBlank(std::string_view b) {
  return std::all_of(b.begin(), b.end(), [](char c) { return c == ' ' || c == '\t'; });
}

bool NeedsQuoting(std::string_view s) {
  return !IsIdentifierSafe(s);
}

bool HasNewline(std::string_view s) {
  return s.find('\n') != std::string_view::npos;
}

// strings.Split(path, ".").
std::vector<std::string> SplitDots(std::string_view path) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    size_t dot = path.find('.', start);
    if (dot == std::string_view::npos) {
      out.emplace_back(path.substr(start));
      return out;
    }
    out.emplace_back(path.substr(start, dot - start));
    start = dot + 1;
  }
}

// --- rewrite_keyed.go helpers ----------------------------------------------

// SplitPathSegs splits a dotted path into segments, returning nullopt when the
// path is empty or has an empty segment.
std::optional<std::vector<std::string>> SplitPathSegs(std::string_view path) {
  if (path.empty()) return std::nullopt;
  std::vector<std::string> segs = SplitDots(path);
  for (const auto& s : segs) {
    if (s.empty()) return std::nullopt;
  }
  return segs;
}

// NewKeyedElementBlock builds the AST node for a fresh keyed-block-form
// element, quoting the entry name exactly when it is not identifier-safe.
EntryPtr NewKeyedElementBlock(std::string_view key, Entries body) {
  auto b = std::make_unique<Block>();
  b->name = std::string(key);
  b->name_quoted = !IsIdentifierSafeEntryName(key);
  b->entries = std::move(body);
  return b;
}

// NewAnonElement builds the block value for a fresh anonymous-form element: the
// key field written explicitly first, then the body.
ValuePtr NewAnonElement(const std::string& key_field, std::string_view key, Entries body) {
  auto sv = std::make_unique<StringVal>();
  sv->value = std::string(key);
  auto a = std::make_unique<Assignment>();
  a->key = key_field;
  a->value = std::move(sv);
  auto bv = std::make_unique<BlockVal>();
  bv->entries.reserve(body.size() + 1);
  bv->entries.emplace_back(std::move(a));
  for (auto& e : body) bv->entries.push_back(std::move(e));
  return bv;
}

// BuildEntryChain turns a dotted subpath and a leaf value into nested block
// entries: `a.b.c` becomes `a { b { c = v } }`. Every segment must be a bare
// identifier, since a fresh element has no map context to justify a quoted key.
StatusOr<Entries> BuildEntryChain(const std::string& op,
                                  std::string_view subpath,
                                  const ValuePtr& v) {
  auto segs = SplitPathSegs(subpath);
  if (!segs) {
    return Status::Error("pxf: " + op + ": invalid subpath " + GoQuote(subpath) +
                         ": empty segment");
  }
  for (const auto& seg : *segs) {
    if (NeedsQuoting(seg)) {
      return Status::Error("pxf: " + op + ": segment " + GoQuote(seg) +
                           " is not a valid field name");
    }
  }
  auto leaf = std::make_unique<Assignment>();
  leaf->key = segs->back();
  leaf->value = CloneValue(v);
  EntryPtr e = std::move(leaf);
  for (int i = static_cast<int>(segs->size()) - 2; i >= 0; --i) {
    auto b = std::make_unique<Block>();
    b->name = (*segs)[i];
    b->entries.push_back(std::move(e));
    e = std::move(b);
  }
  Entries out;
  out.push_back(std::move(e));
  return out;
}

// AnonKeyAssignment returns the element's key-field assignment (the
// `key_field = "…"` entry), or nullptr.
const Assignment* AnonKeyAssignment(const Entries& entries, std::string_view key_field) {
  for (const auto& e : entries) {
    const auto* a = As<Assignment>(e);
    if (a != nullptr && !a->key_quoted && a->key == key_field && As<StringVal>(a->value)) {
      return a;
    }
  }
  return nullptr;
}

bool IsIdentPartByte(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9') ||
         c == '.';
}

// SkipDirString returns the offset just past the string literal at i, or -1.
// Go's skipDirString.
int SkipDirString(std::string_view input, int i) {
  const int n = static_cast<int>(input.size());
  if (i + 2 < n && input[i + 1] == '"' && input[i + 2] == '"') {
    for (int j = i + 3; j + 2 < n; ++j) {
      if (input[j] == '"' && input[j + 1] == '"' && input[j + 2] == '"') return j + 3;
    }
    return -1;
  }
  int j = i + 1;
  while (j < n) {
    if (input[j] == '\\') {
      if (j + 1 >= n) return -1;
      j += 2;
      continue;
    }
    if (input[j] == '"') return j + 1;
    if (input[j] == '\n') return -1;
    ++j;
  }
  return -1;
}

// NameTokenEnd returns the offset just past the entry-name token that starts at
// start: a string literal (quoted entry name) or an identifier run. Returns -1
// when no token is recognizable there.
int NameTokenEnd(std::string_view src, int start) {
  const int n = static_cast<int>(src.size());
  if (start >= n) return -1;
  if (src[start] == '"') return SkipDirString(src, start);
  int i = start;
  while (i < n && IsIdentPartByte(src[i])) ++i;
  return i == start ? -1 : i;
}

// JoinPathSegs rejoins path segments with dots.
std::string JoinPathSegs(const std::vector<std::string>& segs, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) out.push_back('.');
    out += segs[i];
  }
  return out;
}

// SpaceRuneLen returns the width of the unicode.IsSpace rune at the start of s,
// or 0. bytes.TrimSpace trims these, the non-ASCII ones included.
int SpaceRuneLen(std::string_view s) {
  static constexpr std::string_view kWide[] = {
      "\xc2\x85",     "\xc2\xa0",     "\xe1\x9a\x80", "\xe2\x80\x80", "\xe2\x80\x81",
      "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86",
      "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
      "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80",
  };
  if (s.empty()) return 0;
  switch (s[0]) {
    case ' ':
    case '\t':
    case '\n':
    case '\v':
    case '\f':
    case '\r':
      return 1;
    default:
      break;
  }
  for (std::string_view w : kWide) {
    if (s.substr(0, w.size()) == w) return static_cast<int>(w.size());
  }
  return 0;
}

}  // namespace

// Target is the result of resolving a dotted path: either a concrete entry, or
// the deepest existing scope plus the missing trailing segments.
struct Rewriter::Target {
  const EntryPtr* entry = nullptr;     // non-null when the full path resolved
  std::optional<Container> container;  // innermost resolved ancestor; unset = top level
  const Entries* container_entries{};  // the entry list the last lookup ran in
  std::vector<std::string> missing;    // unresolved trailing segments
};

// KeyedBinding is one binding of a keyed field, a keyed block or an anonymous
// list, together with its source span.
struct Rewriter::KeyedBinding {
  bool anon = false;
  int scope_start = 0;            // offset of the binding's container entry's first byte
  int close_off = 0;              // block form: '}' offset; anonymous form: ']' offset
  const Entries* entries{};       // block form: the block's entries
  const ListVal* list = nullptr;  // anonymous form: the list value
  std::string key_field;          // anonymous form: the (pxf.key) field name

  // Each visits every addressable element in document order until fn returns
  // false.
  template <class F>
  void Each(F fn) const;
};

// KeyedElem is one located element within a binding.
struct Rewriter::KeyedElem {
  const KeyedBinding* b = nullptr;
  std::string key;
  const EntryPtr* node = nullptr;  // block form: the Block or Assignment entry
  int idx = 0;                     // anonymous form: index into b->list->elements
  const BlockVal* bv = nullptr;    // body as a block value (anonymous, block form `name = {}`)
  const Entries* body = nullptr;   // the element's body entries, for subpath resolution

  // ContainerOf returns the scope holding the element's body, for subpath
  // resolution and value edits.
  Rewriter::Container ContainerOf() const {
    if (node != nullptr) {
      if (const auto* b_node = As<Block>(*node)) {
        return {b_node->pos.offset, b_node->end.offset - 1};
      }
      return {EntryPos(*node).offset, bv->end.offset - 1};
    }
    return {bv->pos.offset, bv->end.offset - 1};
  }
};

template <class F>
void Rewriter::KeyedBinding::Each(F fn) const {
  if (anon) {
    for (size_t i = 0; i < list->elements.size(); ++i) {
      const auto* bv = As<BlockVal>(list->elements[i]);
      if (bv == nullptr) continue;
      std::string key;
      if (!ExplicitKeyOf(bv->entries, key_field, &key) || key.empty()) continue;
      KeyedElem e;
      e.b = this;
      e.key = key;
      e.idx = static_cast<int>(i);
      e.bv = bv;
      e.body = &bv->entries;
      if (!fn(e)) return;
    }
    return;
  }
  for (const auto& entry : *entries) {
    if (const auto* n = As<Block>(entry)) {
      KeyedElem e;
      e.b = this;
      e.key = n->name;
      e.node = &entry;
      e.body = &n->entries;
      if (!fn(e)) return;
    } else if (const auto* n = As<Assignment>(entry)) {
      if (const auto* bv = As<BlockVal>(n->value)) {
        KeyedElem e;
        e.b = this;
        e.key = n->key;
        e.node = &entry;
        e.bv = bv;
        e.body = &bv->entries;
        if (!fn(e)) return;
      }
    }
  }
}

namespace {

// FindKeyedElem returns the first element named key across all bindings, in
// document order.
template <class Elem, class Binding>
std::optional<Elem> FindKeyedElem(const std::vector<Binding>& bs, std::string_view key) {
  for (const auto& b : bs) {
    std::optional<Elem> found;
    b.Each([&](const Elem& e) {
      if (e.key == key) {
        found = e;
        return false;
      }
      return true;
    });
    if (found) return found;
  }
  return std::nullopt;
}

// ContainerFor reads the scope of an entry a path descended through: a Block,
// or an Assignment or MapEntry holding a BlockVal. Those are the only entries
// ResolveIn descends through.
template <class C>
C ContainerFor(const EntryPtr& e) {
  if (const auto* b = As<Block>(e)) return {b->pos.offset, b->end.offset - 1};
  return {EntryPos(e).offset, As<BlockVal>(*EntryValue(e))->end.offset - 1};
}

}  // namespace

Rewriter::~Rewriter() = default;

StatusOr<std::unique_ptr<Rewriter>> Rewriter::New(std::string src) {
  StatusOr<pxf::Document> doc = Parse(src);
  if (!doc.ok()) return doc.status();
  return std::unique_ptr<Rewriter>(new Rewriter(std::move(src), std::move(doc).consume()));
}

std::string_view Rewriter::Slice(int start, int end) const {
  return std::string_view(src_).substr(start, end - start);
}

Status Rewriter::Set(std::string_view path, const ValuePtr& v) {
  const std::string p(path);
  if (IsNil(v)) return Status::Error("pxf: Set " + p + ": nil value");
  if (ContainsBadVal(v)) {
    return Status::Error("pxf: Set " + p + ": cannot write a BadVal placeholder");
  }
  auto t = Resolve(path);
  if (!t.ok()) return t.status();
  return SetResolved("Set " + p, p, *t, v);
}

// SetResolved stages the value edit for a resolved target: replacing an
// existing entry's value span, or inserting the missing chain. op is the
// display prefix for error messages ("Set <path>"); path_key is the staging
// identity (same-key edits replace each other).
Status Rewriter::SetResolved(const std::string& op,
                             const std::string& path_key,
                             const Target& t,
                             const ValuePtr& v) {
  if (t.entry != nullptr) {
    const ValuePtr* old = EntryValue(*t.entry);
    if (old == nullptr) {
      return Status::Error("pxf: " + op +
                           ": path addresses a block; set its fields individually, or Remove it "
                           "and Set a BlockVal");
    }
    auto [old_start, old_end] = ValueSpan(*old);
    std::string indent = LineIndentAt(EntryPos(*t.entry).offset);
    Stage({path_key,
           old_start.offset,
           old_end.offset,
           RenderValue(v, indent, StepFor(*t.container_entries))});
    return Status::OK();
  }
  return StageInsert(op, path_key, t, v);
}

Status Rewriter::Remove(std::string_view path) {
  auto t = Resolve(path);
  if (!t.ok()) return t.status();
  const std::string p(path);
  if (t->entry == nullptr) return Status::Error("pxf: Remove " + p + ": no such entry");
  RemovalSpan s = EntryRemovalSpan(*t->entry);
  Stage({p, s.start, s.end, ""});
  return Status::OK();
}

// EntryRemovalSpan returns the source span removing e should delete: when the
// entry sits alone on its line(s), the whole lines including a trailing comment
// on the last one (whole_lines true); otherwise the exact entry span plus the
// spaces separating it from what follows.
Rewriter::RemovalSpan Rewriter::EntryRemovalSpan(const EntryPtr& e) const {
  auto [pos, end_pos] = EntrySpan(e);
  const int start = pos.offset;
  int end = end_pos.offset;
  const int n = Len();

  const int ls = LineStartOffset(src_, start);
  if (IsBlank(Slice(ls, start))) {
    // The entry starts its line; try to consume through end of line.
    int le = end;
    while (le < n && (src_[le] == ' ' || src_[le] == '\t')) ++le;
    if (le < n && (src_[le] == '#' || (src_[le] == '/' && le + 1 < n && src_[le + 1] == '/'))) {
      while (le < n && src_[le] != '\n') ++le;
    } else if (le + 1 < n && src_[le] == '/' && src_[le + 1] == '*') {
      // A trailing block comment is consumed only when it closes on the same
      // line; a multi-line /* ... */ is not the entry's own trailing comment,
      // so fall through to span-only removal.
      size_t close = src_.find("*/", le);
      if (close != std::string::npos) {
        int through = static_cast<int>(close) + 2;
        if (!HasNewline(Slice(le, through))) {
          le = through;
          while (le < n && (src_[le] == ' ' || src_[le] == '\t')) ++le;
        }
      }
    }
    if (le >= n || src_[le] == '\n') {
      if (le < n) ++le;  // include the newline
      return {ls, le, true};
    }
  }
  // The entry shares its line(s) with other content: remove its exact span plus
  // the spaces that separated it from what follows.
  while (end < n && (src_[end] == ' ' || src_[end] == '\t')) ++end;
  return {start, end, false};
}

Status Rewriter::ReplaceValue(const ValuePtr& old, const ValuePtr& with) {
  if (IsNil(old)) return Status::Error("pxf: ReplaceValue: nil old value");
  if (IsNil(with)) return Status::Error("pxf: ReplaceValue: nil replacement value");
  if (ContainsBadVal(with)) {
    return Status::Error("pxf: ReplaceValue: cannot write a BadVal placeholder");
  }
  auto [start_pos, end_pos] = ValueSpan(old);
  const int start = start_pos.offset;
  const int end = end_pos.offset;
  if (start >= end) {
    return Status::Error(
        "pxf: ReplaceValue: old value has no source span (not a parsed value, or a BadVal)");
  }
  return SetSpan(start, end, RenderValue(with, LineLeadingIndent(start), IndentStep()));
}

Status Rewriter::SetSpan(int start, int end, std::string_view text) {
  if (start < 0 || end < start || end > Len()) {
    return Status::Error("pxf: SetSpan: span [" + std::to_string(start) + ":" +
                         std::to_string(end) + ") out of range for source of length " +
                         std::to_string(Len()));
  }
  Stage({SpanPath(start, end), start, end, std::string(text)});
  return Status::OK();
}

Status Rewriter::AppendEntry(const Block* block, const EntryPtr& e) {
  if (block == nullptr) return Status::Error("pxf: AppendEntry: nil block");
  if (IsNil(e)) return Status::Error("pxf: AppendEntry: nil entry");
  if (EntryContainsBadVal(e)) {
    return Status::Error("pxf: AppendEntry: cannot write a BadVal placeholder");
  }
  const int close_off = block->end.offset - 1;
  const int scope_start = block->pos.offset;
  if (close_off < scope_start || close_off >= Len() || src_[close_off] != '}') {
    return Status::Error("pxf: AppendEntry: block " + GoQuote(block->name) +
                         " has no source span (not a parsed block)");
  }
  Insertion ins = RenderAppendEntry(scope_start, close_off, block->entries, e);
  StageInsertEdit(ins.at, std::move(ins.text));
  return Status::OK();
}

// RenderAppendEntry computes the insertion that appends e as the last entry of
// the scope whose opening entry starts at scope_start and whose closing '}' sits
// at close_off, formatted to match the scope's existing siblings. Shared by
// AppendEntry and the keyed-collection editors, which stage the result under
// their own edit identities.
Rewriter::Insertion Rewriter::RenderAppendEntry(int scope_start,
                                                int close_off,
                                                const Entries& siblings,
                                                const EntryPtr& e) const {
  if (!HasNewline(Slice(scope_start, close_off))) {
    // Inline block `{ ... }` (or `{}`): splice ` <entry> ` before '}'.
    std::string sb;
    if (close_off > 0 && src_[close_off - 1] != ' ' && src_[close_off - 1] != '\t') {
      sb.push_back(' ');
    }
    WriteEntryInline(&sb, e);
    sb.push_back(' ');
    return {close_off, std::move(sb)};
  }

  // Multi-line block: insert a full line just above the closing brace, indented
  // like the existing siblings. A nested body (a block entry's children) steps
  // in by the document's own indent width, not a fixed two spaces.
  std::string step = StepFor(siblings);
  std::string indent = ChildIndent(siblings, scope_start);
  if (siblings.empty()) {
    // No sibling reveals the base indent; derive it from the block's own line
    // plus one step, rather than ChildIndent's 2-space guess.
    indent = LineLeadingIndent(scope_start) + step;
  }
  int insert_at = close_off;
  std::string sb;
  const int ls = LineStartOffset(src_, close_off);
  if (IsBlank(Slice(ls, close_off))) {
    insert_at = ls;
  } else {
    // The '}' shares a line with the last entry; break the line.
    sb.push_back('\n');
  }
  sb += RenderEntryLines(e, indent, step);
  sb.push_back('\n');
  return {insert_at, std::move(sb)};
}

// StageInsertEdit records a zero-width insertion of text at off under a unique
// edit identity, so multiple insertions at the same offset are all kept, in
// call order, rather than deduplicated.
void Rewriter::StageInsertEdit(int off, std::string text) {
  ++insert_seq_;
  std::string path(1, '\0');
  path += "insert#" + std::to_string(insert_seq_);
  Stage({std::move(path), off, off, std::move(text)});
}

StatusOr<std::string> Rewriter::Bytes() const {
  if (edits_.empty()) return src_;
  std::vector<SpanEdit> edits = edits_;
  std::stable_sort(edits.begin(), edits.end(), [](const SpanEdit& a, const SpanEdit& b) {
    return a.start < b.start;
  });
  for (size_t i = 1; i < edits.size(); ++i) {
    if (edits[i - 1].end > edits[i].start) {
      return Status::Error("pxf: conflicting edits: " + edits[i - 1].path + " overlaps " +
                           edits[i].path);
    }
  }
  std::string out;
  out.reserve(src_.size() + 64);
  int last = 0;
  for (const auto& e : edits) {
    out.append(src_, last, e.start - last);
    out += e.text;
    last = e.end;
  }
  out.append(src_, last, std::string::npos);
  auto reparsed = Parse(out);
  if (!reparsed.ok()) {
    return Status::Error("pxf: rewrite produced an invalid document: " +
                         reparsed.status().ToString());
  }
  return out;
}

// Stage records an edit, replacing any previously staged edit for the same path
// (last call wins).
void Rewriter::Stage(SpanEdit e) {
  for (auto& existing : edits_) {
    if (existing.path == e.path) {
      existing = std::move(e);
      return;
    }
  }
  edits_.push_back(std::move(e));
}

StatusOr<Rewriter::Target> Rewriter::Resolve(std::string_view path) const {
  return ResolveIn(doc_.entries, std::nullopt, path);
}

// ResolveIn resolves a dotted path against an arbitrary entry scope: the
// document top level for Resolve, or one keyed element's body for the
// keyed-collection editors. container is the scope's owner (unset at the top
// level).
StatusOr<Rewriter::Target> Rewriter::ResolveIn(const Entries& entries_in,
                                               std::optional<Container> container,
                                               std::string_view path) const {
  const std::vector<std::string> segs = SplitDots(path);
  for (const auto& seg : segs) {
    if (seg.empty()) {
      return Status::Error("pxf: invalid path " + GoQuote(path) + ": empty segment");
    }
  }
  const Entries* entries = &entries_in;
  for (size_t i = 0; i < segs.size(); ++i) {
    const EntryPtr* e = FindEntry(*entries, segs[i]);
    Target t;
    t.container = container;
    t.container_entries = entries;
    if (e == nullptr) {
      t.missing.assign(segs.begin() + static_cast<std::ptrdiff_t>(i), segs.end());
      return t;
    }
    if (i == segs.size() - 1) {
      t.entry = e;
      return t;
    }
    if (const auto* b = As<Block>(*e)) {
      entries = &b->entries;
    } else {
      const BlockVal* bv = As<BlockVal>(*EntryValue(*e));
      if (bv == nullptr) {
        return Status::Error("pxf: path " + GoQuote(path) + ": segment " + GoQuote(segs[i]) +
                             " is a scalar field, not a block");
      }
      entries = &bv->entries;
    }
    container = ContainerFor<Container>(*e);
  }
  return Status::Error("pxf: invalid path " + GoQuote(path));  // unreachable: segs is never empty
}

// StageInsert builds the text for the missing tail of a path and stages its
// insertion at the end of the deepest existing scope. op is the display prefix
// for error messages; path_key the staging identity.
Status Rewriter::StageInsert(const std::string& op,
                             const std::string& path_key,
                             const Target& t,
                             const ValuePtr& v) {
  // The intermediate (block-creating) segments must be bare identifiers; the
  // leaf may need quoting only in map (':') form.
  for (size_t i = 0; i + 1 < t.missing.size(); ++i) {
    if (NeedsQuoting(t.missing[i])) {
      return Status::Error("pxf: " + op + ": segment " + GoQuote(t.missing[i]) +
                           " is not a valid block name");
    }
  }
  const std::string& leaf = t.missing.back();
  const bool map_form = HasMapEntry(*t.container_entries);
  if (NeedsQuoting(leaf) && !map_form) {
    return Status::Error("pxf: " + op + ": key " + GoQuote(leaf) + " needs a map (':') context");
  }
  std::string step = StepFor(*t.container_entries);

  // Locate the insertion scope: [open..close) of the container block, or the
  // end of the document for the top level.
  int close_off = Len();  // offset of the closing '}' (top level: len(src))
  int scope_start = -1;   // offset the block's entry starts at, for indent fallback
  if (t.container) {
    close_off = t.container->close_off;
    scope_start = t.container->pos;
  }
  if (scope_start >= 0 && (close_off < 0 || close_off >= Len() || src_[close_off] != '}')) {
    return Status::Error("pxf: " + op + ": internal error: container span does not end at '}'");
  }

  if (scope_start >= 0 && !HasNewline(Slice(scope_start, close_off))) {
    // Inline block `{ ... }` (or `{}`): splice ` key = value ` just before the
    // closing brace.
    std::string sb;
    if (close_off > 0 && src_[close_off - 1] != ' ' && src_[close_off - 1] != '\t') {
      sb.push_back(' ');
    }
    sb += RenderChain(t.missing, "", step, map_form, v, true);
    sb.push_back(' ');
    Stage({path_key, close_off, close_off, std::move(sb)});
    return Status::OK();
  }

  // Multi-line scope: insert full lines just above the closing brace (top
  // level: at end of input), indented like the existing siblings.
  std::string indent = SiblingIndent(t);
  int insert_at = close_off;
  std::string sb;
  if (scope_start >= 0) {
    const int ls = LineStartOffset(src_, close_off);
    if (IsBlank(Slice(ls, close_off))) {
      insert_at = ls;
    } else {
      // The '}' shares a line with the last entry; break the line.
      sb.push_back('\n');
    }
  } else if (!src_.empty() && src_.back() != '\n') {
    sb.push_back('\n');
  }
  sb += RenderChain(t.missing, indent, step, map_form, v, false);
  sb.push_back('\n');
  Stage({path_key, insert_at, insert_at, std::move(sb)});
  return Status::OK();
}

// RenderChain renders the missing path tail: zero or more nested block opens,
// then `leaf = value` (or `leaf: value` in map form). base_indent is the indent
// of the first rendered line; nested lines add one step per level. In inline
// form everything renders on one line and base_indent and step are ignored.
std::string Rewriter::RenderChain(const std::vector<std::string>& missing,
                                  const std::string& base_indent,
                                  const std::string& step,
                                  bool map_form,
                                  const ValuePtr& v,
                                  bool is_inline) const {
  std::string sb;
  // Only the leaf's immediate container decides '=' vs ':'; synthesized
  // intermediate blocks are messages, so their leaf uses '='.
  const bool leaf_map = map_form && missing.size() == 1;
  const int depth = static_cast<int>(missing.size()) - 1;
  for (int i = 0; i < depth; ++i) {
    if (!is_inline) sb += IndentAt(base_indent, step, i);
    sb += missing[i];
    sb += is_inline ? " { " : " {\n";
  }
  const std::string& leaf = missing.back();
  if (!is_inline) sb += IndentAt(base_indent, step, depth);
  sb += leaf_map && NeedsQuoting(leaf) ? GoQuote(leaf) : leaf;
  sb += leaf_map ? ": " : " = ";
  sb += RenderValue(v, IndentAt(base_indent, step, depth), step);
  for (int i = depth - 1; i >= 0; --i) {
    if (is_inline) {
      sb += " }";
    } else {
      sb.push_back('\n');
      sb += IndentAt(base_indent, step, i);
      sb.push_back('}');
    }
  }
  return sb;
}

// RenderValue formats v on its own and re-indents any continuation lines with
// line_prefix so multi-line values (lists, blocks) align under the entry they
// are spliced into. Nested bodies step in by step per level, matching the
// document's indent width rather than a fixed two spaces.
std::string Rewriter::RenderValue(const ValuePtr& v,
                                  const std::string& line_prefix,
                                  const std::string& step) const {
  std::string out;
  AppendValueIndented(&out, v, step);
  if (!line_prefix.empty()) ReplaceNewlines(&out, line_prefix);
  return out;
}

// SiblingIndent returns the indentation for a line inserted into the target's
// container: the indent of the container's first entry when there is one on its
// own line, otherwise the container's own indent plus one level (top level:
// none).
std::string Rewriter::SiblingIndent(const Target& t) const {
  return ChildIndent(*t.container_entries, t.container ? t.container->pos : -1);
}

// ChildIndent returns the indentation a new child of a block should use: the
// indent of the first existing child on its own line, else the block's own
// indent plus one level. container_pos < 0 means the document top level, which
// is not indented.
std::string Rewriter::ChildIndent(const Entries& entries, int container_pos) const {
  if (!entries.empty()) {
    const int first = EntryPos(entries[0]).offset;
    const int ls = LineStartOffset(src_, first);
    if (IsBlank(Slice(ls, first))) return std::string(Slice(ls, first));
  }
  if (container_pos < 0) return "";
  return LineIndentAt(container_pos) + "  ";
}

// StepFor infers the per-level indentation step to use for content inserted
// among entries: prefer a nested block among those entries (a sibling's body is
// the most relevant reference), then a document-wide scan, then a two-space
// default when nothing reveals it.
std::string Rewriter::StepFor(const Entries& entries) const {
  std::string s = ScanIndentStep(entries);
  return s.empty() ? IndentStep() : s;
}

// IndentStep infers the document's per-level indentation step from a
// document-wide scan, or two spaces when nothing reveals it.
std::string Rewriter::IndentStep() const {
  std::string s = ScanIndentStep(doc_.entries);
  return s.empty() ? "  " : s;
}

// ScanIndentStep returns the whitespace the first own-line child of the first
// nested block (or block value) in entries adds over that block's own line (the
// document's indentation step), or "" when no nested block on its own lines
// reveals one.
std::string Rewriter::ScanIndentStep(const Entries& entries) const {
  for (const auto& e : entries) {
    const Entries* kids = nullptr;
    if (const auto* n = As<Block>(e)) {
      kids = &n->entries;
    } else if (const ValuePtr* v = EntryValue(e)) {
      if (const auto* bv = As<BlockVal>(*v)) kids = &bv->entries;
    }
    if (kids == nullptr || kids->empty()) continue;
    std::string base = LineLeadingIndent(EntryPos(e).offset);
    const int first = EntryPos((*kids)[0]).offset;
    const int ls = LineStartOffset(src_, first);
    if (IsBlank(Slice(ls, first))) {
      std::string_view ci = Slice(ls, first);
      if (ci.size() > base.size() && ci.substr(0, base.size()) == base) {
        return std::string(ci.substr(base.size()));
      }
    }
    std::string s = ScanIndentStep(*kids);
    if (!s.empty()) return s;
  }
  return "";
}

// LineIndentAt returns the whitespace prefix of the line containing off, or ""
// when non-whitespace precedes off on its line.
std::string Rewriter::LineIndentAt(int off) const {
  const int ls = LineStartOffset(src_, off);
  if (IsBlank(Slice(ls, off))) return std::string(Slice(ls, off));
  return "";
}

// LineLeadingIndent returns the leading whitespace of the line containing off,
// regardless of what else precedes off on the line. Used to align continuation
// lines of a value spliced after its key.
std::string Rewriter::LineLeadingIndent(int off) const {
  const int ls = LineStartOffset(src_, off);
  int i = ls;
  while (i < off && (src_[i] == ' ' || src_[i] == '\t')) ++i;
  return std::string(Slice(ls, i));
}

// --- Keyed-collection editing (rewrite_keyed.go) ---------------------------

Rewriter& Rewriter::KeyedByField(std::string_view field_path, std::string_view key_field) {
  key_fields_.insert_or_assign(std::string(field_path), std::string(key_field));
  return *this;
}

// KeyedScopeEntries resolves the sibling scope that holds field_path's bindings
// and the collection's own key segment (the last path segment), so the caller
// can gather every binding of the field.
Status Rewriter::KeyedScopeEntries(std::string_view field_path,
                                   const Entries** scope,
                                   std::string* key) const {
  auto segs = SplitPathSegs(field_path);
  if (!segs) return Status::Error("invalid path " + GoQuote(field_path));
  *key = segs->back();
  if (segs->size() == 1) {
    *scope = &doc_.entries;
    return Status::OK();
  }
  const std::string parent = JoinPathSegs(*segs, segs->size() - 1);
  auto t = ResolveIn(doc_.entries, std::nullopt, parent);
  if (!t.ok()) return t.status();
  if (t->entry == nullptr) return Status::Error("no entry at " + GoQuote(parent));
  if (const auto* b = As<Block>(*t->entry)) {
    *scope = &b->entries;
    return Status::OK();
  }
  if (const ValuePtr* v = EntryValue(*t->entry)) {
    if (const auto* bv = As<BlockVal>(*v)) {
      *scope = &bv->entries;
      return Status::OK();
    }
  }
  return Status::Error(GoQuote(parent) + " is not a block");
}

// KeyedBindings gathers every binding of the keyed field at field_path, in
// document order. Anonymous-list bindings are included only when the field's key
// field has been declared (KeyedByField).
StatusOr<std::vector<Rewriter::KeyedBinding>> Rewriter::KeyedBindings(
    const std::string& op, std::string_view field_path) const {
  const Entries* scope = nullptr;
  std::string key;
  Status st = KeyedScopeEntries(field_path, &scope, &key);
  if (!st.ok()) return Status::Error("pxf: " + op + ": " + st.ToString());
  std::string key_field;
  if (auto it = key_fields_.find(field_path); it != key_fields_.end()) key_field = it->second;
  std::vector<KeyedBinding> bs;
  bool saw_undeclared_list = false;
  for (const auto& e : *scope) {
    if (EntryKey(e) != key) continue;
    if (const auto* n = As<Block>(e)) {
      KeyedBinding b;
      b.scope_start = n->pos.offset;
      b.close_off = n->end.offset - 1;
      b.entries = &n->entries;
      bs.push_back(std::move(b));
    } else if (const auto* n = As<Assignment>(e)) {
      if (const auto* v = As<BlockVal>(n->value)) {
        KeyedBinding b;
        b.scope_start = n->pos.offset;
        b.close_off = v->end.offset - 1;
        b.entries = &v->entries;
        bs.push_back(std::move(b));
      } else if (const auto* v = As<ListVal>(n->value)) {
        if (key_field.empty()) {
          saw_undeclared_list = true;
          continue;
        }
        KeyedBinding b;
        b.anon = true;
        b.scope_start = n->pos.offset;
        b.close_off = v->end.offset - 1;
        b.list = v;
        b.key_field = key_field;
        bs.push_back(std::move(b));
      }
    }
  }
  if (bs.empty()) {
    if (saw_undeclared_list) {
      return Status::Error("pxf: " + op + ": " + GoQuote(field_path) +
                           " is written in the anonymous list form; declare its key field with "
                           "KeyedByField to edit it");
    }
    return Status::Error("pxf: " + op + ": no keyed collection at " + GoQuote(field_path));
  }
  for (const auto& b : bs) {
    const char want = b.anon ? ']' : '}';
    if (b.close_off < b.scope_start || b.close_off >= Len() || src_[b.close_off] != want) {
      return Status::Error("pxf: " + op + ": binding at " + GoQuote(field_path) +
                           " has no source span");
    }
  }
  return bs;
}

Status Rewriter::SetKeyed(std::string_view field_path,
                          std::string_view key,
                          std::string_view subpath,
                          const ValuePtr& v) {
  const std::string fp(field_path);
  const std::string sub(subpath);
  const std::string op = "SetKeyed " + fp + "[" + GoQuote(key) + "]." + sub;
  if (IsNil(v)) return Status::Error("pxf: " + op + ": nil value");
  if (ContainsBadVal(v)) {
    return Status::Error("pxf: " + op + ": cannot write a BadVal placeholder");
  }
  if (key.empty()) return Status::Error("pxf: " + op + ": empty key");
  auto bs = KeyedBindings(op, field_path);
  if (!bs.ok()) return bs.status();
  std::string path_key(1, '\0');
  path_key += "keyed:set:" + fp;
  path_key.push_back('\0');
  path_key += key;
  path_key.push_back('\0');
  path_key += sub;
  auto elem = FindKeyedElem<KeyedElem>(*bs, key);
  if (!elem) {
    auto chain = BuildEntryChain(op, subpath, v);
    if (!chain.ok()) return chain.status();
    StageSplice(path_key, RenderAppendKeyedElement(bs->back(), key, std::move(chain).consume()));
    return Status::OK();
  }
  auto t = ResolveIn(*elem->body, elem->ContainerOf(), subpath);
  if (!t.ok()) return t.status();
  return SetResolved(op, path_key, *t, v);
}

// RenderAppendKeyedElement builds the insertion that appends a new element with
// the given key and body to a binding, in the binding's form.
Rewriter::Insertion Rewriter::RenderAppendKeyedElement(const KeyedBinding& b,
                                                       std::string_view key,
                                                       Entries body) const {
  if (b.anon) {
    return SpliceListElementAppend(
        b, RenderListElement(b, NewAnonElement(b.key_field, key, std::move(body))), "");
  }
  return RenderAppendEntry(
      b.scope_start, b.close_off, *b.entries, NewKeyedElementBlock(key, std::move(body)));
}

Status Rewriter::RemoveKeyed(std::string_view field_path,
                             std::string_view key,
                             std::string_view subpath) {
  const std::string fp(field_path);
  const std::string sub(subpath);
  const std::string op = "RemoveKeyed " + fp + "[" + GoQuote(key) + "]." + sub;
  auto bs = KeyedBindings(op, field_path);
  if (!bs.ok()) return bs.status();
  auto elem = FindKeyedElem<KeyedElem>(*bs, key);
  if (!elem) return Status::Error("pxf: " + op + ": no element " + GoQuote(key));
  auto t = ResolveIn(*elem->body, elem->ContainerOf(), subpath);
  if (!t.ok()) return t.status();
  if (t->entry == nullptr) return Status::Error("pxf: " + op + ": no such entry");
  RemovalSpan s = EntryRemovalSpan(*t->entry);
  std::string path_key(1, '\0');
  path_key += "keyed:rm:" + fp;
  path_key.push_back('\0');
  path_key += key;
  path_key.push_back('\0');
  path_key += sub;
  Stage({std::move(path_key), s.start, s.end, ""});
  return Status::OK();
}

Status Rewriter::RemoveKeyedElement(std::string_view field_path, std::string_view key) {
  const std::string fp(field_path);
  const std::string op = "RemoveKeyedElement " + fp + "[" + GoQuote(key) + "]";
  auto bs = KeyedBindings(op, field_path);
  if (!bs.ok()) return bs.status();
  auto elem = FindKeyedElem<KeyedElem>(*bs, key);
  if (!elem) return Status::Error("pxf: " + op + ": no element " + GoQuote(key));
  std::string path_key(1, '\0');
  path_key += "keyed:rmel:" + fp;
  path_key.push_back('\0');
  path_key += key;
  if (elem->b->anon) {
    StageListRemoval(path_key, ListElementRemovalSpans(*elem->b->list, elem->idx, false));
    return Status::OK();
  }
  RemovalSpan s = EntryRemovalSpan(*elem->node);
  Stage({std::move(path_key), s.start, s.end, ""});
  return Status::OK();
}

Status Rewriter::InsertKeyedElement(std::string_view field_path,
                                    std::string_view key,
                                    std::string_view before,
                                    const std::vector<EntryPtr>& body) {
  const std::string fp(field_path);
  const std::string op = "InsertKeyedElement " + fp + "[" + GoQuote(key) + "]";
  if (key.empty()) return Status::Error("pxf: " + op + ": empty key");
  for (const auto& e : body) {
    if (IsNil(e)) return Status::Error("pxf: " + op + ": nil body entry");
    if (EntryContainsBadVal(e)) {
      return Status::Error("pxf: " + op + ": cannot write a BadVal placeholder");
    }
  }
  auto bs = KeyedBindings(op, field_path);
  if (!bs.ok()) return bs.status();
  if (FindKeyedElem<KeyedElem>(*bs, key)) {
    return Status::Error("pxf: " + op + ": element " + GoQuote(key) + " already exists");
  }
  std::string path_key(1, '\0');
  path_key += "keyed:ins:" + fp;
  path_key.push_back('\0');
  path_key += key;

  if (before.empty()) {
    StageSplice(path_key, RenderAppendKeyedElement(bs->back(), key, CloneEntries(body)));
    return Status::OK();
  }
  auto anchor = FindKeyedElem<KeyedElem>(*bs, before);
  if (!anchor) {
    return Status::Error("pxf: " + op + ": no element " + GoQuote(before) + " to insert before");
  }
  if (anchor->b->anon) {
    std::string text = RenderListElement(
        *anchor->b, NewAnonElement(anchor->b->key_field, key, CloneEntries(body)));
    StageSplice(path_key, SpliceListElementBefore(*anchor->b, anchor->idx, text, ""));
    return Status::OK();
  }
  Insertion ins = RenderInsertBlockElementBefore(
      *anchor->b, *anchor->node, NewKeyedElementBlock(key, CloneEntries(body)));
  Stage({std::move(path_key), ins.at, ins.at, std::move(ins.text)});
  return Status::OK();
}

// RenderInsertBlockElementBefore builds the insertion that places nb immediately
// before anchor in a keyed-block binding: above the anchor's glued leading
// comments when the anchor starts its own line, or inline before it otherwise.
Rewriter::Insertion Rewriter::RenderInsertBlockElementBefore(const KeyedBinding& b,
                                                             const EntryPtr& anchor,
                                                             const EntryPtr& nb) const {
  const int a_start = EntryPos(anchor).offset;
  const int ls = LineStartOffset(src_, a_start);
  if (IsBlank(Slice(ls, a_start))) {
    std::string indent(Slice(ls, a_start));
    int at = ExtendOverLeadingComments(ls);
    std::string text = RenderEntryLines(nb, indent, StepFor(*b.entries));
    text.push_back('\n');
    return {at, std::move(text)};
  }
  std::string sb;
  WriteEntryInline(&sb, nb);
  sb.push_back(' ');
  return {a_start, std::move(sb)};
}

Status Rewriter::RenameKeyedElement(std::string_view field_path,
                                    std::string_view old_key,
                                    std::string_view new_key) {
  const std::string fp(field_path);
  const std::string op = "RenameKeyedElement " + fp + "[" + GoQuote(old_key) + "]";
  if (new_key.empty()) return Status::Error("pxf: " + op + ": empty new key");
  if (new_key == old_key) return Status::OK();
  auto bs = KeyedBindings(op, field_path);
  if (!bs.ok()) return bs.status();
  auto elem = FindKeyedElem<KeyedElem>(*bs, old_key);
  if (!elem) return Status::Error("pxf: " + op + ": no element " + GoQuote(old_key));
  if (FindKeyedElem<KeyedElem>(*bs, new_key)) {
    return Status::Error("pxf: " + op + ": element " + GoQuote(new_key) + " already exists");
  }
  std::string path_key(1, '\0');
  path_key += "keyed:ren:" + fp;
  path_key.push_back('\0');
  path_key += old_key;

  if (elem->b->anon) {
    // The key is the value of the key-field assignment; replace that value
    // literal (always a quoted string in the anonymous form).
    const Assignment* ka = AnonKeyAssignment(elem->bv->entries, elem->b->key_field);
    if (ka == nullptr) {
      return Status::Error("pxf: " + op + ": element has no " + GoQuote(elem->b->key_field) +
                           " assignment");
    }
    auto [start, end] = ValueSpan(ka->value);
    Stage({std::move(path_key), start.offset, end.offset, GoQuote(new_key)});
    return Status::OK();
  }

  const int start = EntryPos(*elem->node).offset;
  const int end = NameTokenEnd(src_, start);
  if (end < 0) return Status::Error("pxf: " + op + ": element has no source span");
  std::string text = IsIdentifierSafeEntryName(new_key) ? std::string(new_key) : GoQuote(new_key);
  Stage({std::move(path_key), start, end, std::move(text)});
  return Status::OK();
}

Status Rewriter::MoveKeyedElement(std::string_view field_path,
                                  std::string_view key,
                                  std::string_view before) {
  const std::string fp(field_path);
  const std::string op = "MoveKeyedElement " + fp + "[" + GoQuote(key) + "]";
  if (before == key) {
    return Status::Error("pxf: " + op + ": cannot move an element before itself");
  }
  auto bs = KeyedBindings(op, field_path);
  if (!bs.ok()) return bs.status();
  auto elem = FindKeyedElem<KeyedElem>(*bs, key);
  if (!elem) return Status::Error("pxf: " + op + ": no element " + GoQuote(key));
  std::optional<KeyedElem> anchor;
  if (!before.empty()) {
    anchor = FindKeyedElem<KeyedElem>(*bs, before);
    if (!anchor) {
      return Status::Error("pxf: " + op + ": no element " + GoQuote(before) + " to move before");
    }
    if (anchor->b->anon != elem->b->anon) {
      return Status::Error("pxf: " + op + ": element " + GoQuote(key) + " and anchor " +
                           GoQuote(before) + " are in different surface forms");
    }
  }
  const KeyedElem* a = anchor ? &*anchor : nullptr;
  if (elem->b->anon) return MoveListElement(field_path, *elem, a);
  return MoveBlockElement(op, field_path, *elem, a);
}

// MoveBlockElement reorders a keyed-block-form element by capturing its source
// (plus glued comments) and reinserting it at a line boundary.
Status Rewriter::MoveBlockElement(const std::string& op,
                                  std::string_view field_path,
                                  const KeyedElem& elem,
                                  const KeyedElem* anchor) {
  RemovalSpan span = EntryRemovalSpan(*elem.node);
  int start = span.start;
  const int end = span.end;
  if (span.whole_lines) start = ExtendOverLeadingComments(start);
  std::string text(Slice(start, end));
  std::string del_key(1, '\0');
  del_key += "keyed:mv:";
  del_key += field_path;
  del_key.push_back('\0');
  del_key += elem.key;
  std::string ins_key = del_key;
  ins_key.push_back('\0');
  ins_key += "ins";

  if (!span.whole_lines) {
    if (text.empty() || text.back() != ' ') text.push_back(' ');
    int at = elem.b->close_off;
    if (anchor != nullptr) {
      at = EntryPos(*anchor->node).offset;
    } else if (at > 0 && src_[at - 1] != ' ' && src_[at - 1] != '\t') {
      text.insert(0, " ");
    }
    Stage({std::move(del_key), start, end, ""});
    Stage({std::move(ins_key), at, at, std::move(text)});
    return Status::OK();
  }

  if (text.empty() || text.back() != '\n') text.push_back('\n');
  int at = 0;
  if (anchor != nullptr) {
    const int a_start = EntryPos(*anchor->node).offset;
    const int ls = LineStartOffset(src_, a_start);
    if (!IsBlank(Slice(ls, a_start))) {
      return Status::Error("pxf: " + op + ": anchor element does not start its own line");
    }
    at = ExtendOverLeadingComments(ls);
  } else {
    at = elem.b->close_off;
    const int ls = LineStartOffset(src_, elem.b->close_off);
    if (IsBlank(Slice(ls, elem.b->close_off))) {
      at = ls;
    } else {
      text.insert(0, "\n");
    }
  }
  Stage({std::move(del_key), start, end, ""});
  Stage({std::move(ins_key), at, at, std::move(text)});
  return Status::OK();
}

// MoveListElement reorders an anonymous-form element by capturing its value span
// and reinserting it before the anchor (or at the list end). The element's
// trailing comment goes along only to a line of the element's own: on an inline
// target a line comment would swallow whatever followed it.
Status Rewriter::MoveListElement(std::string_view field_path,
                                 const KeyedElem& elem,
                                 const KeyedElem* anchor) {
  const ListVal& lv = *elem.b->list;
  // Already where it was asked to go: reproduce the input.
  if (anchor == nullptr && elem.idx == static_cast<int>(lv.elements.size()) - 1) {
    return Status::OK();
  }
  if (anchor != nullptr && anchor->b->list == &lv && anchor->idx == elem.idx + 1) {
    return Status::OK();
  }
  const std::string text(Slice(elem.bv->pos.offset, elem.bv->end.offset));
  std::string del_key(1, '\0');
  del_key += "keyed:mv:";
  del_key += field_path;
  del_key.push_back('\0');
  del_key += elem.key;

  const KeyedBinding* to = anchor != nullptr ? anchor->b : elem.b;
  bool on_own_line = ListMultiline(*to->list);
  if (anchor != nullptr) {
    const int p = ValuePos(to->list->elements[anchor->idx]).offset;
    on_own_line = on_own_line && IsBlank(Slice(LineStartOffset(src_, p), p));
  }
  std::string comment;
  if (on_own_line) {
    const TrailingCommentSpan c = TrailingComment(ListElemTail(lv, elem.idx));
    if (c.start >= 0) comment = std::string(Slice(c.start, c.end));
  }

  Insertion ins = anchor != nullptr
                      ? SpliceListElementBefore(*anchor->b, anchor->idx, text, comment)
                      : SpliceListElementAppend(*elem.b, text, comment);
  StageListRemoval(del_key, ListElementRemovalSpans(lv, elem.idx, !on_own_line));
  del_key.push_back('\0');
  del_key += "ins";
  StageSplice(del_key, std::move(ins));
  return Status::OK();
}

// --- Anonymous-list element surgery ----------------------------------------

// ListMultiline reports whether the list spans more than one source line (the
// canonical formatter's shape), as opposed to an inline `[ a, b ]`.
bool Rewriter::ListMultiline(const ListVal& lv) const {
  return HasNewline(Slice(lv.pos.offset, lv.end.offset));
}

// ListElementIndent returns the indentation of the list's elements: the first
// element's own-line indent, else the list's line plus one step.
std::string Rewriter::ListElementIndent(const ListVal& lv, int scope_start) const {
  if (!lv.elements.empty()) {
    const int p = ValuePos(lv.elements[0]).offset;
    const int ls = LineStartOffset(src_, p);
    if (IsBlank(Slice(ls, p))) return std::string(Slice(ls, p));
  }
  return LineLeadingIndent(scope_start) + IndentStep();
}

// ListComma returns the offset of the first comma in [from, to), which must be
// the gap between two list tokens, or -1. Comments in the gap are stepped over,
// since one may hold a comma of its own.
int Rewriter::ListComma(int from, int to) const {
  const auto starts = [&](int i, std::string_view pre) {
    return Slice(i, to).substr(0, pre.size()) == pre;
  };
  for (int i = from; i < to;) {
    if (src_[i] == ',') return i;
    if (src_[i] == '#' || starts(i, "//")) {
      while (i < to && src_[i] != '\n') ++i;
    } else if (starts(i, "/*")) {
      const size_t n = Slice(i, to).find("*/");
      if (n == std::string_view::npos) return -1;
      i += static_cast<int>(n) + 2;
    } else {
      ++i;
    }
  }
  return -1;
}

// ListElemComma returns the offset of the comma following element idx of lv, or
// -1 for a last element with no trailing comma.
int Rewriter::ListElemComma(const ListVal& lv, int idx) const {
  int limit = lv.end.offset - 1;
  if (idx < static_cast<int>(lv.elements.size()) - 1) limit = ValuePos(lv.elements[idx + 1]).offset;
  return ListComma(ValueSpan(lv.elements[idx]).second.offset, limit);
}

// ListElemTail returns the offset where a trailing comment of element idx of lv
// would start to look: past its comma when that sits on the element's last
// line, else just past the element.
int Rewriter::ListElemTail(const ListVal& lv, int idx) const {
  const int end = ValueSpan(lv.elements[idx]).second.offset;
  const int c = ListElemComma(lv, idx);
  if (c >= 0 && !HasNewline(Slice(end, c))) return c + 1;
  return end;
}

// TrailingComment finds the comment that ends the line at off. It returns the
// comment's span, or -1s when there is none, and the offset of the line's end,
// or -1 when something other than a comment follows off on the line. A block
// comment counts only when it closes on the line, as in EntryRemovalSpan.
Rewriter::TrailingCommentSpan Rewriter::TrailingComment(int off) const {
  std::string_view src(src_);
  int i = off + TrailingListPad(off);
  int start = -1;
  int end = -1;
  if (i < Len()) {
    std::string_view rest = src.substr(i);
    if (rest[0] == '#' || rest.substr(0, 2) == "//") {
      start = i;
      while (i < Len() && src_[i] != '\n') ++i;
      end = i;
    } else if (rest.substr(0, 2) == "/*") {
      const size_t n = rest.find("*/");
      if (n != std::string_view::npos && !HasNewline(rest.substr(0, n))) {
        start = i;
        end = i + static_cast<int>(n) + 2;
        i = end + TrailingListPad(end);
      }
    }
  }
  if (i >= Len() || src_[i] == '\n') return {start, end, i};
  return {-1, -1, -1};
}

// ListElementRemovalSpans returns the spans to delete for element idx of lv: the
// element, the comma that separates it from a neighbour so the list stays
// well-formed, and a trailing comment on its last line unless keep_comment is
// set. Those can be apart, with a comment on the element that stays between
// them, which is why this is more than one span.
//
// An element on lines of its own goes with those lines, so the lines around it
// survive. A last element with no trailing comma gives up the comma before it,
// which is then not its own: a list with a trailing comma keeps that shape.
std::vector<Rewriter::ByteSpan> Rewriter::ListElementRemovalSpans(const ListVal& lv,
                                                                  int idx,
                                                                  bool keep_comment) const {
  const auto& elems = lv.elements;
  auto [pos, end_pos] = ValueSpan(elems[idx]);
  int start = pos.offset;
  const int end = end_pos.offset;
  const int comma = ListElemComma(lv, idx);
  const int tail = ListElemTail(lv, idx);
  const TrailingCommentSpan tc = TrailingComment(tail);
  int line_end = tc.line_end;
  if (keep_comment && tc.start >= 0) line_end = -1;

  std::vector<ByteSpan> out;
  if (comma >= tail) {  // on a later line than the element
    out.push_back({comma, comma + 1 + TrailingListPad(comma + 1)});
  } else if (comma < 0 && idx > 0) {
    const int prev_end = ValueSpan(elems[idx - 1]).second.offset;
    const int prev = ListComma(prev_end, start);
    if (tc.start < 0 && prev >= 0 && !GapHasComment(prev_end, start)) {
      // Nothing to keep between the two elements: they close up.
      return {{prev_end, end + TrailingListPad(end)}};
    }
    if (prev >= 0) out.push_back({prev, prev + 1});
  }

  const int ls = LineStartOffset(src_, start);
  if (line_end < 0) {  // shares its last line with what follows
    out.push_back({start, tail + TrailingListPad(tail)});
  } else if (!IsBlank(Slice(ls, start))) {
    while (start > ls && (src_[start - 1] == ' ' || src_[start - 1] == '\t')) --start;
    out.push_back({start, line_end});
  } else if (line_end < Len()) {
    out.push_back({ls, line_end + 1});
  } else {
    out.push_back({ls, line_end});
  }
  return out;
}

// GapHasComment reports whether [from, to), the gap between two list tokens,
// holds a comment.
bool Rewriter::GapHasComment(int from, int to) const {
  for (char c : Slice(from, to)) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',') return true;
  }
  return false;
}

// StageListRemoval stages the deletion of spans under one identity.
void Rewriter::StageListRemoval(const std::string& path_key, const std::vector<ByteSpan>& spans) {
  for (size_t i = 0; i < spans.size(); ++i) {
    std::string path = path_key;
    path.push_back('\0');
    path += std::to_string(i);
    Stage({std::move(path), spans[i].start, spans[i].end, ""});
  }
}

// StageSplice stages an insertion under one identity.
void Rewriter::StageSplice(const std::string& path_key, Insertion s) {
  Stage({path_key, s.at, s.at, std::move(s.text)});
  if (s.comma_at >= 0) {
    std::string path = path_key;
    path.push_back('\0');
    path += "comma";
    Stage({std::move(path), s.comma_at, s.comma_at, ","});
  }
}

// TrailingListPad returns the count of spaces and tabs immediately after off
// (used to tidy an inline last-element removal). Newlines are left for the
// bracket line.
int Rewriter::TrailingListPad(int off) const {
  int n = 0;
  while (off + n < Len() && (src_[off + n] == ' ' || src_[off + n] == '\t')) ++n;
  return n;
}

// RenderListElement formats bv as a list element (no surrounding comma or
// newline), indented for b's list.
std::string Rewriter::RenderListElement(const KeyedBinding& b, const ValuePtr& bv) const {
  return RenderValue(bv, ListElementIndent(*b.list, b.scope_start), IndentStep());
}

// SpliceListElementAppend returns the insertion that adds elem_text as the last
// element of b's list, with comment after it on its line. It goes after the last
// element's line, so a comment there stays on that element, and after a trailing
// comma, which the new element then ends with too: the list keeps its shape.
Rewriter::Insertion Rewriter::SpliceListElementAppend(const KeyedBinding& b,
                                                      const std::string& elem_text,
                                                      const std::string& comment) const {
  const ListVal& lv = *b.list;
  std::string indent = ListElementIndent(lv, b.scope_start);
  if (lv.elements.empty()) {
    // Empty list: place the element on its own line before ']'.
    std::string sb = "\n" + indent + elem_text + "\n" + LineLeadingIndent(b.scope_start);
    return {b.close_off, std::move(sb)};
  }
  const int last = static_cast<int>(lv.elements.size()) - 1;
  const int last_end = ValueSpan(lv.elements[last]).second.offset;
  const int comma = ListElemComma(lv, last);

  if (!ListMultiline(lv)) {
    if (comma >= 0) return {comma + 1, " " + elem_text + ","};
    return {last_end, ", " + elem_text};
  }

  const int after = comma >= 0 ? comma + 1 : last_end;
  const int comment_end = TrailingComment(after).end;
  const int at = comment_end >= 0 ? comment_end : after;
  int comma_at = -1;
  std::string sb;
  if (comma < 0) {
    if (comment_end >= 0) {
      comma_at = last_end;
    } else {
      sb.push_back(',');
    }
  }
  sb += "\n" + indent + elem_text;
  if (comma >= 0) sb.push_back(',');
  if (!comment.empty()) sb += " " + comment;
  return {at, std::move(sb), comma_at};
}

// SpliceListElementBefore returns the insertion that places elem_text
// immediately before element idx of b's list, with comment after it on its line.
// On a list of lines it goes above the target's glued leading comments, which
// belong to the target.
Rewriter::Insertion Rewriter::SpliceListElementBefore(const KeyedBinding& b,
                                                      int idx,
                                                      const std::string& elem_text,
                                                      const std::string& comment) const {
  const ListVal& lv = *b.list;
  std::string indent = ListElementIndent(lv, b.scope_start);
  const int target = ValuePos(lv.elements[idx]).offset;
  if (ListMultiline(lv)) {
    const int ls = LineStartOffset(src_, target);
    if (IsBlank(Slice(ls, target))) {
      // Insert at a line start: the new element takes the target's indent, then
      // the comma and newline, leaving the lines after it intact.
      std::string sb = indent + elem_text + ",";
      if (!comment.empty()) sb += " " + comment;
      sb.push_back('\n');
      return {ExtendOverLeadingComments(ls), std::move(sb)};
    }
  }
  return {target, elem_text + ", "};
}

// --- shared helpers --------------------------------------------------------

// ExtendOverLeadingComments walks a line-start offset upward over contiguous
// whole-line `#` / `//` comment lines, returning the start of the topmost one.
// A blank line (or any non-comment line) stops the walk, so a comment block
// separated from the entry by a blank line, a group header, is not included.
int Rewriter::ExtendOverLeadingComments(int start) const {
  while (start > 0) {
    const int ls = LineStartOffset(src_, start - 1);
    std::string_view line = Slice(ls, start - 1);
    while (int w = SpaceRuneLen(line)) line.remove_prefix(w);
    if (line.empty() || (line[0] != '#' && line.substr(0, 2) != "//")) break;
    start = ls;
  }
  return start;
}

}  // namespace protowire::pxf

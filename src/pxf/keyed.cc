// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// CanonicalizeKeyed — the C++ port of protowire-go's keyed.go canonicalizer.

#include "protowire/pxf/keyed.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "format_internal.h"
#include "protowire/detail/go_quote.h"
#include "protowire/pxf/annotations.h"

namespace protowire::pxf {

namespace pb = google::protobuf;

namespace {

template <class V, class T>
T* GetMut(V& v) {
  return std::holds_alternative<std::unique_ptr<T>>(v) ? std::get<std::unique_ptr<T>>(v).get()
                                                       : nullptr;
}

void CanonEntries(std::vector<EntryPtr>& entries, const pb::Descriptor* desc);

// DropKeyAssignments removes `key_name = "entry_name"` assignments — the
// redundant agreeing spelling of an entry's key — from entries. Disagreeing
// or non-string assignments are kept (the document is invalid; formatting
// must not silently change its meaning). Leading comments of a dropped
// assignment move to the next surviving entry; when none follows, they are
// returned, for the block's trailing comments (#176).
std::vector<Comment> DropKeyAssignments(std::vector<EntryPtr>& entries,
                                        std::string_view key_name,
                                        std::string_view entry_name) {
  std::vector<Comment> pending;
  std::vector<EntryPtr> out;
  out.reserve(entries.size());
  for (auto& e : entries) {
    if (auto* a = GetMut<EntryPtr, Assignment>(e); a && !a->key_quoted && a->key == key_name) {
      if (auto* sv = GetMut<ValuePtr, StringVal>(a->value); sv && sv->value == entry_name) {
        pending.insert(pending.end(), a->leading_comments.begin(), a->leading_comments.end());
        continue;
      }
    }
    // A type entry takes none; they wait for the next entry that does.
    if (!pending.empty() && !std::holds_alternative<std::unique_ptr<TypeEntry>>(e)) {
      std::visit(
          [&](auto& p) {
            p->leading_comments.insert(p->leading_comments.begin(), pending.begin(), pending.end());
          },
          e);
      pending.clear();
    }
    out.push_back(std::move(e));
  }
  entries = std::move(out);
  return pending;
}

// PrependComments puts `left` before the comments already in *to.
void PrependComments(std::vector<Comment> left, std::vector<Comment>* to) {
  left.insert(left.end(), std::make_move_iterator(to->begin()), std::make_move_iterator(to->end()));
  *to = std::move(left);
}

// CanonKeyedEntries normalizes the entries of a keyed block in place:
// assignment-spelled entries become blocks, identifier-safe quoted names
// are unquoted, redundant agreeing key assignments are dropped, and entry
// bodies are canonicalized recursively.
void CanonKeyedEntries(Block* b, const pb::FieldDescriptor* fd, const pb::FieldDescriptor* key_fd) {
  const std::string_view key_name = key_fd->name();
  for (auto& e : b->entries) {
    Block* eb = GetMut<EntryPtr, Block>(e);
    if (!eb) {
      if (auto* a = GetMut<EntryPtr, Assignment>(e)) {
        if (auto* bv = GetMut<ValuePtr, BlockVal>(a->value)) {
          auto nb = std::make_unique<Block>();
          nb->pos = a->pos;
          nb->end = a->end;
          nb->name = a->key;
          nb->name_quoted = a->key_quoted;
          nb->entries = std::move(bv->entries);
          nb->leading_comments = std::move(a->leading_comments);
          nb->trailing_comments = std::move(bv->trailing_comments);
          eb = nb.get();
          e = EntryPtr(std::move(nb));
        }
      }
    }
    if (!eb) continue;  // malformed entry; leave untouched
    if (eb->name_quoted && IsIdentifierSafeEntryName(eb->name)) eb->name_quoted = false;
    PrependComments(DropKeyAssignments(eb->entries, key_name, eb->name), &eb->trailing_comments);
    CanonEntries(eb->entries, fd->message_type());
  }
}

// CanonAnonymousKeyed converts an eligible anonymous list binding of a
// keyed repeated field to the keyed block form. Ineligible bindings
// (non-block elements, absent / empty / duplicate / non-string keys) stay
// anonymous; their element bodies are still canonicalized.
void CanonAnonymousKeyed(EntryPtr& entry,
                         Assignment* n,
                         ListVal* lv,
                         const pb::FieldDescriptor* fd,
                         const pb::FieldDescriptor* key_fd) {
  const std::string_view key_name = key_fd->name();
  std::vector<std::string> keys;
  std::unordered_set<std::string> seen;
  bool eligible = true;
  for (auto& v : lv->elements) {
    auto* bv = GetMut<ValuePtr, BlockVal>(v);
    std::string key;
    if (!bv || !ExplicitKeyOf(bv->entries, key_name, &key) || key.empty() ||
        !seen.insert(key).second) {
      eligible = false;
      break;
    }
    keys.push_back(key);
  }
  if (!eligible) {
    for (auto& v : lv->elements) {
      if (auto* bv = GetMut<ValuePtr, BlockVal>(v)) CanonEntries(bv->entries, fd->message_type());
    }
    return;
  }
  auto blk = std::make_unique<Block>();
  blk->pos = n->pos;
  blk->end = n->end;
  blk->name = n->key;
  blk->leading_comments = std::move(n->leading_comments);
  for (size_t i = 0; i < lv->elements.size(); ++i) {
    auto* bv = GetMut<ValuePtr, BlockVal>(lv->elements[i]);
    std::vector<Comment> left = DropKeyAssignments(bv->entries, key_name, keys[i]);
    CanonEntries(bv->entries, fd->message_type());
    auto eb = std::make_unique<Block>();
    eb->pos = bv->pos;
    eb->end = bv->end;
    eb->name = keys[i];
    eb->name_quoted = !IsIdentifierSafeEntryName(keys[i]);
    eb->entries = std::move(bv->entries);
    eb->trailing_comments = std::move(bv->trailing_comments);
    PrependComments(std::move(left), &eb->trailing_comments);
    blk->entries.push_back(EntryPtr(std::move(eb)));
  }
  entry = EntryPtr(std::move(blk));
}

void CanonAssignment(EntryPtr& entry, Assignment* n, const pb::FieldDescriptor* fd) {
  if (fd->is_map()) {
    auto* bv = GetMut<ValuePtr, BlockVal>(n->value);
    const auto* val_fd = fd->message_type()->map_value();
    if (!bv || val_fd->cpp_type() != pb::FieldDescriptor::CPPTYPE_MESSAGE) return;
    for (auto& e : bv->entries) {
      if (auto* me = GetMut<EntryPtr, MapEntry>(e)) {
        if (auto* inner = GetMut<ValuePtr, BlockVal>(me->value)) {
          CanonEntries(inner->entries, val_fd->message_type());
        }
      }
    }
    return;
  }
  if (fd->is_repeated()) {
    if (fd->cpp_type() != pb::FieldDescriptor::CPPTYPE_MESSAGE) return;
    const pb::FieldDescriptor* key_fd = KeyField(fd);
    if (auto* lv = GetMut<ValuePtr, ListVal>(n->value)) {
      if (key_fd) {
        CanonAnonymousKeyed(entry, n, lv, fd, key_fd);
        return;
      }
      for (auto& el : lv->elements) {
        if (auto* bv = GetMut<ValuePtr, BlockVal>(el))
          CanonEntries(bv->entries, fd->message_type());
      }
      return;
    }
    if (auto* bv = GetMut<ValuePtr, BlockVal>(n->value); bv && key_fd) {
      // `children = { ... }` → `children { ... }`.
      auto blk = std::make_unique<Block>();
      blk->pos = n->pos;
      blk->end = n->end;
      blk->name = n->key;
      blk->entries = std::move(bv->entries);
      blk->leading_comments = std::move(n->leading_comments);
      blk->trailing_comments = std::move(bv->trailing_comments);
      CanonKeyedEntries(blk.get(), fd, key_fd);
      entry = EntryPtr(std::move(blk));
    }
    return;
  }
  if (fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
    if (auto* bv = GetMut<ValuePtr, BlockVal>(n->value)) {
      CanonEntries(bv->entries, fd->message_type());
    }
  }
}

void CanonEntries(std::vector<EntryPtr>& entries, const pb::Descriptor* desc) {
  for (auto& e : entries) {
    if (auto* a = GetMut<EntryPtr, Assignment>(e)) {
      if (a->key_quoted) continue;  // invalid outside keyed blocks; leave untouched
      const pb::FieldDescriptor* fd = desc->FindFieldByName(a->key);
      if (!fd) continue;
      CanonAssignment(e, a, fd);
    } else if (auto* b = GetMut<EntryPtr, Block>(e)) {
      if (b->name_quoted) continue;
      const pb::FieldDescriptor* fd = desc->FindFieldByName(b->name);
      if (!fd) continue;
      if (const pb::FieldDescriptor* key_fd = KeyField(fd)) {
        CanonKeyedEntries(b, fd, key_fd);
        continue;
      }
      if (fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE && !fd->is_repeated() &&
          !fd->is_map()) {
        CanonEntries(b->entries, fd->message_type());
      }
    }
  }
}

// KeyedElem carries the key-field checks for the immediate body of one element
// of a keyed repeated field: an explicit assignment to the key field must not
// be empty, and in the named form must agree with the entry name.
struct KeyedElem {
  std::string field;
  const pb::FieldDescriptor* key_fd = nullptr;
  std::string entry_name;
  bool named = false;
};

KeyedError QuotedNameUnkeyed(Position pos, const std::string& name) {
  return KeyedError{pos,
                    KeyedErrorKind::kQuotedNameUnkeyed,
                    "",
                    name,
                    "quoted entry name " + detail::GoQuote(name) +
                        " is only valid inside a keyed repeated field's block (draft -01 §3.13)"};
}

std::optional<KeyedError> CheckExplicitKey(const KeyedElem& ke,
                                           const std::string& value,
                                           Position pos) {
  const std::string key(ke.key_fd->name());
  if (value.empty()) {
    return KeyedError{pos,
                      KeyedErrorKind::kEmptyKey,
                      ke.field,
                      "",
                      "explicit empty-string assignment to key field " + detail::GoQuote(key) +
                          " of keyed field " + detail::GoQuote(ke.field) +
                          ": the empty string is not a valid key"};
  }
  if (ke.named && value != ke.entry_name) {
    return KeyedError{pos,
                      KeyedErrorKind::kKeyConflict,
                      ke.field,
                      ke.entry_name,
                      "key field " + detail::GoQuote(key) + " = " + detail::GoQuote(value) +
                          " conflicts with entry name " + detail::GoQuote(ke.entry_name) +
                          " in keyed field " + detail::GoQuote(ke.field)};
  }
  return std::nullopt;
}

std::optional<KeyedError> BindingConflict(const KeyedElem& ke,
                                          const pb::FieldDescriptor* fd,
                                          bool is_null,
                                          Position pos) {
  if (!ke.named) return std::nullopt;
  const std::string key(ke.key_fd->name());
  std::string msg;
  if (fd->name() == ke.key_fd->name()) {
    if (!is_null) return std::nullopt;
    msg = "null on key field " + detail::GoQuote(key) + " conflicts with entry name " +
          detail::GoQuote(ke.entry_name) + " in keyed field " + detail::GoQuote(ke.field);
  } else {
    const auto* oo = fd->real_containing_oneof();
    if (oo == nullptr || oo != ke.key_fd->real_containing_oneof()) return std::nullopt;
    msg = "field " + detail::GoQuote(fd->name()) + " shares oneof " + detail::GoQuote(oo->name()) +
          " with key field " + detail::GoQuote(key) + ", which entry name " +
          detail::GoQuote(ke.entry_name) + " already set in keyed field " +
          detail::GoQuote(ke.field);
  }
  return KeyedError{pos, KeyedErrorKind::kKeyConflict, ke.field, ke.entry_name, std::move(msg)};
}

template <class V, class T>
const T* Get(const V& v) {
  return std::holds_alternative<std::unique_ptr<T>>(v) ? std::get<std::unique_ptr<T>>(v).get()
                                                       : nullptr;
}

void DiagEntries(const std::vector<EntryPtr>& entries,
                 const pb::Descriptor* desc,
                 const KeyedElem* ke,
                 std::vector<KeyedError>* out);

// DiagKeyedBlock checks the entries of one keyed block: empty and duplicate
// names, compared unquoted, then each entry's body with the key field in hand.
void DiagKeyedBlock(const std::vector<EntryPtr>& entries,
                    const pb::FieldDescriptor* fd,
                    const pb::FieldDescriptor* key_fd,
                    std::vector<KeyedError>* out) {
  const std::string_view field = fd->name();
  std::unordered_set<std::string> seen;
  for (const auto& e : entries) {
    std::string name;
    Position pos;
    const std::vector<EntryPtr>* body = nullptr;
    if (const auto* b = Get<EntryPtr, Block>(e)) {
      name = b->name;
      pos = b->pos;
      body = &b->entries;
    } else if (const auto* a = Get<EntryPtr, Assignment>(e)) {
      // A non-block value on a named entry is an ordinary decode error.
      const auto* bv = Get<ValuePtr, BlockVal>(a->value);
      if (bv == nullptr) continue;
      name = a->key;
      pos = a->pos;
      body = &bv->entries;
    } else {
      continue;
    }
    if (name.empty()) {
      out->push_back(KeyedError{pos,
                                KeyedErrorKind::kEmptyKey,
                                std::string(field),
                                "",
                                "empty entry name in keyed field " + detail::GoQuote(field) +
                                    ": the empty string is not a valid key"});
    } else if (!seen.insert(name).second) {
      out->push_back(KeyedError{
          pos,
          KeyedErrorKind::kDuplicateKey,
          std::string(field),
          name,
          "duplicate key " + detail::GoQuote(name) + " in keyed field " + detail::GoQuote(field)});
    }
    const KeyedElem elem{std::string(field), key_fd, name, true};
    DiagEntries(*body, fd->message_type(), &elem, out);
  }
}

// DiagValue descends into an assignment's value per fd's shape: keyed blocks,
// anonymous list elements, singular submessages, message-valued map entries.
void DiagValue(const ValuePtr& v, const pb::FieldDescriptor* fd, std::vector<KeyedError>* out) {
  const bool is_message = fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE;
  if (fd->is_map()) {
    const auto* bv = Get<ValuePtr, BlockVal>(v);
    const auto* val_fd = fd->message_type()->map_value();
    if (bv == nullptr || val_fd->cpp_type() != pb::FieldDescriptor::CPPTYPE_MESSAGE) return;
    for (const auto& e : bv->entries) {
      const auto* me = Get<EntryPtr, MapEntry>(e);
      if (me == nullptr) continue;
      if (const auto* inner = Get<ValuePtr, BlockVal>(me->value)) {
        DiagEntries(inner->entries, val_fd->message_type(), nullptr, out);
      }
    }
    return;
  }
  if (fd->is_repeated()) {
    if (!is_message) return;
    const pb::FieldDescriptor* key_fd = KeyField(fd);
    if (const auto* list = Get<ValuePtr, ListVal>(v)) {
      const KeyedElem elem{std::string(fd->name()), key_fd, "", false};
      for (const auto& el : list->elements) {
        if (const auto* bv = Get<ValuePtr, BlockVal>(el)) {
          DiagEntries(bv->entries, fd->message_type(), key_fd ? &elem : nullptr, out);
        }
      }
    } else if (const auto* bv = Get<ValuePtr, BlockVal>(v); bv && key_fd) {
      // `name = { ... }` spelling of the keyed block form.
      DiagKeyedBlock(bv->entries, fd, key_fd, out);
    }
    return;
  }
  if (is_message) {
    if (const auto* bv = Get<ValuePtr, BlockVal>(v)) {
      DiagEntries(bv->entries, fd->message_type(), nullptr, out);
    }
  }
}

void DiagEntries(const std::vector<EntryPtr>& entries,
                 const pb::Descriptor* desc,
                 const KeyedElem* ke,
                 std::vector<KeyedError>* out) {
  for (const auto& e : entries) {
    if (const auto* n = Get<EntryPtr, Assignment>(e)) {
      if (n->key_quoted) {
        out->push_back(QuotedNameUnkeyed(n->pos, n->key));
        continue;
      }
      const pb::FieldDescriptor* fd = desc->FindFieldByName(n->key);
      if (fd == nullptr) continue;
      if (ke != nullptr) {
        const bool is_null = Get<ValuePtr, NullVal>(n->value) != nullptr;
        if (auto err = BindingConflict(*ke, fd, is_null, n->pos)) out->push_back(std::move(*err));
        if (fd->name() == ke->key_fd->name()) {
          if (const auto* sv = Get<ValuePtr, StringVal>(n->value)) {
            if (auto err = CheckExplicitKey(*ke, sv->value, sv->pos)) {
              out->push_back(std::move(*err));
            }
          }
        }
      }
      DiagValue(n->value, fd, out);
    } else if (const auto* b = Get<EntryPtr, Block>(e)) {
      if (b->name_quoted) {
        out->push_back(QuotedNameUnkeyed(b->pos, b->name));
        continue;
      }
      const pb::FieldDescriptor* fd = desc->FindFieldByName(b->name);
      if (fd == nullptr) continue;
      if (ke != nullptr) {
        if (auto err = BindingConflict(*ke, fd, false, b->pos)) out->push_back(std::move(*err));
      }
      if (const pb::FieldDescriptor* key_fd = KeyField(fd)) {
        DiagKeyedBlock(b->entries, fd, key_fd, out);
        continue;
      }
      const bool is_message = fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE;
      if (fd->is_repeated() && !fd->is_map() && is_message) {
        // Block form on a repeated field with no (pxf.key): the shape is an
        // ordinary schema error, but a quoted entry name inside is this one.
        for (const auto& be : b->entries) {
          if (const auto* a = Get<EntryPtr, Assignment>(be); a && a->key_quoted) {
            out->push_back(QuotedNameUnkeyed(a->pos, a->key));
          } else if (const auto* bb = Get<EntryPtr, Block>(be); bb && bb->name_quoted) {
            out->push_back(QuotedNameUnkeyed(bb->pos, bb->name));
          }
        }
        continue;
      }
      if (is_message && !fd->is_repeated()) {
        DiagEntries(b->entries, fd->message_type(), nullptr, out);
      }
    }
  }
}

}  // namespace

const char* KeyedErrorKindName(KeyedErrorKind k) {
  switch (k) {
    case KeyedErrorKind::kDuplicateKey:
      return "duplicate key";
    case KeyedErrorKind::kKeyConflict:
      return "key conflict";
    case KeyedErrorKind::kEmptyKey:
      return "empty key";
    case KeyedErrorKind::kQuotedNameUnkeyed:
      return "quoted entry name outside keyed field";
  }
  return "?";
}

std::vector<KeyedError> KeyedDiagnostics(const Document& doc, const pb::Descriptor* desc) {
  std::vector<KeyedError> out;
  if (desc == nullptr) return out;
  DiagEntries(doc.entries, desc, nullptr, &out);
  std::stable_sort(out.begin(), out.end(), [](const KeyedError& a, const KeyedError& b) {
    return a.pos.offset < b.pos.offset;
  });
  return out;
}

bool IsIdentifierSafeEntryName(std::string_view s) {
  if (s.empty() || s == "true" || s == "false" || s == "null") return false;
  auto start = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
  if (!start(s[0])) return false;
  for (size_t i = 1; i < s.size(); ++i) {
    char c = s[i];
    if (!(start(c) || (c >= '0' && c <= '9') || c == '.')) return false;
  }
  return true;
}

void CanonicalizeKeyed(Document* doc, const pb::Descriptor* desc) {
  if (doc == nullptr || desc == nullptr) return;
  CanonEntries(doc->entries, desc);
}

bool ExplicitKeyOf(const std::vector<EntryPtr>& entries,
                   std::string_view key_name,
                   std::string* key) {
  bool found = false;
  for (const auto& e : entries) {
    const auto* a = std::holds_alternative<std::unique_ptr<Assignment>>(e)
                        ? std::get<std::unique_ptr<Assignment>>(e).get()
                        : nullptr;
    if (!a || a->key != key_name) continue;
    const auto* sv = std::holds_alternative<std::unique_ptr<StringVal>>(a->value)
                         ? std::get<std::unique_ptr<StringVal>>(a->value).get()
                         : nullptr;
    if (!sv || a->key_quoted || found) return false;
    *key = sv->value;
    found = true;
  }
  return found;
}

}  // namespace protowire::pxf

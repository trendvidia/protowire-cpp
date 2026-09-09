// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// CanonicalizeKeyed — the C++ port of protowire-go's keyed.go canonicalizer.

#include "protowire/pxf/keyed.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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
// assignment move to the next surviving entry.
void DropKeyAssignments(std::vector<EntryPtr>& entries,
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
    if (!pending.empty()) {
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
}

// ExplicitKeyOf returns the value of the single explicit `key_name = "…"`
// assignment among entries; false when there is none, more than one, or
// it is not a string.
bool ExplicitKeyOf(const std::vector<EntryPtr>& entries,
                   std::string_view key_name,
                   std::string* key) {
  bool found = false;
  for (const auto& e : entries) {
    const auto* a = std::holds_alternative<std::unique_ptr<Assignment>>(e)
                        ? std::get<std::unique_ptr<Assignment>>(e).get()
                        : nullptr;
    if (!a || a->key_quoted || a->key != key_name) continue;
    if (found) return false;
    const auto* sv = std::holds_alternative<std::unique_ptr<StringVal>>(a->value)
                         ? std::get<std::unique_ptr<StringVal>>(a->value).get()
                         : nullptr;
    if (!sv) return false;
    *key = sv->value;
    found = true;
  }
  return found;
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
          nb->name = a->key;
          nb->name_quoted = a->key_quoted;
          nb->entries = std::move(bv->entries);
          nb->leading_comments = std::move(a->leading_comments);
          eb = nb.get();
          e = EntryPtr(std::move(nb));
        }
      }
    }
    if (!eb) continue;  // malformed entry; leave untouched
    if (eb->name_quoted && IsIdentifierSafeEntryName(eb->name)) eb->name_quoted = false;
    DropKeyAssignments(eb->entries, key_name, eb->name);
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
  blk->name = n->key;
  blk->leading_comments = std::move(n->leading_comments);
  for (size_t i = 0; i < lv->elements.size(); ++i) {
    auto* bv = GetMut<ValuePtr, BlockVal>(lv->elements[i]);
    DropKeyAssignments(bv->entries, key_name, keys[i]);
    CanonEntries(bv->entries, fd->message_type());
    auto eb = std::make_unique<Block>();
    eb->pos = bv->pos;
    eb->name = keys[i];
    eb->name_quoted = !IsIdentifierSafeEntryName(keys[i]);
    eb->entries = std::move(bv->entries);
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
      blk->name = n->key;
      blk->entries = std::move(bv->entries);
      blk->leading_comments = std::move(n->leading_comments);
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

}  // namespace

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

}  // namespace protowire::pxf

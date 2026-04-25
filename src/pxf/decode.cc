// PXF → google::protobuf::Message decoding.
//
// Drives the existing AST built by parser.cc: walks the Document, looks up
// each field on the message descriptor, decodes the value through the
// libprotobuf Reflection API, and recurses for nested messages, lists, and
// maps.
//
// Coverage today: scalars (all proto3 numeric kinds, bool, string, bytes,
// enum), repeated scalars/messages, maps with scalar/message values, nested
// messages, and well-known Timestamp/Duration/Wrapper sugar.
//
// Deferred (not yet wired): google.protobuf.Any sugar, pxf.BigInt/Decimal/
// BigFloat sugar, _null FieldMask discovery, (pxf.required) / (pxf.default)
// annotation enforcement in UnmarshalFull. Returning Status::Error early
// keeps these as well-defined gaps rather than silent miscompiles.

#include "protowire/pxf.h"

#include <charconv>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
#include "protowire/detail/rfc3339.h"
#include "protowire/pxf/ast.h"
#include "protowire/pxf/parser.h"
#include "protowire/pxf/wellknown.h"

namespace protowire::pxf {

namespace pb = google::protobuf;
using FieldDescriptor = pb::FieldDescriptor;
using Descriptor = pb::Descriptor;
using Reflection = pb::Reflection;
using Message = pb::Message;

namespace {

template <class V, class T>
const T* Get(const V& v) {
  return std::holds_alternative<std::unique_ptr<T>>(v)
             ? std::get<std::unique_ptr<T>>(v).get()
             : nullptr;
}

Status PosError(Position p, std::string msg) {
  if (p.line > 0) return Status::Error(p.line, p.column, std::move(msg));
  return Status::Error(std::move(msg));
}

// ------------------------------------------------------------------ scalar

template <class T>
bool ParseInteger(std::string_view s, T& out) {
  auto* first = s.data();
  auto* last = s.data() + s.size();
  auto r = std::from_chars(first, last, out);
  return r.ec == std::errc() && r.ptr == last;
}

bool ParseDouble(std::string_view s, double& out) {
  // std::from_chars for double exists in C++17 but not all libs ship it.
  // strtod is good enough; require null-terminated.
  std::string buf(s);
  char* end = nullptr;
  out = std::strtod(buf.c_str(), &end);
  return end != buf.c_str() && static_cast<size_t>(end - buf.c_str()) == buf.size();
}

Status SetScalar(Message* msg, const FieldDescriptor* fd, const ValuePtr& v,
                 Position pos);

// DecodeContext threads root-message state needed for cross-cutting concerns:
// _null FieldMask writes and Any-sugar TypeResolver lookup.
struct DecodeContext {
  Message* root = nullptr;
  const FieldDescriptor* null_mask_fd = nullptr;
  Result* result = nullptr;
  TypeResolver* resolver = nullptr;
};

Status DecodeMessage(Message* msg, const std::vector<EntryPtr>& entries,
                     std::string path_prefix, DecodeContext* ctx);

Status DecodeMessageValueInto(Message* sub, const ValuePtr& v, Position pos,
                              std::string path_prefix, DecodeContext* ctx) {
  // Special cases for well-known types appearing as scalars.
  const Descriptor* d = sub->GetDescriptor();
  if (IsTimestamp(d)) {
    if (auto* ts = Get<ValuePtr, TimestampVal>(v)) {
      SetTimestampFields(sub, ts->seconds, ts->nanos);
      return Status::OK();
    }
  }
  if (IsDuration(d)) {
    if (auto* du = Get<ValuePtr, DurationVal>(v)) {
      SetDurationFields(sub, du->seconds, du->nanos);
      return Status::OK();
    }
  }
  // Wrapper sugar: bare scalar → wrapper.value.
  int wt = WrapperInnerCppType(d);
  if (wt != -1) {
    const FieldDescriptor* inner = d->FindFieldByName("value");
    if (!inner) return PosError(pos, "wrapper missing 'value' field");
    if (!Get<ValuePtr, BlockVal>(v)) {
      return SetScalar(sub, inner, v, pos);
    }
  }
  // Big-number sugar: bare numeric literal → fill byte-magnitude fields.
  if (IsBigInt(d)) {
    if (auto* iv = Get<ValuePtr, IntVal>(v)) {
      if (!SetBigIntFromString(sub, iv->raw)) {
        return PosError(pos, "invalid pxf.BigInt literal: " + iv->raw);
      }
      return Status::OK();
    }
  }
  if (IsDecimal(d)) {
    if (auto* iv = Get<ValuePtr, IntVal>(v)) {
      if (!SetDecimalFromString(sub, iv->raw)) {
        return PosError(pos, "invalid pxf.Decimal literal: " + iv->raw);
      }
      return Status::OK();
    }
    if (auto* fv = Get<ValuePtr, FloatVal>(v)) {
      if (!SetDecimalFromString(sub, fv->raw)) {
        return PosError(pos, "invalid pxf.Decimal literal: " + fv->raw);
      }
      return Status::OK();
    }
  }
  if (IsBigFloat(d)) {
    if (auto* iv = Get<ValuePtr, IntVal>(v)) {
      if (!SetBigFloatFromString(sub, iv->raw)) {
        return PosError(pos, "invalid pxf.BigFloat literal: " + iv->raw);
      }
      return Status::OK();
    }
    if (auto* fv = Get<ValuePtr, FloatVal>(v)) {
      if (!SetBigFloatFromString(sub, fv->raw)) {
        return PosError(pos, "invalid pxf.BigFloat literal: " + fv->raw);
      }
      return Status::OK();
    }
  }
  // Otherwise, expect a block.
  auto* bv = Get<ValuePtr, BlockVal>(v);
  if (!bv) {
    return PosError(pos, "expected '{' for message field of " +
                             std::string(d->full_name()));
  }
  // google.protobuf.Any sugar: if the block starts with @type = "...", the
  // remaining entries are decoded against the resolved descriptor, then
  // packed into Any.value. Requires a TypeResolver in UnmarshalOptions.
  if (IsAny(d) && ctx->resolver != nullptr && !bv->entries.empty()) {
    if (auto* head = Get<EntryPtr, Assignment>(bv->entries.front())) {
      if (head->key == "@type") {
        auto* sv = Get<ValuePtr, StringVal>(head->value);
        if (!sv) {
          return PosError(head->pos, "@type must be a string literal");
        }
        std::string type_url = sv->value;
        const Descriptor* inner_desc =
            ctx->resolver->FindMessageByURL(type_url);
        if (!inner_desc) {
          return PosError(head->pos,
                          "cannot resolve Any type \"" + type_url + "\"");
        }
        google::protobuf::DynamicMessageFactory factory(inner_desc->file()->pool());
        std::unique_ptr<Message> inner(
            factory.GetPrototype(inner_desc)->New());
        // Decode the remaining entries into `inner`.
        std::vector<EntryPtr> rest;
        rest.reserve(bv->entries.size() - 1);
        for (size_t i = 1; i < bv->entries.size(); ++i) {
          rest.push_back(std::move(const_cast<EntryPtr&>(bv->entries[i])));
        }
        DecodeContext inner_ctx;  // do not propagate _null/result into inner
        inner_ctx.root = inner.get();
        inner_ctx.resolver = ctx->resolver;
        Status st = DecodeMessage(inner.get(), rest, "", &inner_ctx);
        if (!st.ok()) return st;
        std::string packed;
        if (!inner->SerializeToString(&packed)) {
          return PosError(pos, "Any inner serialization failed");
        }
        sub->GetReflection()->SetString(
            sub, sub->GetDescriptor()->FindFieldByName("type_url"),
            std::move(type_url));
        sub->GetReflection()->SetString(
            sub, sub->GetDescriptor()->FindFieldByName("value"),
            std::move(packed));
        return Status::OK();
      }
    }
  }
  return DecodeMessage(sub, bv->entries, std::move(path_prefix), ctx);
}

Status SetScalar(Message* msg, const FieldDescriptor* fd, const ValuePtr& v,
                 Position pos) {
  const Reflection* r = msg->GetReflection();
  switch (fd->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING: {
      if (auto* s = Get<ValuePtr, StringVal>(v)) {
        if (fd->type() == FieldDescriptor::TYPE_BYTES) {
          // bytes from a STRING token is unusual but accept it.
          r->SetString(msg, fd, s->value);
        } else {
          r->SetString(msg, fd, s->value);
        }
        return Status::OK();
      }
      if (auto* b = Get<ValuePtr, BytesVal>(v)) {
        std::string bytes(b->value.begin(), b->value.end());
        r->SetString(msg, fd, std::move(bytes));
        return Status::OK();
      }
      return PosError(pos, "expected string for field \"" +
                               std::string(fd->name()) + "\"");
    }
    case FieldDescriptor::CPPTYPE_BOOL: {
      if (auto* b = Get<ValuePtr, BoolVal>(v)) {
        r->SetBool(msg, fd, b->value);
        return Status::OK();
      }
      return PosError(pos, "expected bool for field \"" +
                               std::string(fd->name()) + "\"");
    }
    case FieldDescriptor::CPPTYPE_INT32: {
      auto* i = Get<ValuePtr, IntVal>(v);
      if (!i) return PosError(pos, "expected integer for " + std::string(fd->name()));
      int32_t n;
      if (!ParseInteger<int32_t>(i->raw, n))
        return PosError(pos, "invalid int32: " + i->raw);
      r->SetInt32(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_INT64: {
      auto* i = Get<ValuePtr, IntVal>(v);
      if (!i) return PosError(pos, "expected integer for " + std::string(fd->name()));
      int64_t n;
      if (!ParseInteger<int64_t>(i->raw, n))
        return PosError(pos, "invalid int64: " + i->raw);
      r->SetInt64(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT32: {
      auto* i = Get<ValuePtr, IntVal>(v);
      if (!i) return PosError(pos, "expected integer for " + std::string(fd->name()));
      uint32_t n;
      if (!ParseInteger<uint32_t>(i->raw, n))
        return PosError(pos, "invalid uint32: " + i->raw);
      r->SetUInt32(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT64: {
      auto* i = Get<ValuePtr, IntVal>(v);
      if (!i) return PosError(pos, "expected integer for " + std::string(fd->name()));
      uint64_t n;
      if (!ParseInteger<uint64_t>(i->raw, n))
        return PosError(pos, "invalid uint64: " + i->raw);
      r->SetUInt64(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_FLOAT:
    case FieldDescriptor::CPPTYPE_DOUBLE: {
      double d;
      if (auto* fv = Get<ValuePtr, FloatVal>(v)) {
        if (!ParseDouble(fv->raw, d))
          return PosError(pos, "invalid float: " + fv->raw);
      } else if (auto* iv = Get<ValuePtr, IntVal>(v)) {
        if (!ParseDouble(iv->raw, d))
          return PosError(pos, "invalid float: " + iv->raw);
      } else {
        return PosError(pos, "expected number for " + std::string(fd->name()));
      }
      if (fd->cpp_type() == FieldDescriptor::CPPTYPE_FLOAT) {
        r->SetFloat(msg, fd, static_cast<float>(d));
      } else {
        r->SetDouble(msg, fd, d);
      }
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_ENUM: {
      const auto* enum_d = fd->enum_type();
      if (auto* id = Get<ValuePtr, IdentVal>(v)) {
        const auto* ev = enum_d->FindValueByName(id->name);
        if (!ev) {
          return PosError(pos, "unknown enum value \"" + id->name + "\" for " +
                                   std::string(enum_d->full_name()));
        }
        r->SetEnumValue(msg, fd, ev->number());
        return Status::OK();
      }
      if (auto* iv = Get<ValuePtr, IntVal>(v)) {
        int32_t n;
        if (!ParseInteger<int32_t>(iv->raw, n))
          return PosError(pos, "invalid enum number: " + iv->raw);
        r->SetEnumValue(msg, fd, n);
        return Status::OK();
      }
      return PosError(pos, "expected enum name or number for " +
                               std::string(fd->name()));
    }
    case FieldDescriptor::CPPTYPE_MESSAGE: {
      // Caller should have routed through DecodeMessageValueInto.
      return PosError(pos, "unexpected scalar dispatch for message field");
    }
  }
  return PosError(pos, "unsupported field type");
}

// Add an element to a repeated field (already in repeated context).
Status AddRepeatedScalar(Message* msg, const FieldDescriptor* fd,
                        const ValuePtr& v, Position pos) {
  const Reflection* r = msg->GetReflection();
  switch (fd->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING: {
      if (auto* s = Get<ValuePtr, StringVal>(v)) {
        r->AddString(msg, fd, s->value);
        return Status::OK();
      }
      if (auto* b = Get<ValuePtr, BytesVal>(v)) {
        r->AddString(msg, fd, std::string(b->value.begin(), b->value.end()));
        return Status::OK();
      }
      return PosError(pos, "expected string in repeated field");
    }
    case FieldDescriptor::CPPTYPE_BOOL:
      if (auto* b = Get<ValuePtr, BoolVal>(v)) {
        r->AddBool(msg, fd, b->value);
        return Status::OK();
      }
      return PosError(pos, "expected bool in repeated field");
    case FieldDescriptor::CPPTYPE_INT32: {
      auto* i = Get<ValuePtr, IntVal>(v);
      if (!i) return PosError(pos, "expected int32 in repeated field");
      int32_t n;
      if (!ParseInteger<int32_t>(i->raw, n))
        return PosError(pos, "invalid int32: " + i->raw);
      r->AddInt32(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_INT64: {
      auto* i = Get<ValuePtr, IntVal>(v);
      if (!i) return PosError(pos, "expected int64 in repeated field");
      int64_t n;
      if (!ParseInteger<int64_t>(i->raw, n))
        return PosError(pos, "invalid int64: " + i->raw);
      r->AddInt64(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT32: {
      auto* i = Get<ValuePtr, IntVal>(v);
      if (!i) return PosError(pos, "expected uint32 in repeated field");
      uint32_t n;
      if (!ParseInteger<uint32_t>(i->raw, n))
        return PosError(pos, "invalid uint32: " + i->raw);
      r->AddUInt32(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT64: {
      auto* i = Get<ValuePtr, IntVal>(v);
      if (!i) return PosError(pos, "expected uint64 in repeated field");
      uint64_t n;
      if (!ParseInteger<uint64_t>(i->raw, n))
        return PosError(pos, "invalid uint64: " + i->raw);
      r->AddUInt64(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_FLOAT:
    case FieldDescriptor::CPPTYPE_DOUBLE: {
      double d;
      if (auto* fv = Get<ValuePtr, FloatVal>(v)) {
        if (!ParseDouble(fv->raw, d))
          return PosError(pos, "invalid float: " + fv->raw);
      } else if (auto* iv = Get<ValuePtr, IntVal>(v)) {
        if (!ParseDouble(iv->raw, d))
          return PosError(pos, "invalid float: " + iv->raw);
      } else {
        return PosError(pos, "expected number in repeated field");
      }
      if (fd->cpp_type() == FieldDescriptor::CPPTYPE_FLOAT) {
        r->AddFloat(msg, fd, static_cast<float>(d));
      } else {
        r->AddDouble(msg, fd, d);
      }
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_ENUM: {
      const auto* enum_d = fd->enum_type();
      if (auto* id = Get<ValuePtr, IdentVal>(v)) {
        const auto* ev = enum_d->FindValueByName(id->name);
        if (!ev) return PosError(pos, "unknown enum value \"" + id->name + "\"");
        r->AddEnumValue(msg, fd, ev->number());
        return Status::OK();
      }
      if (auto* iv = Get<ValuePtr, IntVal>(v)) {
        int32_t n;
        if (!ParseInteger<int32_t>(iv->raw, n))
          return PosError(pos, "invalid enum number: " + iv->raw);
        r->AddEnumValue(msg, fd, n);
        return Status::OK();
      }
      return PosError(pos, "expected enum in repeated field");
    }
    case FieldDescriptor::CPPTYPE_MESSAGE:
      return PosError(pos, "AddRepeatedScalar called for message field");
  }
  return PosError(pos, "unsupported repeated scalar type");
}

Status DecodeListInto(Message* msg, const FieldDescriptor* fd,
                      const ListVal& list, std::string path_prefix,
                      DecodeContext* ctx) {
  const Reflection* r = msg->GetReflection();
  for (const ValuePtr& el : list.elements) {
    Position p = ValuePos(el);
    if (Get<ValuePtr, NullVal>(el)) {
      return PosError(p, "null is not allowed in repeated field \"" +
                             std::string(fd->name()) + "\"");
    }
    if (fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      Message* sub = r->AddMessage(msg, fd);
      Status st = DecodeMessageValueInto(sub, el, p, path_prefix, ctx);
      if (!st.ok()) return st;
    } else {
      Status st = AddRepeatedScalar(msg, fd, el, p);
      if (!st.ok()) return st;
    }
  }
  return Status::OK();
}

Status DecodeMapInto(Message* msg, const FieldDescriptor* fd,
                     const std::vector<EntryPtr>& entries,
                     std::string path_prefix, DecodeContext* ctx) {
  const Reflection* r = msg->GetReflection();
  const FieldDescriptor* key_fd = fd->message_type()->map_key();
  const FieldDescriptor* val_fd = fd->message_type()->map_value();

  for (const EntryPtr& e : entries) {
    auto* m = Get<EntryPtr, MapEntry>(e);
    if (!m) {
      return PosError(EntryPos(e),
                      "map field \"" + std::string(fd->name()) +
                          "\" expects 'key: value' entries");
    }
    if (Get<ValuePtr, NullVal>(m->value)) {
      return PosError(m->pos, "null is not allowed as map value in \"" +
                                  std::string(fd->name()) + "\"");
    }
    // Build a fresh entry message via AddMessage on the map field.
    Message* entry = r->AddMessage(msg, fd);
    const Reflection* er = entry->GetReflection();
    // Decode key.
    switch (key_fd->cpp_type()) {
      case FieldDescriptor::CPPTYPE_STRING:
        er->SetString(entry, key_fd, m->key);
        break;
      case FieldDescriptor::CPPTYPE_INT32: {
        int32_t n;
        if (!ParseInteger<int32_t>(m->key, n))
          return PosError(m->pos, "invalid int32 map key: " + m->key);
        er->SetInt32(entry, key_fd, n);
        break;
      }
      case FieldDescriptor::CPPTYPE_INT64: {
        int64_t n;
        if (!ParseInteger<int64_t>(m->key, n))
          return PosError(m->pos, "invalid int64 map key: " + m->key);
        er->SetInt64(entry, key_fd, n);
        break;
      }
      case FieldDescriptor::CPPTYPE_UINT32: {
        uint32_t n;
        if (!ParseInteger<uint32_t>(m->key, n))
          return PosError(m->pos, "invalid uint32 map key: " + m->key);
        er->SetUInt32(entry, key_fd, n);
        break;
      }
      case FieldDescriptor::CPPTYPE_UINT64: {
        uint64_t n;
        if (!ParseInteger<uint64_t>(m->key, n))
          return PosError(m->pos, "invalid uint64 map key: " + m->key);
        er->SetUInt64(entry, key_fd, n);
        break;
      }
      case FieldDescriptor::CPPTYPE_BOOL:
        er->SetBool(entry, key_fd, m->key == "true");
        break;
      default:
        return PosError(m->pos, "unsupported map key kind");
    }
    // Decode value.
    if (val_fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      Message* sub = er->MutableMessage(entry, val_fd);
      Status st = DecodeMessageValueInto(sub, m->value, m->pos, path_prefix,
                                         ctx);
      if (!st.ok()) return st;
    } else {
      Status st = SetScalar(entry, val_fd, m->value, m->pos);
      if (!st.ok()) return st;
    }
  }
  return Status::OK();
}

Status DecodeAssignment(Message* msg, const std::string& key,
                        const ValuePtr& v, Position pos,
                        std::string path_prefix, DecodeContext* ctx) {
  const Descriptor* desc = msg->GetDescriptor();
  const FieldDescriptor* fd = desc->FindFieldByName(key);
  if (!fd) {
    return PosError(pos, "unknown field \"" + key + "\" in " +
                             std::string(desc->full_name()));
  }
  std::string path = path_prefix + key;

  if (Get<ValuePtr, NullVal>(v)) {
    if (ctx->result) ctx->result->MarkNull(path);
    if (ctx->null_mask_fd) {
      AppendNullPath(ctx->root, ctx->null_mask_fd, path);
    }
    return Status::OK();
  }
  if (ctx->result) ctx->result->MarkPresent(path);

  if (fd->is_map()) {
    auto* bv = Get<ValuePtr, BlockVal>(v);
    if (!bv) {
      return PosError(pos, "map field \"" + key + "\" expects '{ ... }'");
    }
    return DecodeMapInto(msg, fd, bv->entries, path + ".", ctx);
  }
  if (fd->is_repeated()) {
    auto* lv = Get<ValuePtr, ListVal>(v);
    if (!lv) {
      return PosError(pos, "repeated field \"" + key + "\" expects '[ ... ]'");
    }
    return DecodeListInto(msg, fd, *lv, path + ".", ctx);
  }
  if (fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
    Message* sub = msg->GetReflection()->MutableMessage(msg, fd);
    return DecodeMessageValueInto(sub, v, pos, path + ".", ctx);
  }
  return SetScalar(msg, fd, v, pos);
}

Status DecodeBlockEntry(Message* msg, const Block& block,
                        std::string path_prefix, DecodeContext* ctx) {
  const Descriptor* desc = msg->GetDescriptor();
  const FieldDescriptor* fd = desc->FindFieldByName(block.name);
  if (!fd) {
    return PosError(block.pos, "unknown field \"" + block.name + "\" in " +
                                   std::string(desc->full_name()));
  }
  if (fd->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
    return PosError(block.pos, "field \"" + block.name +
                                   "\" is not a message — block syntax forbidden");
  }
  if (fd->is_map()) {
    return PosError(block.pos, "map field \"" + block.name +
                                   "\" must use 'name = { ... }' syntax");
  }
  if (fd->is_repeated()) {
    return PosError(block.pos, "repeated field \"" + block.name +
                                   "\" must use list syntax");
  }
  std::string path = path_prefix + block.name;
  if (ctx->result) ctx->result->MarkPresent(path);
  Message* sub = msg->GetReflection()->MutableMessage(msg, fd);

  // Any-sugar: when the field is google.protobuf.Any and the block starts
  // with @type = "...", route through DecodeMessageValueInto with a synthetic
  // BlockVal so the Any-packing logic runs.
  if (IsAny(sub->GetDescriptor()) && ctx->resolver != nullptr &&
      !block.entries.empty()) {
    if (auto* head = Get<EntryPtr, Assignment>(block.entries.front())) {
      if (head->key == "@type") {
        auto bv = std::make_unique<BlockVal>();
        bv->pos = block.pos;
        // Move entries out of the Block — they're consumed here.
        bv->entries.reserve(block.entries.size());
        for (auto& e : const_cast<std::vector<EntryPtr>&>(block.entries)) {
          bv->entries.push_back(std::move(e));
        }
        ValuePtr v(std::move(bv));
        return DecodeMessageValueInto(sub, v, block.pos, path + ".", ctx);
      }
    }
  }

  return DecodeMessage(sub, block.entries, path + ".", ctx);
}

Status DecodeMessage(Message* msg, const std::vector<EntryPtr>& entries,
                     std::string path_prefix, DecodeContext* ctx) {
  for (const EntryPtr& e : entries) {
    if (auto* a = Get<EntryPtr, Assignment>(e)) {
      Status st = DecodeAssignment(msg, a->key, a->value, a->pos, path_prefix,
                                   ctx);
      if (!st.ok()) return st;
    } else if (auto* b = Get<EntryPtr, Block>(e)) {
      Status st = DecodeBlockEntry(msg, *b, path_prefix, ctx);
      if (!st.ok()) return st;
    } else {
      return PosError(EntryPos(e),
                      "map entry ':' is only valid inside a map context");
    }
  }
  return Status::OK();
}

}  // namespace

// AST-walking decoder used by callers that have already parsed a Document
// (e.g. Parse() consumers preserving comments). Not exposed publicly today;
// the public Unmarshal/UnmarshalFull route through the fused fast decoder
// in decode_fast.cc.
Status DecodeDocumentInto(const Document& doc, Message* msg,
                          UnmarshalOptions opts, Result* result) {
  DecodeContext ctx;
  ctx.root = msg;
  ctx.result = result;
  ctx.resolver = opts.type_resolver;
  ctx.null_mask_fd = result ? FindNullMaskField(msg->GetDescriptor()) : nullptr;
  return DecodeMessage(msg, doc.entries, "", &ctx);
}

}  // namespace protowire::pxf

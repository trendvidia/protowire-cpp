// Fused single-pass PXF decoder.
//
// This is the C++ port of decode_fast.go. The pipeline lexes one token,
// decides what proto field it corresponds to, and writes through
// libprotobuf::Reflection directly — no Document/Entry/Value AST nodes are
// ever allocated. Token values borrow from the input buffer via
// std::string_view, mirroring Go's `unsafe.String` zero-copy trick.
//
// Feature parity with the AST decoder in decode.cc:
//   - all proto3 scalars, enums, repeated, maps, nested messages
//   - WKT sugar: Timestamp, Duration, the wrappers, Any
//   - pxf.BigInt / pxf.Decimal / pxf.BigFloat sugar
//   - _null FieldMask write-through (UnmarshalFull only)
//   - oneof conflict detection
//   - UnmarshalOptions::discard_unknown
//   - Result presence tracking (UnmarshalFull only)
//
// Public Unmarshal / UnmarshalFull live here; the AST-based path moved to
// internal helpers in decode.cc, used only by Parse() consumers who want
// comments preserved.

#include "protowire/pxf.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
#include "protowire/detail/rfc3339.h"
#include "protowire/pxf/annotations.h"
#include "protowire/pxf/lexer.h"
#include "protowire/pxf/wellknown.h"

namespace protowire::pxf {

namespace pb = google::protobuf;
using FieldDescriptor = pb::FieldDescriptor;
using Descriptor = pb::Descriptor;
using Reflection = pb::Reflection;
using Message = pb::Message;

namespace {

Status PosError(Position p, std::string msg) {
  if (p.line > 0) return Status::Error(p.line, p.column, std::move(msg));
  return Status::Error(std::move(msg));
}

template <class T>
bool ParseInteger(std::string_view s, T& out) {
  auto* first = s.data();
  auto* last = s.data() + s.size();
  auto r = std::from_chars(first, last, out);
  return r.ec == std::errc() && r.ptr == last;
}

bool ParseDouble(std::string_view s, double& out) {
  std::string buf(s);
  char* end = nullptr;
  out = std::strtod(buf.c_str(), &end);
  return end != buf.c_str() &&
         static_cast<size_t>(end - buf.c_str()) == buf.size();
}

class DirectDecoder {
 public:
  DirectDecoder(std::string_view input, Message* root, Result* result,
                TypeResolver* resolver, bool discard_unknown)
      : lex_(input),
        root_(root),
        result_(result),
        resolver_(resolver),
        discard_unknown_(discard_unknown) {
    if (result_) {
      null_mask_fd_ = FindNullMaskField(root_->GetDescriptor());
    }
  }

  Status Run() {
    Advance();
    if (current_.kind == TokenKind::kAtType) {
      Advance();  // consume @type
      if (current_.kind != TokenKind::kIdent &&
          current_.kind != TokenKind::kString) {
        return PosError(current_.pos, "expected type name after @type");
      }
      Advance();
    }
    return DecodeFields(root_, /*in_block=*/false);
  }

 private:
  void Advance() {
    for (;;) {
      current_ = lex_.Next();
      if (current_.kind == TokenKind::kComment ||
          current_.kind == TokenKind::kNewline) {
        continue;
      }
      return;
    }
  }

  // ----- Field-level dispatch ------------------------------------------

  Status DecodeFields(Message* msg, bool in_block);
  Status DecodeFieldValue(Message* msg, const FieldDescriptor* fd);
  Status DecodeMsgValue(Message* msg, const FieldDescriptor* fd);
  Status DecodeListInline(Message* msg, const FieldDescriptor* fd);
  Status DecodeMapInline(Message* msg, const FieldDescriptor* fd);
  Status DecodeAnyInner(Message* any_msg);

  Status SetScalar(Message* msg, const FieldDescriptor* fd);
  Status AddRepeatedScalar(Message* msg, const FieldDescriptor* fd);
  Status SetEnum(Message* msg, const FieldDescriptor* fd, bool repeated);
  Status SetMapKey(Message* entry, const FieldDescriptor* key_fd,
                   std::string_view key, Position pos);

  Status CheckOneof(const FieldDescriptor* fd, Position pos,
                    std::unordered_map<std::string, std::string>* set_oneofs);

  // Skipping for unknown fields.
  void SkipValue();
  void SkipBraced();
  void SkipBracketed();

  Lexer lex_;
  Token current_;
  Message* root_ = nullptr;
  Result* result_ = nullptr;
  const FieldDescriptor* null_mask_fd_ = nullptr;
  TypeResolver* resolver_ = nullptr;
  bool discard_unknown_ = false;
  std::string path_prefix_;
};

// --- top-level body --------------------------------------------------------

Status DirectDecoder::DecodeFields(Message* msg, bool in_block) {
  const Descriptor* desc = msg->GetDescriptor();
  std::unordered_map<std::string, std::string> set_oneofs;

  for (;;) {
    if (in_block && current_.kind == TokenKind::kRBrace) {
      Advance();
      return Status::OK();
    }
    if (current_.kind == TokenKind::kEOF) {
      if (in_block) return PosError(current_.pos, "expected '}', got EOF");
      return Status::OK();
    }

    Position pos = current_.pos;
    if (current_.kind != TokenKind::kIdent &&
        current_.kind != TokenKind::kString &&
        current_.kind != TokenKind::kInt) {
      return PosError(pos,
                      std::string("expected identifier, string, or integer, got ") +
                          TokenKindName(current_.kind));
    }
    std::string key(current_.value);
    Advance();

    switch (current_.kind) {
      case TokenKind::kEquals: {
        Advance();
        const FieldDescriptor* fd = desc->FindFieldByName(key);
        if (!fd) {
          if (discard_unknown_) {
            SkipValue();
            continue;
          }
          return PosError(pos, "unknown field \"" + key + "\" in " +
                                   std::string(desc->full_name()));
        }
        Status st = CheckOneof(fd, pos, &set_oneofs);
        if (!st.ok()) return st;

        // null literal: track and continue without setting any field value.
        if (current_.kind == TokenKind::kNull) {
          if (result_) {
            std::string path = path_prefix_ + std::string(fd->name());
            result_->MarkNull(path);
            if (null_mask_fd_) {
              AppendNullPath(root_, null_mask_fd_, path);
            }
          }
          Advance();
          continue;
        }
        if (result_) {
          result_->MarkPresent(path_prefix_ + std::string(fd->name()));
        }
        st = DecodeFieldValue(msg, fd);
        if (!st.ok()) return st;
        break;
      }
      case TokenKind::kLBrace: {
        Advance();
        const FieldDescriptor* fd = desc->FindFieldByName(key);
        if (!fd) {
          if (discard_unknown_) {
            SkipBraced();
            continue;
          }
          return PosError(pos, "unknown field \"" + key + "\" in " +
                                   std::string(desc->full_name()));
        }
        if (fd->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
          return PosError(pos, "field \"" + key +
                                   "\" is not a message — block syntax forbidden");
        }
        if (fd->is_repeated()) {
          return PosError(pos, "repeated field \"" + key +
                                   "\" must use list syntax");
        }
        if (fd->is_map()) {
          return PosError(pos, "map field \"" + key +
                                   "\" must use 'name = { ... }' syntax");
        }
        Status st = CheckOneof(fd, pos, &set_oneofs);
        if (!st.ok()) return st;
        if (result_) {
          result_->MarkPresent(path_prefix_ + std::string(fd->name()));
        }
        Message* sub = msg->GetReflection()->MutableMessage(msg, fd);
        // Any-block sugar: name { @type = "..." ... }
        if (IsAny(sub->GetDescriptor()) && resolver_ != nullptr &&
            current_.kind == TokenKind::kAtType) {
          st = DecodeAnyInner(sub);
          if (!st.ok()) return st;
          break;
        }
        std::string saved = path_prefix_;
        path_prefix_ += std::string(fd->name());
        path_prefix_ += '.';
        st = DecodeFields(sub, /*in_block=*/true);
        path_prefix_ = std::move(saved);
        if (!st.ok()) return st;
        break;
      }
      case TokenKind::kColon:
        return PosError(pos,
                        "unexpected ':' in message context, use '=' for fields");
      default:
        return PosError(current_.pos,
                        std::string("expected '=', ':', or '{' after \"") + key +
                            "\", got " + TokenKindName(current_.kind));
    }
  }
}

Status DirectDecoder::CheckOneof(
    const FieldDescriptor* fd, Position pos,
    std::unordered_map<std::string, std::string>* set_oneofs) {
  // real_containing_oneof returns null for synthetic (proto3 optional) oneofs.
  const auto* oo = fd->real_containing_oneof();
  if (!oo) return Status::OK();
  std::string name(oo->name());
  auto it = set_oneofs->find(name);
  if (it != set_oneofs->end()) {
    return PosError(pos,
                    "oneof \"" + name + "\": field \"" + std::string(fd->name()) +
                        "\" conflicts with already-set field \"" + it->second + "\"");
  }
  (*set_oneofs)[name] = std::string(fd->name());
  return Status::OK();
}

// --- value dispatch --------------------------------------------------------

Status DirectDecoder::DecodeFieldValue(Message* msg, const FieldDescriptor* fd) {
  if (fd->is_map()) return DecodeMapInline(msg, fd);
  if (fd->is_repeated()) return DecodeListInline(msg, fd);
  if (fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
    return DecodeMsgValue(msg, fd);
  }
  return SetScalar(msg, fd);
}

Status DirectDecoder::DecodeMsgValue(Message* msg, const FieldDescriptor* fd) {
  const Descriptor* d = fd->message_type();
  Message* sub = msg->GetReflection()->MutableMessage(msg, fd);

  // Timestamp / Duration shortcut tokens.
  if (IsTimestamp(d) && current_.kind == TokenKind::kTimestamp) {
    auto t = detail::ParseRFC3339(current_.value);
    if (!t.has_value()) {
      return PosError(current_.pos, "invalid timestamp");
    }
    SetTimestampFields(sub, t->seconds, t->nanos);
    Advance();
    return Status::OK();
  }
  if (IsDuration(d) && current_.kind == TokenKind::kDuration) {
    auto dur = detail::ParseDuration(current_.value);
    if (!dur.has_value()) {
      return PosError(current_.pos, "invalid duration");
    }
    SetDurationFields(sub, dur->seconds, dur->nanos);
    Advance();
    return Status::OK();
  }
  // Wrapper sugar: bare scalar → wrapper.value.
  if (WrapperInnerCppType(d) != -1 && current_.kind != TokenKind::kLBrace) {
    const FieldDescriptor* inner = d->FindFieldByName("value");
    if (!inner) return PosError(current_.pos, "wrapper missing 'value' field");
    return SetScalar(sub, inner);
  }
  // Big-number sugar.
  if (IsBigInt(d) && current_.kind == TokenKind::kInt) {
    if (!SetBigIntFromString(sub, current_.value)) {
      return PosError(current_.pos,
                      "invalid pxf.BigInt: " + std::string(current_.value));
    }
    Advance();
    return Status::OK();
  }
  if (IsDecimal(d) &&
      (current_.kind == TokenKind::kInt || current_.kind == TokenKind::kFloat)) {
    if (!SetDecimalFromString(sub, current_.value)) {
      return PosError(current_.pos,
                      "invalid pxf.Decimal: " + std::string(current_.value));
    }
    Advance();
    return Status::OK();
  }
  if (IsBigFloat(d) &&
      (current_.kind == TokenKind::kInt || current_.kind == TokenKind::kFloat)) {
    if (!SetBigFloatFromString(sub, current_.value)) {
      return PosError(current_.pos,
                      "invalid pxf.BigFloat: " + std::string(current_.value));
    }
    Advance();
    return Status::OK();
  }
  // Any with assignment-syntax sugar: name = { @type = "..." ... }
  if (IsAny(d) && resolver_ != nullptr && current_.kind == TokenKind::kLBrace) {
    Advance();  // consume {
    return DecodeAnyInner(sub);
  }
  if (current_.kind != TokenKind::kLBrace) {
    return PosError(current_.pos,
                    "expected '{' for message field \"" +
                        std::string(fd->name()) + "\"");
  }
  Advance();
  std::string saved = path_prefix_;
  path_prefix_ += std::string(fd->name());
  path_prefix_ += '.';
  Status st = DecodeFields(sub, /*in_block=*/true);
  path_prefix_ = std::move(saved);
  return st;
}

// Caller has already consumed the opening '{' (block syntax) or '=' '{' pair
// (assignment syntax). On entry, current_ points at @type.
Status DirectDecoder::DecodeAnyInner(Message* any_msg) {
  if (current_.kind != TokenKind::kAtType) {
    return PosError(current_.pos, "Any field requires @type as first entry");
  }
  Advance();
  if (current_.kind != TokenKind::kEquals) {
    return PosError(current_.pos, "expected '=' after @type");
  }
  Advance();
  if (current_.kind != TokenKind::kString) {
    return PosError(current_.pos, "expected string type URL after @type =");
  }
  std::string type_url(current_.value);
  Advance();

  const Descriptor* inner_desc = resolver_->FindMessageByURL(type_url);
  if (!inner_desc) {
    return PosError(current_.pos,
                    "cannot resolve Any type \"" + type_url + "\"");
  }
  pb::DynamicMessageFactory factory(inner_desc->file()->pool());
  std::unique_ptr<Message> inner(factory.GetPrototype(inner_desc)->New());
  Status st = DecodeFields(inner.get(), /*in_block=*/true);
  if (!st.ok()) return st;
  std::string packed;
  if (!inner->SerializeToString(&packed)) {
    return Status::Error("Any inner serialization failed");
  }
  any_msg->GetReflection()->SetString(
      any_msg, any_msg->GetDescriptor()->FindFieldByName("type_url"),
      std::move(type_url));
  any_msg->GetReflection()->SetString(
      any_msg, any_msg->GetDescriptor()->FindFieldByName("value"),
      std::move(packed));
  return Status::OK();
}

// --- repeated --------------------------------------------------------------

Status DirectDecoder::DecodeListInline(Message* msg,
                                       const FieldDescriptor* fd) {
  if (current_.kind != TokenKind::kLBracket) {
    return PosError(current_.pos,
                    "expected '[' for repeated field \"" +
                        std::string(fd->name()) + "\"");
  }
  Advance();

  while (current_.kind != TokenKind::kRBracket &&
         current_.kind != TokenKind::kEOF) {
    if (current_.kind == TokenKind::kNull) {
      return PosError(current_.pos,
                      "null is not allowed in repeated field \"" +
                          std::string(fd->name()) + "\"");
    }
    if (fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      Message* sub = msg->GetReflection()->AddMessage(msg, fd);
      const Descriptor* d = fd->message_type();
      // Permit the same WKT/sugar tokens as scalar message fields.
      if (IsTimestamp(d) && current_.kind == TokenKind::kTimestamp) {
        auto t = detail::ParseRFC3339(current_.value);
        if (!t.has_value()) return PosError(current_.pos, "invalid timestamp");
        SetTimestampFields(sub, t->seconds, t->nanos);
        Advance();
      } else if (IsDuration(d) && current_.kind == TokenKind::kDuration) {
        auto dur = detail::ParseDuration(current_.value);
        if (!dur.has_value()) return PosError(current_.pos, "invalid duration");
        SetDurationFields(sub, dur->seconds, dur->nanos);
        Advance();
      } else if (WrapperInnerCppType(d) != -1 &&
                 current_.kind != TokenKind::kLBrace) {
        const FieldDescriptor* inner = d->FindFieldByName("value");
        Status st = SetScalar(sub, inner);
        if (!st.ok()) return st;
      } else if (IsBigInt(d) && current_.kind == TokenKind::kInt) {
        if (!SetBigIntFromString(sub, current_.value)) {
          return PosError(current_.pos, "invalid BigInt");
        }
        Advance();
      } else if (IsDecimal(d) && (current_.kind == TokenKind::kInt ||
                                   current_.kind == TokenKind::kFloat)) {
        if (!SetDecimalFromString(sub, current_.value)) {
          return PosError(current_.pos, "invalid Decimal");
        }
        Advance();
      } else if (IsBigFloat(d) && (current_.kind == TokenKind::kInt ||
                                    current_.kind == TokenKind::kFloat)) {
        if (!SetBigFloatFromString(sub, current_.value)) {
          return PosError(current_.pos, "invalid BigFloat");
        }
        Advance();
      } else {
        if (current_.kind != TokenKind::kLBrace) {
          return PosError(current_.pos,
                          "expected '{' for repeated message element");
        }
        Advance();
        Status st = DecodeFields(sub, /*in_block=*/true);
        if (!st.ok()) return st;
      }
    } else if (fd->cpp_type() == FieldDescriptor::CPPTYPE_ENUM) {
      Status st = SetEnum(msg, fd, /*repeated=*/true);
      if (!st.ok()) return st;
    } else {
      Status st = AddRepeatedScalar(msg, fd);
      if (!st.ok()) return st;
    }
    if (current_.kind == TokenKind::kComma) Advance();
  }
  if (current_.kind != TokenKind::kRBracket) {
    return PosError(current_.pos,
                    std::string("expected ']', got ") +
                        TokenKindName(current_.kind));
  }
  Advance();
  return Status::OK();
}

// --- map -------------------------------------------------------------------

Status DirectDecoder::DecodeMapInline(Message* msg, const FieldDescriptor* fd) {
  if (current_.kind != TokenKind::kLBrace) {
    return PosError(current_.pos,
                    "expected '{' for map field \"" +
                        std::string(fd->name()) + "\"");
  }
  Advance();
  const Reflection* r = msg->GetReflection();
  const FieldDescriptor* key_fd = fd->message_type()->map_key();
  const FieldDescriptor* val_fd = fd->message_type()->map_value();

  while (current_.kind != TokenKind::kRBrace &&
         current_.kind != TokenKind::kEOF) {
    Position pos = current_.pos;
    if (current_.kind != TokenKind::kIdent &&
        current_.kind != TokenKind::kString &&
        current_.kind != TokenKind::kInt) {
      return PosError(pos, std::string("expected map key, got ") +
                               TokenKindName(current_.kind));
    }
    std::string key(current_.value);
    Advance();
    switch (current_.kind) {
      case TokenKind::kColon: Advance(); break;
      case TokenKind::kEquals:
        return PosError(current_.pos,
                        "unexpected '=' in map, use ':' for map entries");
      default:
        return PosError(current_.pos,
                        std::string("expected ':' after map key, got ") +
                            TokenKindName(current_.kind));
    }
    if (current_.kind == TokenKind::kNull) {
      return PosError(current_.pos,
                      "null is not allowed as map value in field \"" +
                          std::string(fd->name()) + "\"");
    }
    Message* entry = r->AddMessage(msg, fd);
    Status st = SetMapKey(entry, key_fd, key, pos);
    if (!st.ok()) return st;
    if (val_fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      if (current_.kind != TokenKind::kLBrace) {
        return PosError(current_.pos, "expected '{' for map message value");
      }
      Advance();
      st = DecodeFields(entry->GetReflection()->MutableMessage(entry, val_fd),
                        /*in_block=*/true);
      if (!st.ok()) return st;
    } else if (val_fd->cpp_type() == FieldDescriptor::CPPTYPE_ENUM) {
      st = SetEnum(entry, val_fd, /*repeated=*/false);
      if (!st.ok()) return st;
    } else {
      st = SetScalar(entry, val_fd);
      if (!st.ok()) return st;
    }
  }
  if (current_.kind != TokenKind::kRBrace) {
    return PosError(current_.pos,
                    std::string("expected '}', got ") +
                        TokenKindName(current_.kind));
  }
  Advance();
  return Status::OK();
}

Status DirectDecoder::SetMapKey(Message* entry, const FieldDescriptor* key_fd,
                                std::string_view key, Position pos) {
  const Reflection* r = entry->GetReflection();
  switch (key_fd->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING:
      r->SetString(entry, key_fd, std::string(key));
      return Status::OK();
    case FieldDescriptor::CPPTYPE_INT32: {
      int32_t n;
      if (!ParseInteger(key, n))
        return PosError(pos, "invalid int32 map key: " + std::string(key));
      r->SetInt32(entry, key_fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_INT64: {
      int64_t n;
      if (!ParseInteger(key, n))
        return PosError(pos, "invalid int64 map key: " + std::string(key));
      r->SetInt64(entry, key_fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT32: {
      uint32_t n;
      if (!ParseInteger(key, n))
        return PosError(pos, "invalid uint32 map key: " + std::string(key));
      r->SetUInt32(entry, key_fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT64: {
      uint64_t n;
      if (!ParseInteger(key, n))
        return PosError(pos, "invalid uint64 map key: " + std::string(key));
      r->SetUInt64(entry, key_fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_BOOL:
      r->SetBool(entry, key_fd, key == "true");
      return Status::OK();
    default:
      return PosError(pos, "unsupported map key kind");
  }
}

// --- scalars ---------------------------------------------------------------

Status DirectDecoder::SetScalar(Message* msg, const FieldDescriptor* fd) {
  Position pos = current_.pos;
  const Reflection* r = msg->GetReflection();
  switch (fd->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING: {
      if (current_.kind == TokenKind::kString) {
        r->SetString(msg, fd, std::string(current_.value));
        Advance();
        return Status::OK();
      }
      if (current_.kind == TokenKind::kBytes) {
        auto decoded = detail::Base64DecodeStd(current_.value);
        if (!decoded.has_value())
          return PosError(pos, "invalid base64 in bytes literal");
        r->SetString(msg, fd,
                     std::string(decoded->begin(), decoded->end()));
        Advance();
        return Status::OK();
      }
      return PosError(pos, "expected string for field \"" +
                               std::string(fd->name()) + "\"");
    }
    case FieldDescriptor::CPPTYPE_BOOL: {
      if (current_.kind != TokenKind::kBool)
        return PosError(pos, "expected bool");
      r->SetBool(msg, fd, current_.value == "true");
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_INT32: {
      if (current_.kind != TokenKind::kInt)
        return PosError(pos, "expected integer for " + std::string(fd->name()));
      int32_t n;
      if (!ParseInteger(current_.value, n))
        return PosError(pos, "invalid int32: " + std::string(current_.value));
      r->SetInt32(msg, fd, n);
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_INT64: {
      if (current_.kind != TokenKind::kInt)
        return PosError(pos, "expected integer");
      int64_t n;
      if (!ParseInteger(current_.value, n))
        return PosError(pos, "invalid int64: " + std::string(current_.value));
      r->SetInt64(msg, fd, n);
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT32: {
      if (current_.kind != TokenKind::kInt)
        return PosError(pos, "expected integer");
      uint32_t n;
      if (!ParseInteger(current_.value, n))
        return PosError(pos, "invalid uint32: " + std::string(current_.value));
      r->SetUInt32(msg, fd, n);
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT64: {
      if (current_.kind != TokenKind::kInt)
        return PosError(pos, "expected integer");
      uint64_t n;
      if (!ParseInteger(current_.value, n))
        return PosError(pos, "invalid uint64: " + std::string(current_.value));
      r->SetUInt64(msg, fd, n);
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_FLOAT:
    case FieldDescriptor::CPPTYPE_DOUBLE: {
      if (current_.kind != TokenKind::kFloat &&
          current_.kind != TokenKind::kInt) {
        return PosError(pos, "expected number");
      }
      double d;
      if (!ParseDouble(current_.value, d))
        return PosError(pos, "invalid number: " + std::string(current_.value));
      if (fd->cpp_type() == FieldDescriptor::CPPTYPE_FLOAT) {
        r->SetFloat(msg, fd, static_cast<float>(d));
      } else {
        r->SetDouble(msg, fd, d);
      }
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_ENUM:
      return SetEnum(msg, fd, /*repeated=*/false);
    default:
      return PosError(pos, "unsupported scalar kind");
  }
}

Status DirectDecoder::AddRepeatedScalar(Message* msg,
                                        const FieldDescriptor* fd) {
  Position pos = current_.pos;
  const Reflection* r = msg->GetReflection();
  switch (fd->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING: {
      if (current_.kind == TokenKind::kString) {
        r->AddString(msg, fd, std::string(current_.value));
        Advance();
        return Status::OK();
      }
      if (current_.kind == TokenKind::kBytes) {
        auto decoded = detail::Base64DecodeStd(current_.value);
        if (!decoded.has_value())
          return PosError(pos, "invalid base64");
        r->AddString(msg, fd, std::string(decoded->begin(), decoded->end()));
        Advance();
        return Status::OK();
      }
      return PosError(pos, "expected string in repeated field");
    }
    case FieldDescriptor::CPPTYPE_BOOL:
      if (current_.kind != TokenKind::kBool)
        return PosError(pos, "expected bool in repeated field");
      r->AddBool(msg, fd, current_.value == "true");
      Advance();
      return Status::OK();
    case FieldDescriptor::CPPTYPE_INT32: {
      int32_t n;
      if (current_.kind != TokenKind::kInt || !ParseInteger(current_.value, n))
        return PosError(pos, "expected int32 in repeated field");
      r->AddInt32(msg, fd, n);
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_INT64: {
      int64_t n;
      if (current_.kind != TokenKind::kInt || !ParseInteger(current_.value, n))
        return PosError(pos, "expected int64 in repeated field");
      r->AddInt64(msg, fd, n);
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT32: {
      uint32_t n;
      if (current_.kind != TokenKind::kInt || !ParseInteger(current_.value, n))
        return PosError(pos, "expected uint32 in repeated field");
      r->AddUInt32(msg, fd, n);
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT64: {
      uint64_t n;
      if (current_.kind != TokenKind::kInt || !ParseInteger(current_.value, n))
        return PosError(pos, "expected uint64 in repeated field");
      r->AddUInt64(msg, fd, n);
      Advance();
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_FLOAT:
    case FieldDescriptor::CPPTYPE_DOUBLE: {
      if (current_.kind != TokenKind::kFloat &&
          current_.kind != TokenKind::kInt)
        return PosError(pos, "expected number in repeated field");
      double d;
      if (!ParseDouble(current_.value, d))
        return PosError(pos, "invalid number");
      if (fd->cpp_type() == FieldDescriptor::CPPTYPE_FLOAT) {
        r->AddFloat(msg, fd, static_cast<float>(d));
      } else {
        r->AddDouble(msg, fd, d);
      }
      Advance();
      return Status::OK();
    }
    default:
      return PosError(pos, "AddRepeatedScalar called for non-scalar kind");
  }
}

Status DirectDecoder::SetEnum(Message* msg, const FieldDescriptor* fd,
                              bool repeated) {
  Position pos = current_.pos;
  const Reflection* r = msg->GetReflection();
  const auto* enum_d = fd->enum_type();
  if (current_.kind == TokenKind::kIdent) {
    const auto* ev = enum_d->FindValueByName(std::string(current_.value));
    if (!ev) {
      return PosError(pos,
                      "unknown enum value \"" + std::string(current_.value) +
                          "\" for " + std::string(enum_d->full_name()));
    }
    if (repeated) r->AddEnumValue(msg, fd, ev->number());
    else r->SetEnumValue(msg, fd, ev->number());
    Advance();
    return Status::OK();
  }
  if (current_.kind == TokenKind::kInt) {
    int32_t n;
    if (!ParseInteger(current_.value, n)) {
      return PosError(pos, "invalid enum number");
    }
    if (repeated) r->AddEnumValue(msg, fd, n);
    else r->SetEnumValue(msg, fd, n);
    Advance();
    return Status::OK();
  }
  return PosError(pos, "expected enum name or number");
}

// --- skip helpers (for discard_unknown) ----------------------------------

void DirectDecoder::SkipValue() {
  switch (current_.kind) {
    case TokenKind::kLBrace:
      Advance();
      SkipBraced();
      return;
    case TokenKind::kLBracket:
      Advance();
      SkipBracketed();
      return;
    default:
      Advance();
      return;
  }
}

void DirectDecoder::SkipBraced() {
  int depth = 1;
  while (depth > 0 && current_.kind != TokenKind::kEOF) {
    if (current_.kind == TokenKind::kLBrace) ++depth;
    else if (current_.kind == TokenKind::kRBrace) --depth;
    Advance();
  }
}

void DirectDecoder::SkipBracketed() {
  int depth = 1;
  while (depth > 0 && current_.kind != TokenKind::kEOF) {
    if (current_.kind == TokenKind::kLBracket) ++depth;
    else if (current_.kind == TokenKind::kRBracket) --depth;
    Advance();
  }
}

// ApplyDefault parses `def` (the (pxf.default) string) and writes it into
// `msg`'s `fd` slot. Mirrors protowire-go applyDefault — handles every scalar
// kind plus enum and bytes. Message-typed defaults (well-known types) are
// reported as unsupported; add as needed when a user requires them.
Status ApplyDefault(Message* msg, const FieldDescriptor* fd,
                    std::string_view def) {
  const Reflection* r = msg->GetReflection();
  switch (fd->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING: {
      if (fd->type() == FieldDescriptor::TYPE_BYTES) {
        auto decoded = detail::Base64DecodeStd(def);
        if (!decoded.has_value()) {
          return Status::Error(
              "invalid default bytes for field \"" +
              std::string(fd->name()) + "\"");
        }
        r->SetString(msg, fd,
                     std::string(decoded->begin(), decoded->end()));
      } else {
        r->SetString(msg, fd, std::string(def));
      }
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_BOOL:
      r->SetBool(msg, fd, def == "true");
      return Status::OK();
    case FieldDescriptor::CPPTYPE_INT32: {
      int32_t n;
      if (!ParseInteger(def, n))
        return Status::Error("invalid default int32 \"" + std::string(def) +
                             "\" for field \"" + std::string(fd->name()) + "\"");
      r->SetInt32(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_INT64: {
      int64_t n;
      if (!ParseInteger(def, n))
        return Status::Error("invalid default int64 \"" + std::string(def) +
                             "\" for field \"" + std::string(fd->name()) + "\"");
      r->SetInt64(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT32: {
      uint32_t n;
      if (!ParseInteger(def, n))
        return Status::Error("invalid default uint32 \"" + std::string(def) +
                             "\" for field \"" + std::string(fd->name()) + "\"");
      r->SetUInt32(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_UINT64: {
      uint64_t n;
      if (!ParseInteger(def, n))
        return Status::Error("invalid default uint64 \"" + std::string(def) +
                             "\" for field \"" + std::string(fd->name()) + "\"");
      r->SetUInt64(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_FLOAT: {
      double d;
      if (!ParseDouble(def, d))
        return Status::Error("invalid default float \"" + std::string(def) +
                             "\" for field \"" + std::string(fd->name()) + "\"");
      r->SetFloat(msg, fd, static_cast<float>(d));
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_DOUBLE: {
      double d;
      if (!ParseDouble(def, d))
        return Status::Error("invalid default double \"" + std::string(def) +
                             "\" for field \"" + std::string(fd->name()) + "\"");
      r->SetDouble(msg, fd, d);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_ENUM: {
      const auto* enum_desc = fd->enum_type();
      if (const auto* ev = enum_desc->FindValueByName(std::string(def))) {
        r->SetEnumValue(msg, fd, ev->number());
        return Status::OK();
      }
      int32_t n;
      if (!ParseInteger(def, n))
        return Status::Error("invalid default enum \"" + std::string(def) +
                             "\" for field \"" + std::string(fd->name()) + "\"");
      r->SetEnumValue(msg, fd, n);
      return Status::OK();
    }
    case FieldDescriptor::CPPTYPE_MESSAGE:
      // Go applies well-known type defaults here (Timestamp, Duration,
      // wrappers). Wire as needed when a consumer requires it.
      return Status::Error(
          "default values for message fields not yet supported (field \"" +
          std::string(fd->name()) + "\")");
    default:
      return Status::Error("default values not supported for field \"" +
                           std::string(fd->name()) + "\"");
  }
}

// PostDecode validates required fields and applies defaults, recursing into
// nested messages that were present in the input. Mirrors postDecode in
// protowire-go/encoding/pxf/decode_fast.go.
Status PostDecode(Message* msg, const Result* result,
                  const FieldDescriptor* null_mask_fd, std::string prefix) {
  const auto* desc = msg->GetDescriptor();
  for (int i = 0; i < desc->field_count(); ++i) {
    const auto* fd = desc->field(i);
    if (null_mask_fd != nullptr && fd->number() == null_mask_fd->number()) {
      continue;
    }
    std::string path = prefix + std::string(fd->name());
    bool present = result->Has(path);
    if (!present) {
      if (IsRequired(fd)) {
        return Status::Error("required field \"" + path + "\" is absent");
      }
      if (auto def = GetDefault(fd); def.has_value()) {
        if (Status st = ApplyDefault(msg, fd, *def); !st.ok()) return st;
      }
      continue;
    }
    if (fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE && !fd->is_repeated() &&
        !fd->is_map() && !result->IsNull(path) &&
        msg->GetReflection()->HasField(*msg, fd)) {
      Message* sub = msg->GetReflection()->MutableMessage(msg, fd);
      if (Status st = PostDecode(sub, result, /*null_mask_fd=*/nullptr,
                                 path + ".");
          !st.ok()) {
        return st;
      }
    }
  }
  return Status::OK();
}

}  // namespace

// --- Public API ------------------------------------------------------------

Status Unmarshal(std::string_view data, Message* msg, UnmarshalOptions opts) {
  DirectDecoder d(data, msg, /*result=*/nullptr, opts.type_resolver,
                  opts.discard_unknown);
  return d.Run();
}

StatusOr<Result> UnmarshalFull(std::string_view data, Message* msg,
                               UnmarshalOptions opts) {
  Result r;
  DirectDecoder d(data, msg, &r, opts.type_resolver, opts.discard_unknown);
  Status st = d.Run();
  if (!st.ok()) return st;
  const auto* null_mask_fd = FindNullMaskField(msg->GetDescriptor());
  if (Status pst = PostDecode(msg, &r, null_mask_fd, ""); !pst.ok()) return pst;
  return r;
}

}  // namespace protowire::pxf

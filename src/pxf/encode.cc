// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// google::protobuf::Message → PXF text.
//
// Coverage matches the decoder: scalars, repeats, maps, nested messages,
// and the sugar of the well-known types, Any, the big numbers and pxf.Secret.
// Mirrors protowire-go's encode.go, which fixes the text byte for byte.

#include "protowire/pxf.h"
#include "protowire/pxf/annotations.h"
#include "protowire/pxf/keyed.h"
#include "protowire/pxf/result.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
#include "protowire/detail/go_quote.h"
#include "protowire/detail/rfc3339.h"
#include "protowire/pxf/wellknown.h"

namespace protowire::pxf {

namespace pb = google::protobuf;
using FieldDescriptor = pb::FieldDescriptor;
using Reflection = pb::Reflection;
using Message = pb::Message;

namespace {

class Encoder {
 public:
  Encoder(const MarshalOptions& opts, std::string& out) : opts_(opts), out_(out) {}

  // SetNullPaths names the fields written as `field = null`, by the path
  // UnmarshalFull records them under. Paths are only built once there is a
  // set to look them up in.
  void SetNullPaths(const std::vector<std::string>& paths) {
    null_set_.insert(paths.begin(), paths.end());
    track_nulls_ = true;
  }

  void SetNullMaskFd(const FieldDescriptor* fd) { null_mask_fd_ = fd; }

  Status EncodeMessage(const Message& msg, int level);

 private:
  void Indent(int level) {
    for (int i = 0; i < level; ++i) out_ += opts_.indent;
  }

  void WriteFieldPrefix(int level, std::string_view name) {
    Indent(level);
    out_.append(name.data(), name.size());
    out_ += " = ";
  }

  void WriteBlockOpen(int level, std::string_view name) {
    Indent(level);
    out_.append(name.data(), name.size());
    out_ += " {\n";
  }

  // Enter extends the null-lookup path by `name<suffix>.`, as the decoder
  // extends its presence path, so that a null recorded inside a body is
  // found where it was recorded. It returns the path to restore.
  std::string Enter(std::string_view name, const std::string& suffix = "") {
    std::string saved = path_prefix_;
    if (track_nulls_) {
      path_prefix_.append(name.data(), name.size());
      path_prefix_ += suffix;
      path_prefix_ += '.';
    }
    return saved;
  }

  Status EncodeField(const Message& msg, const FieldDescriptor* fd, int level);
  Status EncodeListField(const Message& msg, const FieldDescriptor* fd, int level);
  Status EncodeKeyedList(const Message& msg,
                         const FieldDescriptor* fd,
                         const FieldDescriptor* key_fd,
                         int level);
  Status EncodeMapField(const Message& msg, const FieldDescriptor* fd, int level);
  Status EncodeMessageField(const FieldDescriptor* fd, const Message& sub, int level);
  // Writes `name { @type = ... }`. *encoded stays false when the Any has no
  // body this port can show, and the caller writes its plain fields.
  Status TryEncodeAny(const FieldDescriptor* fd, const Message& any, int level, bool* encoded);
  Status WriteLiteralMessage(const Message& sub);

  // For repeated fields: index into element idx; for non-repeated: idx=-1.
  void EncodeScalarValue(const Message& msg, const FieldDescriptor* fd, int idx);

  void EmitString(std::string_view s);
  void EmitBytes(std::string_view raw);
  void EmitFloat(double v, bool is_float);

  const MarshalOptions& opts_;
  std::string& out_;
  std::unordered_set<std::string> null_set_;
  bool track_nulls_ = false;
  std::string path_prefix_;
  const FieldDescriptor* null_mask_fd_ = nullptr;
  // Key field to omit from the NEXT EncodeMessage call: the entry name of
  // a keyed-block entry already carries its value (draft -01 §3.13).
  const FieldDescriptor* skip_key_fd_ = nullptr;
};

// SecretHasMetadata reports a non-empty hint or fingerprint on a pxf.Secret.
bool SecretHasMetadata(const Message& secret) {
  const google::protobuf::Descriptor* d = secret.GetDescriptor();
  const Reflection* r = secret.GetReflection();
  for (const char* name : {"hint", "fingerprint"}) {
    const FieldDescriptor* fd = d->FindFieldByName(name);
    if (fd != nullptr && fd->type() == FieldDescriptor::TYPE_STRING && !fd->is_repeated() &&
        !r->GetString(secret, fd).empty()) {
      return true;
    }
  }
  return false;
}

// Nullable is whether null can bind to fd (the decoder's CheckNullable). A
// field the set names that cannot is written as if it were not named.
bool Nullable(const FieldDescriptor* fd) {
  return fd->has_presence() && !fd->is_repeated() && !fd->is_required();
}

// IsLiteralMessage reports whether sub is written as one literal. A Secret
// is only without metadata, which the literal would drop.
bool IsLiteralMessage(const Message& sub) {
  const auto* d = sub.GetDescriptor();
  if (IsSecret(d)) return !SecretHasMetadata(sub);
  return IsTimestamp(d) || IsDuration(d) || WrapperInnerCppType(d) != -1 || IsBigInt(d) ||
         IsDecimal(d) || IsBigFloat(d);
}

// MapKeyText is how a key is written and what entries sort by: bare where
// identifier-safe, and quoted as strconv.Quote does otherwise.
std::string MapKeyText(const Message& entry, const FieldDescriptor* key_fd) {
  const Reflection* r = entry.GetReflection();
  switch (key_fd->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING: {
      std::string scratch;
      const std::string& k = r->GetStringReference(entry, key_fd, &scratch);
      return IsIdentifierSafeEntryName(k) ? k : detail::GoQuote(k);
    }
    case FieldDescriptor::CPPTYPE_INT32:
      return std::to_string(r->GetInt32(entry, key_fd));
    case FieldDescriptor::CPPTYPE_INT64:
      return std::to_string(r->GetInt64(entry, key_fd));
    case FieldDescriptor::CPPTYPE_UINT32:
      return std::to_string(r->GetUInt32(entry, key_fd));
    case FieldDescriptor::CPPTYPE_UINT64:
      return std::to_string(r->GetUInt64(entry, key_fd));
    default:
      return r->GetBool(entry, key_fd) ? "true" : "false";
  }
}

// MapKeyPath is a key as it appears in a path, `by_key["k"]`.
std::string MapKeyPath(const Message& entry, const FieldDescriptor* key_fd) {
  const Reflection* r = entry.GetReflection();
  if (key_fd->cpp_type() == FieldDescriptor::CPPTYPE_STRING) {
    return "[" + detail::GoQuote(r->GetString(entry, key_fd)) + "]";
  }
  return "[" + MapKeyText(entry, key_fd) + "]";
}

// KeyedFormEligible reports whether every element of the repeated field
// has a non-empty key value that is distinct within the collection — the
// draft -01 §3.13 condition for emitting the keyed block form.
bool KeyedFormEligible(const Message& msg,
                       const FieldDescriptor* fd,
                       const FieldDescriptor* key_fd) {
  const Reflection* r = msg.GetReflection();
  int n = r->FieldSize(msg, fd);
  std::unordered_set<std::string> seen;
  seen.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const Message& sub = r->GetRepeatedMessage(msg, fd, i);
    std::string scratch;
    const std::string& key = sub.GetReflection()->GetStringReference(sub, key_fd, &scratch);
    if (key.empty() || !seen.insert(key).second) return false;
  }
  return true;
}

void Encoder::EmitString(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  out_ += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out_ += "\\\"";
        break;
      case '\\':
        out_ += "\\\\";
        break;
      case '\n':
        out_ += "\\n";
        break;
      case '\t':
        out_ += "\\t";
        break;
      case '\r':
        out_ += "\\r";
        break;
      default:
        if (c < 0x20) {
          out_ += "\\x";
          out_ += kHex[c >> 4];
          out_ += kHex[c & 0xF];
        } else {
          out_ += static_cast<char>(c);
        }
        break;
    }
  }
  out_ += '"';
}

void Encoder::EmitBytes(std::string_view raw) {
  out_ += "b\"";
  out_ += detail::Base64EncodeStd(raw);
  out_ += '"';
}

// EmitFloat writes what strconv.AppendFloat(v, 'g', -1, bits) does: the
// shortest digits that read back as v, positional for exponents from -4 to 5
// and otherwise d.ddde±XX, with at least two exponent digits.
void Encoder::EmitFloat(double v, bool is_float) {
  if (std::isinf(v)) {
    out_ += v > 0 ? "inf" : "-inf";
    return;
  }
  if (std::isnan(v)) {
    out_ += "nan";
    return;
  }
  char buf[64];
  auto r = is_float
               ? std::to_chars(
                     buf, buf + sizeof(buf), static_cast<float>(v), std::chars_format::scientific)
               : std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
  std::string_view sci(buf, static_cast<size_t>(r.ptr - buf));
  if (sci.front() == '-') {
    out_ += '-';
    sci.remove_prefix(1);
  }
  const size_t e = sci.find('e');
  std::string digits(sci.substr(0, e));
  digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
  int exp = 0;
  std::from_chars(sci.data() + e + (sci[e + 1] == '+' ? 2 : 1), sci.data() + sci.size(), exp);
  if (exp < -4 || exp >= 6) {
    out_ += sci;
    return;
  }
  if (exp < 0) {
    out_ += "0.";
    out_.append(static_cast<size_t>(-exp - 1), '0');
    out_ += digits;
  } else if (static_cast<size_t>(exp) + 1 >= digits.size()) {
    out_ += digits;
    out_.append(static_cast<size_t>(exp) + 1 - digits.size(), '0');
  } else {
    out_.append(digits, 0, static_cast<size_t>(exp) + 1);
    out_ += '.';
    out_.append(digits, static_cast<size_t>(exp) + 1);
  }
}

void Encoder::EncodeScalarValue(const Message& msg, const FieldDescriptor* fd, int idx) {
  const Reflection* r = msg.GetReflection();
  bool repeated = idx >= 0;
  switch (fd->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING: {
      std::string scratch;
      const std::string& v = repeated ? r->GetRepeatedStringReference(msg, fd, idx, &scratch)
                                      : r->GetStringReference(msg, fd, &scratch);
      if (fd->type() == FieldDescriptor::TYPE_BYTES) {
        EmitBytes(v);
      } else {
        EmitString(v);
      }
      break;
    }
    case FieldDescriptor::CPPTYPE_BOOL:
      out_ +=
          (repeated ? r->GetRepeatedBool(msg, fd, idx) : r->GetBool(msg, fd)) ? "true" : "false";
      break;
    case FieldDescriptor::CPPTYPE_INT32:
      out_ += std::to_string(repeated ? r->GetRepeatedInt32(msg, fd, idx) : r->GetInt32(msg, fd));
      break;
    case FieldDescriptor::CPPTYPE_INT64:
      out_ += std::to_string(repeated ? r->GetRepeatedInt64(msg, fd, idx) : r->GetInt64(msg, fd));
      break;
    case FieldDescriptor::CPPTYPE_UINT32:
      out_ += std::to_string(repeated ? r->GetRepeatedUInt32(msg, fd, idx) : r->GetUInt32(msg, fd));
      break;
    case FieldDescriptor::CPPTYPE_UINT64:
      out_ += std::to_string(repeated ? r->GetRepeatedUInt64(msg, fd, idx) : r->GetUInt64(msg, fd));
      break;
    case FieldDescriptor::CPPTYPE_FLOAT:
      EmitFloat(repeated ? r->GetRepeatedFloat(msg, fd, idx) : r->GetFloat(msg, fd), true);
      break;
    case FieldDescriptor::CPPTYPE_DOUBLE:
      EmitFloat(repeated ? r->GetRepeatedDouble(msg, fd, idx) : r->GetDouble(msg, fd), false);
      break;
    case FieldDescriptor::CPPTYPE_ENUM: {
      // An open enum can hold a number with no name.
      const int n = repeated ? r->GetRepeatedEnumValue(msg, fd, idx) : r->GetEnumValue(msg, fd);
      const auto* ev = fd->enum_type()->FindValueByNumber(n);
      if (ev != nullptr) {
        out_.append(ev->name().data(), ev->name().size());
      } else {
        out_ += std::to_string(n);
      }
      break;
    }
    case FieldDescriptor::CPPTYPE_MESSAGE:
      // Caller must route through EncodeMessageValue.
      break;
  }
}

Status Encoder::WriteLiteralMessage(const Message& sub) {
  const auto* d = sub.GetDescriptor();
  if (IsTimestamp(d)) {
    int64_t s;
    int32_t n;
    ReadTimestampFields(sub, &s, &n);
    out_ += detail::FormatRFC3339Nano(s, n);
    return Status::OK();
  }
  if (IsDuration(d)) {
    int64_t s;
    int32_t n;
    ReadDurationFields(sub, &s, &n);
    out_ += detail::FormatDuration(s, n);
    return Status::OK();
  }
  if (WrapperInnerCppType(d) != -1 || IsSecret(d)) {
    EncodeScalarValue(sub, d->FindFieldByName("value"), /*idx=*/-1);
    return Status::OK();
  }
  auto literal = IsBigInt(d)    ? ReadBigIntAsString(sub)
                 : IsDecimal(d) ? ReadDecimalAsString(sub)
                                : ReadBigFloatAsString(sub);
  if (!literal.ok()) return literal.status();
  out_ += *literal;
  return Status::OK();
}

Status Encoder::TryEncodeAny(const FieldDescriptor* fd,
                             const Message& any,
                             int level,
                             bool* encoded) {
  const auto* d = any.GetDescriptor();
  const std::string type_url = any.GetReflection()->GetString(any, d->FindFieldByName("type_url"));
  if (type_url.empty()) return Status::OK();
  const auto* inner_desc = opts_.type_resolver->FindMessageByURL(type_url);
  if (inner_desc == nullptr) return Status::OK();
  pb::DynamicMessageFactory factory(inner_desc->file()->pool());
  std::unique_ptr<Message> inner(factory.GetPrototype(inner_desc)->New());
  if (!inner->ParseFromString(any.GetReflection()->GetString(any, d->FindFieldByName("value")))) {
    return Status::OK();
  }
  WriteBlockOpen(level, fd->name());
  Indent(level + 1);
  out_ += "@type = ";
  EmitString(type_url);
  out_ += "\n";
  std::string saved = Enter(fd->name());
  Status st = EncodeMessage(*inner, level + 1);
  path_prefix_ = std::move(saved);
  if (!st.ok()) return st;
  Indent(level);
  out_ += "}\n";
  *encoded = true;
  return Status::OK();
}

Status Encoder::EncodeMessageField(const FieldDescriptor* fd, const Message& sub, int level) {
  if (IsLiteralMessage(sub)) {
    WriteFieldPrefix(level, fd->name());
    if (Status st = WriteLiteralMessage(sub); !st.ok()) return st;
    out_ += "\n";
    return Status::OK();
  }
  if (IsAny(fd->message_type()) && opts_.type_resolver != nullptr) {
    bool encoded = false;
    if (Status st = TryEncodeAny(fd, sub, level, &encoded); !st.ok()) return st;
    if (encoded) return Status::OK();
  }
  WriteBlockOpen(level, fd->name());
  std::string saved = Enter(fd->name());
  Status st = EncodeMessage(sub, level + 1);
  path_prefix_ = std::move(saved);
  if (!st.ok()) return st;
  Indent(level);
  out_ += "}\n";
  return Status::OK();
}

// EncodeKeyedList emits a keyed repeated field in the keyed block form:
// one named block per element, in list order. Entry names are written
// unquoted when identifier-safe and quoted otherwise; the key field is
// not additionally emitted inside the entry's block.
Status Encoder::EncodeKeyedList(const Message& msg,
                                const FieldDescriptor* fd,
                                const FieldDescriptor* key_fd,
                                int level) {
  const Reflection* r = msg.GetReflection();
  int n = r->FieldSize(msg, fd);
  WriteBlockOpen(level, fd->name());
  for (int i = 0; i < n; ++i) {
    const Message& sub = r->GetRepeatedMessage(msg, fd, i);
    std::string scratch;
    const std::string& key = sub.GetReflection()->GetStringReference(sub, key_fd, &scratch);
    Indent(level + 1);
    if (IsIdentifierSafeEntryName(key)) {
      out_ += key;
    } else {
      EmitString(key);
    }
    out_ += " {\n";
    skip_key_fd_ = key_fd;
    std::string saved = Enter(fd->name(), "[" + std::to_string(i) + "]");
    Status st = EncodeMessage(sub, level + 2);
    path_prefix_ = std::move(saved);
    if (!st.ok()) return st;
    Indent(level + 1);
    out_ += "}\n";
  }
  Indent(level);
  out_ += "}\n";
  return Status::OK();
}

Status Encoder::EncodeListField(const Message& msg, const FieldDescriptor* fd, int level) {
  const Reflection* r = msg.GetReflection();
  int n = r->FieldSize(msg, fd);
  // Keyed repeated field (draft -01 §3.13): emit the keyed block form
  // whenever every element's key is present, non-empty and distinct;
  // otherwise the anonymous list form is the only representation.
  if (const FieldDescriptor* key_fd = KeyField(fd); key_fd != nullptr && n > 0) {
    if (KeyedFormEligible(msg, fd, key_fd)) return EncodeKeyedList(msg, fd, key_fd, level);
  }
  WriteFieldPrefix(level, fd->name());
  out_ += "[\n";
  for (int i = 0; i < n; ++i) {
    Indent(level + 1);
    if (fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      const Message& sub = r->GetRepeatedMessage(msg, fd, i);
      if (IsLiteralMessage(sub)) {
        if (Status st = WriteLiteralMessage(sub); !st.ok()) return st;
      } else {
        out_ += "{\n";
        std::string saved = Enter(fd->name(), "[" + std::to_string(i) + "]");
        Status st = EncodeMessage(sub, level + 2);
        path_prefix_ = std::move(saved);
        if (!st.ok()) return st;
        Indent(level + 1);
        out_ += '}';
      }
    } else {
      EncodeScalarValue(msg, fd, i);
    }
    if (i < n - 1) out_ += ',';
    out_ += '\n';
  }
  Indent(level);
  out_ += "]\n";
  return Status::OK();
}

Status Encoder::EncodeMapField(const Message& msg, const FieldDescriptor* fd, int level) {
  const Reflection* r = msg.GetReflection();
  int n = r->FieldSize(msg, fd);
  WriteFieldPrefix(level, fd->name());
  out_ += "{\n";
  const FieldDescriptor* key_fd = fd->message_type()->map_key();
  const FieldDescriptor* val_fd = fd->message_type()->map_value();

  // Entries sort by their written key, as text: 10 comes before 9.
  std::vector<std::pair<std::string, int>> entries;
  entries.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    entries.emplace_back(MapKeyText(r->GetRepeatedMessage(msg, fd, i), key_fd), i);
  }
  std::sort(entries.begin(), entries.end());

  for (const auto& [key, i] : entries) {
    const Message& entry = r->GetRepeatedMessage(msg, fd, i);
    const Reflection* er = entry.GetReflection();
    Indent(level + 1);
    out_ += key;
    out_ += ": ";
    if (val_fd->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
      EncodeScalarValue(entry, val_fd, /*idx=*/-1);
    } else if (const Message& sub = er->GetMessage(entry, val_fd); IsLiteralMessage(sub)) {
      if (Status st = WriteLiteralMessage(sub); !st.ok()) return st;
    } else {
      out_ += "{\n";
      std::string saved = Enter(fd->name(), MapKeyPath(entry, key_fd));
      Status st = EncodeMessage(sub, level + 2);
      path_prefix_ = std::move(saved);
      if (!st.ok()) return st;
      Indent(level + 1);
      out_ += '}';
    }
    out_ += '\n';
  }
  Indent(level);
  out_ += "}\n";
  return Status::OK();
}

Status Encoder::EncodeField(const Message& msg, const FieldDescriptor* fd, int level) {
  const Reflection* r = msg.GetReflection();
  if (fd->is_repeated()) {  // true of a map field too
    if (r->FieldSize(msg, fd) == 0 && !opts_.emit_defaults) return Status::OK();
    return fd->is_map() ? EncodeMapField(msg, fd, level) : EncodeListField(msg, fd, level);
  }
  if (fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
    // Unset stays unwritten under emit_defaults too: a message that holds its
    // own type would otherwise never end.
    if (!r->HasField(msg, fd)) return Status::OK();
    return EncodeMessageField(fd, r->GetMessage(msg, fd), level);
  }
  if (!opts_.emit_defaults && !r->HasField(msg, fd)) return Status::OK();
  WriteFieldPrefix(level, fd->name());
  EncodeScalarValue(msg, fd, /*idx=*/-1);
  out_ += "\n";
  return Status::OK();
}

Status Encoder::EncodeMessage(const Message& msg, int level) {
  const auto* desc = msg.GetDescriptor();
  // A pending skip_key_fd_ applies to exactly this body: the entry name of
  // a keyed-block entry already carries the key field's value, so the
  // field is not additionally emitted inside the block. Consume it so
  // nested messages don't inherit the skip.
  const FieldDescriptor* skip_key = skip_key_fd_;
  skip_key_fd_ = nullptr;
  for (int i = 0; i < desc->field_count(); ++i) {
    const FieldDescriptor* fd = desc->field(i);
    if (skip_key != nullptr && fd == skip_key) continue;
    // Level 0 is the root: a field of this number in a body is data.
    if (level == 0 && null_mask_fd_ != nullptr && fd->number() == null_mask_fd_->number()) continue;
    if (track_nulls_ && Nullable(fd) &&
        null_set_.count(path_prefix_ + std::string(fd->name())) > 0) {
      WriteFieldPrefix(level, fd->name());
      out_ += "null\n";
      continue;
    }
    if (Status st = EncodeField(msg, fd, level); !st.ok()) return st;
  }
  return Status::OK();
}

}  // namespace

StatusOr<std::string> Marshal(const Message& msg, MarshalOptions opts) {
  if (opts.indent.empty()) opts.indent = "  ";
  std::string out;
  if (!opts.type_url.empty()) {
    out += "@type ";
    out += opts.type_url;
    out += "\n\n";
  }
  Encoder enc(opts, out);
  const FieldDescriptor* null_mask = FindNullMaskField(msg.GetDescriptor());
  enc.SetNullMaskFd(null_mask);
  if (null_mask != nullptr && msg.GetReflection()->HasField(msg, null_mask)) {
    enc.SetNullPaths(ReadNullPaths(msg, null_mask));
  } else if (opts.null_fields != nullptr) {
    enc.SetNullPaths(opts.null_fields->NullFields());
  }
  Status st = enc.EncodeMessage(msg, 0);
  if (!st.ok()) return st;
  return out;
}

}  // namespace protowire::pxf

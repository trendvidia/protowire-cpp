// google::protobuf::Message → PXF text.
//
// Coverage matches the decoder: scalars, repeats, maps, nested messages,
// well-known Timestamp/Duration/Wrapper sugar. Defers Any sugar, big-number
// sugar, _null FieldMask emission.

#include "protowire/pxf.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include "protowire/detail/base64.h"
#include "protowire/detail/duration.h"
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
  Encoder(const MarshalOptions& opts, std::string& out)
      : opts_(opts), out_(out) {}

  // Pre-compute null-path lookup at the top level. Only top-level paths
  // currently emitted as `field = null`; nested null tracking would require
  // walking the path components against descriptors at each level.
  void SetNullPaths(std::vector<std::string> paths) {
    for (auto& p : paths) {
      if (p.find('.') == std::string::npos) {
        top_level_nulls_.insert(std::move(p));
      } else {
        nested_nulls_.insert(std::move(p));
      }
    }
  }

  void SetNullMaskFd(const google::protobuf::FieldDescriptor* fd) {
    null_mask_fd_ = fd;
  }

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

  Status EncodeField(const Message& msg, const FieldDescriptor* fd, int level);
  Status EncodeListField(const Message& msg, const FieldDescriptor* fd,
                         int level);
  Status EncodeMapField(const Message& msg, const FieldDescriptor* fd,
                        int level);
  Status EncodeMessageValue(const Message& sub, int level);

  void EncodeScalar(const Message& msg, const FieldDescriptor* fd, int idx);
  // For repeated fields: index into element idx; for non-repeated: idx=-1.
  void EncodeScalarValue(const Message& msg, const FieldDescriptor* fd,
                         int idx);

  void EmitString(std::string_view s);
  void EmitBytes(std::string_view raw);

  const MarshalOptions& opts_;
  std::string& out_;
  std::unordered_set<std::string> top_level_nulls_;
  std::unordered_set<std::string> nested_nulls_;
  const google::protobuf::FieldDescriptor* null_mask_fd_ = nullptr;
  bool at_top_level_ = true;
};

void Encoder::EmitString(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  out_ += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out_ += "\\\""; break;
      case '\\': out_ += "\\\\"; break;
      case '\n': out_ += "\\n"; break;
      case '\t': out_ += "\\t"; break;
      case '\r': out_ += "\\r"; break;
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

void Encoder::EncodeScalarValue(const Message& msg, const FieldDescriptor* fd,
                                int idx) {
  const Reflection* r = msg.GetReflection();
  bool repeated = idx >= 0;
  switch (fd->cpp_type()) {
    case FieldDescriptor::CPPTYPE_STRING: {
      std::string scratch;
      const std::string& v = repeated
          ? r->GetRepeatedStringReference(msg, fd, idx, &scratch)
          : r->GetStringReference(msg, fd, &scratch);
      if (fd->type() == FieldDescriptor::TYPE_BYTES) {
        EmitBytes(v);
      } else {
        EmitString(v);
      }
      break;
    }
    case FieldDescriptor::CPPTYPE_BOOL:
      out_ += (repeated ? r->GetRepeatedBool(msg, fd, idx) : r->GetBool(msg, fd))
                  ? "true"
                  : "false";
      break;
    case FieldDescriptor::CPPTYPE_INT32:
      out_ += std::to_string(repeated ? r->GetRepeatedInt32(msg, fd, idx)
                                      : r->GetInt32(msg, fd));
      break;
    case FieldDescriptor::CPPTYPE_INT64:
      out_ += std::to_string(repeated ? r->GetRepeatedInt64(msg, fd, idx)
                                      : r->GetInt64(msg, fd));
      break;
    case FieldDescriptor::CPPTYPE_UINT32:
      out_ += std::to_string(repeated ? r->GetRepeatedUInt32(msg, fd, idx)
                                      : r->GetUInt32(msg, fd));
      break;
    case FieldDescriptor::CPPTYPE_UINT64:
      out_ += std::to_string(repeated ? r->GetRepeatedUInt64(msg, fd, idx)
                                      : r->GetUInt64(msg, fd));
      break;
    case FieldDescriptor::CPPTYPE_FLOAT: {
      // %.9g: round-trip-safe minimum for IEEE 754 binary32. fewer digits
      // would silently drop precision, which the comprehensive
      // PXF↔binary↔PXF tests caught.
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.9g",
                    static_cast<double>(repeated ? r->GetRepeatedFloat(msg, fd, idx)
                                                 : r->GetFloat(msg, fd)));
      out_ += buf;
      break;
    }
    case FieldDescriptor::CPPTYPE_DOUBLE: {
      // %.17g: round-trip-safe minimum for IEEE 754 binary64.
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.17g",
                    repeated ? r->GetRepeatedDouble(msg, fd, idx)
                             : r->GetDouble(msg, fd));
      out_ += buf;
      break;
    }
    case FieldDescriptor::CPPTYPE_ENUM: {
      const auto* ev = repeated ? r->GetRepeatedEnum(msg, fd, idx)
                                 : r->GetEnum(msg, fd);
      auto name_sv = ev->name();
      out_.append(name_sv.data(), name_sv.size());
      break;
    }
    case FieldDescriptor::CPPTYPE_MESSAGE:
      // Caller must route through EncodeMessageValue.
      break;
  }
}

Status Encoder::EncodeMessageValue(const Message& sub, int level) {
  const auto* d = sub.GetDescriptor();
  // google.protobuf.Any sugar: emit `{ @type = "..." inner_fields }`
  // when the resolver can find the inner type.
  if (IsAny(d) && opts_.type_resolver != nullptr) {
    std::string type_url, value;
    {
      std::string scratch;
      type_url = std::string(sub.GetReflection()->GetStringReference(
          sub, d->FindFieldByName("type_url"), &scratch));
      value = std::string(sub.GetReflection()->GetStringReference(
          sub, d->FindFieldByName("value"), &scratch));
    }
    if (!type_url.empty()) {
      const auto* inner_desc =
          opts_.type_resolver->FindMessageByURL(type_url);
      if (inner_desc) {
        google::protobuf::DynamicMessageFactory factory(
            inner_desc->file()->pool());
        std::unique_ptr<Message> inner(
            factory.GetPrototype(inner_desc)->New());
        if (inner->ParseFromString(value)) {
          out_ += "{\n";
          Indent(level + 1);
          out_ += "@type = ";
          EmitString(type_url);
          out_ += "\n";
          // Encode inner fields at level+1 by delegating to a fresh encoder
          // run on the inner message.
          Status st = EncodeMessage(*inner, level + 1);
          if (!st.ok()) return st;
          Indent(level);
          out_ += "}";
          return Status::OK();
        }
      }
    }
    // Fall through to generic block emission if the resolver failed.
  }
  if (IsTimestamp(d)) {
    int64_t s; int32_t n;
    ReadTimestampFields(sub, &s, &n);
    out_ += detail::FormatRFC3339Nano(s, n);
    return Status::OK();
  }
  if (IsDuration(d)) {
    int64_t s; int32_t n;
    ReadDurationFields(sub, &s, &n);
    out_ += detail::FormatDuration(s, n);
    return Status::OK();
  }
  // Wrapper sugar — emit just the inner value.
  if (WrapperInnerCppType(d) != -1) {
    const FieldDescriptor* inner = d->FindFieldByName("value");
    EncodeScalarValue(sub, inner, /*idx=*/-1);
    return Status::OK();
  }
  if (IsBigInt(d)) {
    out_ += ReadBigIntAsString(sub);
    return Status::OK();
  }
  if (IsDecimal(d)) {
    out_ += ReadDecimalAsString(sub);
    return Status::OK();
  }
  if (IsBigFloat(d)) {
    out_ += ReadBigFloatAsString(sub);
    return Status::OK();
  }
  // Generic message — open block, recurse, close.
  out_ += "{\n";
  Status st = EncodeMessage(sub, level + 1);
  if (!st.ok()) return st;
  Indent(level);
  out_ += "}";
  return Status::OK();
}

Status Encoder::EncodeListField(const Message& msg, const FieldDescriptor* fd,
                                int level) {
  const Reflection* r = msg.GetReflection();
  int n = r->FieldSize(msg, fd);
  WriteFieldPrefix(level, fd->name());
  out_ += "[";
  if (fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
    out_ += "\n";
    for (int i = 0; i < n; ++i) {
      Indent(level + 1);
      const Message& sub = r->GetRepeatedMessage(msg, fd, i);
      Status st = EncodeMessageValue(sub, level + 1);
      if (!st.ok()) return st;
      out_ += "\n";
    }
    Indent(level);
    out_ += "]\n";
  } else {
    for (int i = 0; i < n; ++i) {
      if (i) out_ += ", ";
      EncodeScalarValue(msg, fd, i);
    }
    out_ += "]\n";
  }
  return Status::OK();
}

Status Encoder::EncodeMapField(const Message& msg, const FieldDescriptor* fd,
                               int level) {
  const Reflection* r = msg.GetReflection();
  int n = r->FieldSize(msg, fd);
  WriteFieldPrefix(level, fd->name());
  out_ += "{\n";
  const FieldDescriptor* key_fd = fd->message_type()->map_key();
  const FieldDescriptor* val_fd = fd->message_type()->map_value();

  // Collect entries to emit in sorted order for stable output.
  std::vector<int> indices(n);
  for (int i = 0; i < n; ++i) indices[i] = i;
  std::sort(indices.begin(), indices.end(), [&](int a, int b) {
    const Message& ea = r->GetRepeatedMessage(msg, fd, a);
    const Message& eb = r->GetRepeatedMessage(msg, fd, b);
    const Reflection* er = ea.GetReflection();
    switch (key_fd->cpp_type()) {
      case FieldDescriptor::CPPTYPE_STRING: {
        std::string s_a, s_b;
        return er->GetStringReference(ea, key_fd, &s_a) <
               er->GetStringReference(eb, key_fd, &s_b);
      }
      case FieldDescriptor::CPPTYPE_INT32:
        return er->GetInt32(ea, key_fd) < er->GetInt32(eb, key_fd);
      case FieldDescriptor::CPPTYPE_INT64:
        return er->GetInt64(ea, key_fd) < er->GetInt64(eb, key_fd);
      case FieldDescriptor::CPPTYPE_UINT32:
        return er->GetUInt32(ea, key_fd) < er->GetUInt32(eb, key_fd);
      case FieldDescriptor::CPPTYPE_UINT64:
        return er->GetUInt64(ea, key_fd) < er->GetUInt64(eb, key_fd);
      default:
        return false;
    }
  });

  for (int i : indices) {
    Indent(level + 1);
    const Message& entry = r->GetRepeatedMessage(msg, fd, i);
    const Reflection* er = entry.GetReflection();
    // Key.
    switch (key_fd->cpp_type()) {
      case FieldDescriptor::CPPTYPE_STRING: {
        std::string scratch;
        const std::string& k = er->GetStringReference(entry, key_fd, &scratch);
        bool simple_ident = !k.empty();
        for (char c : k) {
          if (!(c == '_' || (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            simple_ident = false;
            break;
          }
        }
        if (simple_ident) {
          out_ += k;
        } else {
          EmitString(k);
        }
        break;
      }
      case FieldDescriptor::CPPTYPE_INT32:
        out_ += std::to_string(er->GetInt32(entry, key_fd));
        break;
      case FieldDescriptor::CPPTYPE_INT64:
        out_ += std::to_string(er->GetInt64(entry, key_fd));
        break;
      case FieldDescriptor::CPPTYPE_UINT32:
        out_ += std::to_string(er->GetUInt32(entry, key_fd));
        break;
      case FieldDescriptor::CPPTYPE_UINT64:
        out_ += std::to_string(er->GetUInt64(entry, key_fd));
        break;
      case FieldDescriptor::CPPTYPE_BOOL:
        out_ += er->GetBool(entry, key_fd) ? "true" : "false";
        break;
      default:
        return Status::Error("unsupported map key kind on encode");
    }
    out_ += ": ";
    if (val_fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      const Message& sub = er->GetMessage(entry, val_fd);
      Status st = EncodeMessageValue(sub, level + 1);
      if (!st.ok()) return st;
    } else {
      EncodeScalarValue(entry, val_fd, /*idx=*/-1);
    }
    out_ += "\n";
  }
  Indent(level);
  out_ += "}\n";
  return Status::OK();
}

Status Encoder::EncodeField(const Message& msg, const FieldDescriptor* fd,
                            int level) {
  const Reflection* r = msg.GetReflection();
  if (fd->is_map()) return EncodeMapField(msg, fd, level);
  if (fd->is_repeated()) {
    if (r->FieldSize(msg, fd) == 0 && !opts_.emit_defaults) return Status::OK();
    return EncodeListField(msg, fd, level);
  }
  if (!opts_.emit_defaults && !r->HasField(msg, fd) &&
      fd->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
    // Proto3 scalar zero — skip.
    bool is_zero = false;
    switch (fd->cpp_type()) {
      case FieldDescriptor::CPPTYPE_STRING: {
        std::string s;
        is_zero = r->GetStringReference(msg, fd, &s).empty();
        break;
      }
      case FieldDescriptor::CPPTYPE_BOOL: is_zero = !r->GetBool(msg, fd); break;
      case FieldDescriptor::CPPTYPE_INT32: is_zero = r->GetInt32(msg, fd) == 0; break;
      case FieldDescriptor::CPPTYPE_INT64: is_zero = r->GetInt64(msg, fd) == 0; break;
      case FieldDescriptor::CPPTYPE_UINT32: is_zero = r->GetUInt32(msg, fd) == 0; break;
      case FieldDescriptor::CPPTYPE_UINT64: is_zero = r->GetUInt64(msg, fd) == 0; break;
      case FieldDescriptor::CPPTYPE_FLOAT: is_zero = r->GetFloat(msg, fd) == 0; break;
      case FieldDescriptor::CPPTYPE_DOUBLE: is_zero = r->GetDouble(msg, fd) == 0; break;
      case FieldDescriptor::CPPTYPE_ENUM: is_zero = r->GetEnumValue(msg, fd) == 0; break;
      default: break;
    }
    if (is_zero) return Status::OK();
  }
  if (fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
    if (!opts_.emit_defaults && !r->HasField(msg, fd)) return Status::OK();
    WriteFieldPrefix(level, fd->name());
    const Message& sub = r->GetMessage(msg, fd);
    Status st = EncodeMessageValue(sub, level);
    if (!st.ok()) return st;
    out_ += "\n";
    return Status::OK();
  }
  WriteFieldPrefix(level, fd->name());
  EncodeScalarValue(msg, fd, /*idx=*/-1);
  out_ += "\n";
  return Status::OK();
}

Status Encoder::EncodeMessage(const Message& msg, int level) {
  const auto* desc = msg.GetDescriptor();
  bool was_top = at_top_level_;
  for (int i = 0; i < desc->field_count(); ++i) {
    const FieldDescriptor* fd = desc->field(i);
    if (was_top && null_mask_fd_ && fd == null_mask_fd_) {
      continue;  // skip the _null FieldMask itself
    }
    if (was_top && top_level_nulls_.count(std::string(fd->name())) > 0) {
      WriteFieldPrefix(level, fd->name());
      out_ += "null\n";
      continue;
    }
    at_top_level_ = false;
    Status st = EncodeField(msg, fd, level);
    at_top_level_ = was_top;
    if (!st.ok()) return st;
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
  if (auto* null_mask = FindNullMaskField(msg.GetDescriptor())) {
    enc.SetNullMaskFd(null_mask);
    enc.SetNullPaths(ReadNullPaths(msg, null_mask));
  }
  Status st = enc.EncodeMessage(msg, 0);
  if (!st.ok()) return st;
  return out;
}

}  // namespace protowire::pxf

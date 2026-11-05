// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// SBE marshal: proto::Message → SBE binary buffer.

#include "protowire/sbe.h"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "encoding_range.h"
#include "header_bounds.h"

namespace protowire::sbe {

namespace pb = google::protobuf;

namespace {

void WriteU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void WriteU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}
void WriteU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}

// WriteScalar writes a numeric field as its primitive. The value is read by
// the field's kind and, for an integer primitive, written as the low bytes of
// its two's complement once it is known to fit; a value the primitive cannot
// hold is refused rather than cut to its low bits (draft -01 § SBE
// Annotations, protowire-go#252).
Status WriteScalar(std::vector<uint8_t>& buf,
                   size_t offset,
                   const FieldTemplate& ft,
                   const pb::Message& msg) {
  while (buf.size() < offset + ft.size) buf.push_back(0);
  uint8_t* p = buf.data() + offset;
  const pb::Reflection* r = msg.GetReflection();
  const pb::FieldDescriptor* fd = ft.fd;
  const EncodingRange rg = RangeOf(ft);

  if (ft.encoding == kEncFloat || ft.encoding == kEncDouble) {
    const double f = fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_FLOAT
                         ? static_cast<double>(r->GetFloat(msg, fd))
                         : r->GetDouble(msg, fd);
    if (ft.encoding == kEncDouble) {
      uint64_t b = 0;
      std::memcpy(&b, &f, 8);
      WriteU64(p, b);
      return Status::OK();
    }
    if (rg.write_check && !FloatFits(f)) {
      return Status::Error("sbe: field " + std::string(fd->full_name()) + ": value " + GoG(f) +
                           " is outside the range of float, ±" +
                           GoG(std::numeric_limits<float>::max()));
    }
    const auto narrowed = static_cast<float>(f);
    uint32_t b = 0;
    std::memcpy(&b, &narrowed, 4);
    WriteU32(p, b);
    return Status::OK();
  }

  uint64_t v = 0;
  bool is_unsigned = false;
  switch (fd->cpp_type()) {
    case pb::FieldDescriptor::CPPTYPE_BOOL:
      v = r->GetBool(msg, fd) ? 1 : 0;
      is_unsigned = true;
      break;
    case pb::FieldDescriptor::CPPTYPE_ENUM:
      v = static_cast<uint64_t>(int64_t{r->GetEnumValue(msg, fd)});
      break;
    case pb::FieldDescriptor::CPPTYPE_UINT32:
      v = r->GetUInt32(msg, fd);
      is_unsigned = true;
      break;
    case pb::FieldDescriptor::CPPTYPE_UINT64:
      v = r->GetUInt64(msg, fd);
      is_unsigned = true;
      break;
    case pb::FieldDescriptor::CPPTYPE_INT32:
      v = static_cast<uint64_t>(int64_t{r->GetInt32(msg, fd)});
      break;
    default:  // CPPTYPE_INT64
      v = static_cast<uint64_t>(r->GetInt64(msg, fd));
      break;
  }
  if (rg.write_check && !InRange(v, is_unsigned, rg.enc_lo, rg.enc_hi)) {
    return Status::Error("sbe: field " + std::string(fd->full_name()) + ": value " +
                         IntString(v, is_unsigned) + " is outside the range of " + ft.encoding +
                         ", [" + std::to_string(rg.enc_lo) + ", " + std::to_string(rg.enc_hi) +
                         "]");
  }
  switch (ft.size) {
    case 1:
      p[0] = static_cast<uint8_t>(v);
      break;
    case 2:
      WriteU16(p, static_cast<uint16_t>(v));
      break;
    case 4:
      WriteU32(p, static_cast<uint32_t>(v));
      break;
    default:
      WriteU64(p, v);
      break;
  }
  return Status::OK();
}

// CutLength is how many bytes of s fit in a field of size bytes. A string is
// cut at the last whole UTF-8 character at or before the limit, since a
// byte-exact cut can leave half a character and a strict decoder then rejects
// the whole message; a bytes field is cut at the limit (draft -01 § SBE
// Annotations, protowire#495, protowire-go#246). The rest is NUL-padded.
size_t CutLength(const std::string& s, size_t size, bool is_string) {
  if (s.size() <= size) return s.size();
  size_t n = size;
  if (is_string) {
    // A continuation byte is 10xxxxxx; back up to the start of its character.
    while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
  }
  return n;
}

void WriteFixedString(std::vector<uint8_t>& buf,
                      size_t offset,
                      const FieldTemplate& ft,
                      const pb::Message& msg) {
  while (buf.size() < offset + ft.size) buf.push_back(0);
  std::string scratch;
  const std::string& s = msg.GetReflection()->GetStringReference(msg, ft.fd, &scratch);
  const size_t n = CutLength(s, ft.size, ft.fd->type() == pb::FieldDescriptor::TYPE_STRING);
  std::memcpy(buf.data() + offset, s.data(), n);
  for (size_t i = n; i < ft.size; ++i) buf[offset + i] = 0;
}

Status WriteFields(std::vector<uint8_t>& buf,
                   size_t base,
                   const std::vector<FieldTemplate>& fields,
                   const pb::Message& msg);

Status WriteCompositeField(std::vector<uint8_t>& buf,
                           size_t base,
                           const FieldTemplate& ft,
                           const pb::Message& msg) {
  const pb::Message& sub = msg.GetReflection()->GetMessage(msg, ft.fd);
  return WriteFields(buf, base + ft.offset, ft.composite, sub);
}

Status WriteFields(std::vector<uint8_t>& buf,
                   size_t base,
                   const std::vector<FieldTemplate>& fields,
                   const pb::Message& msg) {
  for (const FieldTemplate& ft : fields) {
    if (!ft.composite.empty()) {
      if (Status st = WriteCompositeField(buf, base, ft, msg); !st.ok()) return st;
      continue;
    }
    if (ft.encoding == kEncChar) {
      WriteFixedString(buf, base + ft.offset, ft, msg);
      continue;
    }
    if (Status st = WriteScalar(buf, base + ft.offset, ft, msg); !st.ok()) return st;
  }
  return Status::OK();
}

}  // namespace

StatusOr<std::vector<uint8_t>> Codec::Marshal(const pb::Message& msg) const {
  const auto* tmpl = TemplateByName(msg.GetDescriptor()->full_name());
  if (!tmpl) {
    return Status::Error("sbe: no template registered for " +
                         std::string(msg.GetDescriptor()->full_name()));
  }
  std::vector<uint8_t> buf;
  buf.resize(8 + tmpl->block_length, 0);
  WriteU16(buf.data() + 0, tmpl->block_length);
  WriteU16(buf.data() + 2, tmpl->template_id);
  WriteU16(buf.data() + 4, tmpl->schema_id);
  WriteU16(buf.data() + 6, tmpl->version);
  if (Status st = WriteFields(buf, 8, tmpl->fields, msg); !st.ok()) return st;

  // Repeating groups.
  for (const GroupTemplate& gt : tmpl->groups) {
    const pb::Reflection* r = msg.GetReflection();
    int n = r->FieldSize(msg, gt.fd);
    // numInGroup is a uint16: a larger count would be written as its low 16
    // bits, and a reader would take the entries past them for trailing bytes.
    if (n > kMaxUint16) {
      return Status::Error("sbe: field " + std::string(gt.fd->full_name()) + " has " +
                           std::to_string(n) + " entries, past " + std::to_string(kMaxUint16) +
                           ", the largest numInGroup SBE can state");
    }
    size_t header_off = buf.size();
    buf.resize(header_off + 4, 0);
    WriteU16(buf.data() + header_off + 0, gt.block_length);
    WriteU16(buf.data() + header_off + 2, static_cast<uint16_t>(n));
    for (int i = 0; i < n; ++i) {
      size_t entry_off = buf.size();
      buf.resize(entry_off + gt.block_length, 0);
      const pb::Message& entry = r->GetRepeatedMessage(msg, gt.fd, i);
      if (Status st = WriteFields(buf, entry_off, gt.fields, entry); !st.ok()) return st;
    }
  }
  return buf;
}

}  // namespace protowire::sbe

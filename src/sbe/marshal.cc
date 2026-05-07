// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// SBE marshal: proto::Message → SBE binary buffer.

#include "protowire/sbe.h"

#include <cstring>
#include <vector>

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

void EnsureAndWriteScalar(std::vector<uint8_t>& buf,
                          size_t offset,
                          const FieldTemplate& ft,
                          uint64_t v_bits) {
  while (buf.size() < offset + ft.size) buf.push_back(0);
  uint8_t* p = buf.data() + offset;
  switch (ft.size) {
    case 1:
      p[0] = static_cast<uint8_t>(v_bits);
      break;
    case 2:
      WriteU16(p, static_cast<uint16_t>(v_bits));
      break;
    case 4:
      WriteU32(p, static_cast<uint32_t>(v_bits));
      break;
    case 8:
      WriteU64(p, v_bits);
      break;
  }
}

uint64_t EncodeScalar(const pb::Message& msg, const FieldTemplate& ft) {
  const pb::Reflection* r = msg.GetReflection();
  const pb::FieldDescriptor* fd = ft.fd;
  switch (fd->cpp_type()) {
    case pb::FieldDescriptor::CPPTYPE_BOOL:
      return r->GetBool(msg, fd) ? 1 : 0;
    case pb::FieldDescriptor::CPPTYPE_INT32:
      return static_cast<uint64_t>(r->GetInt32(msg, fd));
    case pb::FieldDescriptor::CPPTYPE_INT64:
      return static_cast<uint64_t>(r->GetInt64(msg, fd));
    case pb::FieldDescriptor::CPPTYPE_UINT32:
      return r->GetUInt32(msg, fd);
    case pb::FieldDescriptor::CPPTYPE_UINT64:
      return r->GetUInt64(msg, fd);
    case pb::FieldDescriptor::CPPTYPE_FLOAT: {
      float f = r->GetFloat(msg, fd);
      uint32_t b;
      std::memcpy(&b, &f, 4);
      return b;
    }
    case pb::FieldDescriptor::CPPTYPE_DOUBLE: {
      double d = r->GetDouble(msg, fd);
      uint64_t b;
      std::memcpy(&b, &d, 8);
      return b;
    }
    case pb::FieldDescriptor::CPPTYPE_ENUM:
      return static_cast<uint64_t>(r->GetEnumValue(msg, fd));
    default:
      return 0;
  }
}

void WriteFixedString(std::vector<uint8_t>& buf,
                      size_t offset,
                      const FieldTemplate& ft,
                      const pb::Message& msg) {
  while (buf.size() < offset + ft.size) buf.push_back(0);
  std::string scratch;
  const std::string& s = msg.GetReflection()->GetStringReference(msg, ft.fd, &scratch);
  size_t n = std::min<size_t>(s.size(), ft.size);
  std::memcpy(buf.data() + offset, s.data(), n);
  for (size_t i = n; i < ft.size; ++i) buf[offset + i] = 0;
}

void WriteFields(std::vector<uint8_t>& buf,
                 size_t base,
                 const std::vector<FieldTemplate>& fields,
                 const pb::Message& msg);

void WriteCompositeField(std::vector<uint8_t>& buf,
                         size_t base,
                         const FieldTemplate& ft,
                         const pb::Message& msg) {
  const pb::Message& sub = msg.GetReflection()->GetMessage(msg, ft.fd);
  WriteFields(buf, base + ft.offset, ft.composite, sub);
}

void WriteFields(std::vector<uint8_t>& buf,
                 size_t base,
                 const std::vector<FieldTemplate>& fields,
                 const pb::Message& msg) {
  for (const FieldTemplate& ft : fields) {
    if (!ft.composite.empty()) {
      WriteCompositeField(buf, base, ft, msg);
      continue;
    }
    if (ft.encoding == kEncChar) {
      WriteFixedString(buf, base + ft.offset, ft, msg);
      continue;
    }
    EnsureAndWriteScalar(buf, base + ft.offset, ft, EncodeScalar(msg, ft));
  }
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
  WriteFields(buf, 8, tmpl->fields, msg);

  // Repeating groups.
  for (const GroupTemplate& gt : tmpl->groups) {
    const pb::Reflection* r = msg.GetReflection();
    int n = r->FieldSize(msg, gt.fd);
    size_t header_off = buf.size();
    buf.resize(header_off + 4, 0);
    WriteU16(buf.data() + header_off + 0, gt.block_length);
    WriteU16(buf.data() + header_off + 2, static_cast<uint16_t>(n));
    for (int i = 0; i < n; ++i) {
      size_t entry_off = buf.size();
      buf.resize(entry_off + gt.block_length, 0);
      const pb::Message& entry = r->GetRepeatedMessage(msg, gt.fd, i);
      WriteFields(buf, entry_off, gt.fields, entry);
    }
  }
  return buf;
}

}  // namespace protowire::sbe

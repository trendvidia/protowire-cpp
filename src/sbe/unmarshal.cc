// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// SBE unmarshal: SBE binary buffer → proto::Message.

#include "protowire/sbe.h"

#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "protowire/detail/utf8.h"

namespace protowire::sbe {

namespace pb = google::protobuf;

namespace {

uint16_t LoadU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t LoadU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (i * 8);
  return v;
}
uint64_t LoadU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
  return v;
}

uint64_t LoadScalar(const uint8_t* p, size_t size) {
  switch (size) {
    case 1:
      return *p;
    case 2:
      return LoadU16(p);
    case 4:
      return LoadU32(p);
    case 8:
      return LoadU64(p);
  }
  return 0;
}

Status DecodeFields(std::span<const uint8_t> block,
                    const std::vector<FieldTemplate>& fields,
                    pb::Message* msg);

Status DecodeScalarField(const uint8_t* p, const FieldTemplate& ft, pb::Message* msg) {
  const pb::Reflection* r = msg->GetReflection();
  const pb::FieldDescriptor* fd = ft.fd;
  if (ft.encoding == kEncChar) {
    // Char array — null-terminated within ft.size, or the whole region.
    size_t n = 0;
    while (n < ft.size && p[n] != 0) ++n;
    std::string_view raw(reinterpret_cast<const char*>(p), n);
    // HARDENING.md § UTF-8: a proto3 string field is valid UTF-8 whatever
    // the source encoding; a bytes field takes the raw region.
    if (fd->type() == pb::FieldDescriptor::TYPE_STRING && !detail::IsValidUTF8(raw)) {
      return Status::Error("sbe: invalid UTF-8 in string field " + std::string(fd->name()));
    }
    r->SetString(msg, fd, std::string(raw));
    return Status::OK();
  }
  uint64_t bits = LoadScalar(p, ft.size);
  switch (fd->cpp_type()) {
    case pb::FieldDescriptor::CPPTYPE_BOOL:
      r->SetBool(msg, fd, bits != 0);
      break;
    case pb::FieldDescriptor::CPPTYPE_INT32:
      r->SetInt32(msg, fd, static_cast<int32_t>(bits));
      break;
    case pb::FieldDescriptor::CPPTYPE_INT64:
      r->SetInt64(msg, fd, static_cast<int64_t>(bits));
      break;
    case pb::FieldDescriptor::CPPTYPE_UINT32:
      r->SetUInt32(msg, fd, static_cast<uint32_t>(bits));
      break;
    case pb::FieldDescriptor::CPPTYPE_UINT64:
      r->SetUInt64(msg, fd, bits);
      break;
    case pb::FieldDescriptor::CPPTYPE_FLOAT: {
      uint32_t b = static_cast<uint32_t>(bits);
      float f;
      std::memcpy(&f, &b, 4);
      r->SetFloat(msg, fd, f);
      break;
    }
    case pb::FieldDescriptor::CPPTYPE_DOUBLE: {
      double d;
      std::memcpy(&d, &bits, 8);
      r->SetDouble(msg, fd, d);
      break;
    }
    case pb::FieldDescriptor::CPPTYPE_ENUM:
      r->SetEnumValue(msg, fd, static_cast<int>(bits));
      break;
    default:
      break;
  }
  return Status::OK();
}

Status DecodeFields(std::span<const uint8_t> block,
                    const std::vector<FieldTemplate>& fields,
                    pb::Message* msg) {
  for (const FieldTemplate& ft : fields) {
    if (!ft.composite.empty()) {
      pb::Message* sub = msg->GetReflection()->MutableMessage(msg, ft.fd);
      Status st = DecodeFields(block.subspan(ft.offset, ft.size), ft.composite, sub);
      if (!st.ok()) return st;
      continue;
    }
    Status st = DecodeScalarField(block.data() + ft.offset, ft, msg);
    if (!st.ok()) return st;
  }
  return Status::OK();
}

}  // namespace

Status Codec::Unmarshal(std::span<const uint8_t> data, pb::Message* msg) const {
  if (data.size() > static_cast<size_t>(max_message_size_)) {
    return Status::Error("sbe: input of " + std::to_string(data.size()) +
                         " bytes exceeds MaxMessageSize=" + std::to_string(max_message_size_));
  }
  const auto* tmpl = TemplateByName(msg->GetDescriptor()->full_name());
  if (!tmpl) {
    return Status::Error("sbe: no template registered for " +
                         std::string(msg->GetDescriptor()->full_name()));
  }
  if (data.size() < 8) return Status::Error("sbe: buffer too short for header");
  // HARDENING.md § SBE validation, steps 1 and 2: the root block fits, and
  // the wire block is at least the template's — schema evolution may make
  // it larger (fields appended), never smaller, since a smaller block
  // would put some field's offset + size past the wire block.
  uint16_t block_length = LoadU16(data.data());
  uint16_t template_id = LoadU16(data.data() + 2);
  if (template_id != tmpl->template_id) {
    return Status::Error("sbe: template ID mismatch: got " + std::to_string(template_id) +
                         ", want " + std::to_string(tmpl->template_id));
  }
  if (block_length < tmpl->block_length) {
    return Status::Error("sbe: wire blockLength " + std::to_string(block_length) +
                         " < schema blockLength " + std::to_string(tmpl->block_length) +
                         " for template " + std::to_string(tmpl->template_id));
  }
  if (data.size() < 8u + block_length) {
    return Status::Error("sbe: data too short for root block");
  }
  // Steps 3 and 4 for every group, before any entry is allocated.
  if (Status st = ValidateGroups(data, *tmpl, 8u + block_length); !st.ok()) return st;

  if (Status st = DecodeFields(data.subspan(8, block_length), tmpl->fields, msg); !st.ok()) {
    return st;
  }

  size_t pos = 8 + block_length;
  for (const GroupTemplate& gt : tmpl->groups) {
    uint16_t entry_block = LoadU16(data.data() + pos);
    uint16_t count = LoadU16(data.data() + pos + 2);
    pos += 4;
    const pb::Reflection* r = msg->GetReflection();
    for (uint16_t i = 0; i < count; ++i) {
      pb::Message* entry = r->AddMessage(msg, gt.fd);
      Status st = DecodeFields(data.subspan(pos, entry_block), gt.fields, entry);
      if (!st.ok()) return st;
      pos += entry_block;
    }
  }
  return Status::OK();
}

}  // namespace protowire::sbe

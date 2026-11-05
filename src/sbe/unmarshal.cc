// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// SBE unmarshal: SBE binary buffer → proto::Message.

#include "protowire/sbe.h"

#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <google/protobuf/dynamic_message.h>

#include "encoding_range.h"
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
    // A string drops its trailing zero padding, and a zero inside it stays. A
    // bytes field takes the whole region.
    size_t n = ft.size;
    if (fd->type() == pb::FieldDescriptor::TYPE_STRING) {
      while (n > 0 && p[n - 1] == 0) --n;
    }
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
  const auto cpp = fd->cpp_type();
  const EncodingRange rg = RangeOf(ft);

  if (ft.encoding == kEncFloat || ft.encoding == kEncDouble) {
    double f = 0;
    if (ft.encoding == kEncFloat) {
      const auto b = static_cast<uint32_t>(bits);
      float narrow = 0;
      std::memcpy(&narrow, &b, 4);
      f = narrow;
    } else {
      std::memcpy(&f, &bits, 8);
      if (rg.read_check && !FloatFits(f)) {
        return Status::Error("sbe: field " + std::string(fd->full_name()) + ": wire value " +
                             GoG(f) + " does not fit a float field, ±" +
                             GoG(std::numeric_limits<float>::max()));
      }
    }
    if (cpp == pb::FieldDescriptor::CPPTYPE_FLOAT) {
      r->SetFloat(msg, fd, static_cast<float>(f));
    } else {
      r->SetDouble(msg, fd, f);
    }
    return Status::OK();
  }

  // An integer primitive, sign- or zero-extended per the primitive (draft -01
  // § SBE Annotations), then stored by the field's kind. A value the field
  // cannot hold is refused rather than cut to its low bits (protowire-go#252).
  if (rg.enc_signed) {
    switch (ft.size) {
      case 1:
        bits = static_cast<uint64_t>(int64_t{static_cast<int8_t>(bits)});
        break;
      case 2:
        bits = static_cast<uint64_t>(int64_t{static_cast<int16_t>(bits)});
        break;
      case 4:
        bits = static_cast<uint64_t>(int64_t{static_cast<int32_t>(bits)});
        break;
      default:
        break;
    }
  }
  if (rg.read_check && !InRange(bits, !rg.enc_signed, rg.field_lo, rg.field_hi)) {
    return Status::Error("sbe: field " + std::string(fd->full_name()) + ": wire value " +
                         IntString(bits, !rg.enc_signed) + " (" + ft.encoding +
                         ") does not fit a " + std::string(fd->type_name()) + " field, [" +
                         std::to_string(rg.field_lo) + ", " + std::to_string(rg.field_hi) + "]");
  }
  switch (cpp) {
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
    case pb::FieldDescriptor::CPPTYPE_ENUM:
      r->SetEnumValue(msg, fd, static_cast<int32_t>(bits));
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
  return UnmarshalInto(data, *tmpl, msg);
}

StatusOr<std::shared_ptr<pb::Message>> Codec::UnmarshalDescriptor(
    std::span<const uint8_t> data, const pb::Descriptor* desc) const {
  if (data.size() > static_cast<size_t>(max_message_size_)) {
    return Status::Error("sbe: input of " + std::to_string(data.size()) +
                         " bytes exceeds MaxMessageSize=" + std::to_string(max_message_size_));
  }
  const auto* tmpl = TemplateByName(desc->full_name());
  if (!tmpl) {
    return Status::Error("sbe: no template registered for " + std::string(desc->full_name()));
  }
  struct Owned {
    pb::DynamicMessageFactory factory;
    std::unique_ptr<pb::Message> message;
  };
  auto owned = std::make_shared<Owned>();
  owned->message.reset(owned->factory.GetPrototype(desc)->New());
  pb::Message* msg = owned->message.get();
  if (Status st = UnmarshalInto(data, *tmpl, msg); !st.ok()) return st;
  return std::shared_ptr<pb::Message>(owned, msg);
}

Status Codec::UnmarshalInto(std::span<const uint8_t> data,
                            const MessageTemplate& message_template,
                            pb::Message* msg) const {
  const MessageTemplate* tmpl = &message_template;
  if (data.size() < 8) {
    return Status::Error("sbe: data too short for header: " + std::to_string(data.size()) +
                         " bytes");
  }
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
  // A template id is unique only within a schema, so the template id alone
  // does not say the bytes are this message's: they may be another schema's
  // message with the same id. The version is not checked.
  uint16_t schema_id = LoadU16(data.data() + 4);
  if (schema_id != tmpl->schema_id) {
    return Status::Error("sbe: schema ID mismatch: got " + std::to_string(schema_id) + ", want " +
                         std::to_string(tmpl->schema_id) + " for template " +
                         std::to_string(tmpl->template_id));
  }
  if (block_length < tmpl->block_length) {
    return Status::Error("sbe: wire blockLength " + std::to_string(block_length) +
                         " < schema blockLength " + std::to_string(tmpl->block_length) +
                         " for template " + std::to_string(tmpl->template_id));
  }
  if (data.size() < 8u + block_length) {
    return Status::Error("sbe: data too short for root block: need " +
                         std::to_string(8u + block_length) + ", have " +
                         std::to_string(data.size()));
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
  return check::Validate(validator_, *msg, nullptr);
}

}  // namespace protowire::sbe

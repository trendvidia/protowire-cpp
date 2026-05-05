// SBE unmarshal: SBE binary buffer → proto::Message.

#include "protowire/sbe.h"

#include <cstring>
#include <span>
#include <string>

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
    case 1: return *p;
    case 2: return LoadU16(p);
    case 4: return LoadU32(p);
    case 8: return LoadU64(p);
  }
  return 0;
}

Status DecodeFields(std::span<const uint8_t> block,
                    const std::vector<FieldTemplate>& fields,
                    pb::Message* msg);

void DecodeScalarField(const uint8_t* p, const FieldTemplate& ft,
                       pb::Message* msg) {
  const pb::Reflection* r = msg->GetReflection();
  const pb::FieldDescriptor* fd = ft.fd;
  if (ft.encoding == kEncChar) {
    // Char array — null-terminated within ft.size, or the whole region.
    size_t n = 0;
    while (n < ft.size && p[n] != 0) ++n;
    r->SetString(msg, fd, std::string(reinterpret_cast<const char*>(p), n));
    return;
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
}

// DecodeFields walks `fields` against `block`. Returns Status::Error on the
// first composite whose declared offset+size exceeds the enclosing block —
// HARDENING.md § SBE step 5. Scalar fields are bounds-validated by the
// caller's wire_block_length ≥ template_block_length check (step 2).
Status DecodeFields(std::span<const uint8_t> block,
                    const std::vector<FieldTemplate>& fields,
                    pb::Message* msg) {
  for (const FieldTemplate& ft : fields) {
    if (!ft.composite.empty()) {
      if (size_t{ft.offset} + ft.size > block.size()) {
        return Status::Error(
            "sbe: composite field \"" + std::string(ft.fd->name()) +
            "\" exceeds enclosing block");
      }
      pb::Message* sub = msg->GetReflection()->MutableMessage(msg, ft.fd);
      if (Status st = DecodeFields(block.subspan(ft.offset, ft.size),
                                   ft.composite, sub);
          !st.ok()) {
        return st;
      }
      continue;
    }
    DecodeScalarField(block.data() + ft.offset, ft, msg);
  }
  return Status::OK();
}

}  // namespace

Status Codec::Unmarshal(std::span<const uint8_t> data,
                        pb::Message* msg) const {
  const auto* tmpl = TemplateByName(msg->GetDescriptor()->full_name());
  if (!tmpl) {
    return Status::Error("sbe: no template registered for " +
                         std::string(msg->GetDescriptor()->full_name()));
  }
  if (data.size() < 8) return Status::Error("sbe: buffer too short for header");
  // header layout matches Marshal — fields are sanity-checked but otherwise
  // ignored during decode (the codec is identified by message type).
  uint16_t block_length = LoadU16(data.data());
  // HARDENING.md § SBE step 2: a wire block strictly smaller than the
  // schema's block length means at least one field's offset+size exceeds
  // the wire block, so reading any field would over-read. A wire block
  // larger than the template is permitted (forward-compat — newer schema
  // with extra trailing fields).
  if (block_length < tmpl->block_length) {
    return Status::Error("sbe: wire block_length below template");
  }
  if (data.size() < 8u + block_length) {
    return Status::Error("sbe: data too short for root block");
  }
  if (Status st = DecodeFields(data.subspan(8, block_length), tmpl->fields,
                               msg);
      !st.ok()) {
    return st;
  }

  size_t pos = 8 + block_length;
  for (const GroupTemplate& gt : tmpl->groups) {
    if (data.size() < pos + 4) {
      return Status::Error("sbe: truncated group header");
    }
    uint16_t entry_block = LoadU16(data.data() + pos);
    uint16_t count = LoadU16(data.data() + pos + 2);
    pos += 4;
    // HARDENING.md § SBE step 4: zero entry_block_length with non-zero
    // count would otherwise consume zero further bytes while allocating
    // `count` writer entries — an attacker amplification primitive.
    if (entry_block == 0 && count > 0) {
      return Status::Error(
          "sbe: group \"" + std::string(gt.fd->name()) +
          "\" has zero entry_block_length but non-zero count");
    }
    // HARDENING.md § SBE step 2: each group entry must be at least the
    // declared template block_length (smaller would mean fields over-read
    // the wire entry).
    if (count > 0 && entry_block < gt.block_length) {
      return Status::Error(
          "sbe: group \"" + std::string(gt.fd->name()) +
          "\" entry_block_length below template");
    }
    if (data.size() < pos + size_t{entry_block} * count) {
      return Status::Error("sbe: truncated group body");
    }
    const pb::Reflection* r = msg->GetReflection();
    for (uint16_t i = 0; i < count; ++i) {
      pb::Message* entry = r->AddMessage(msg, gt.fd);
      if (Status st = DecodeFields(data.subspan(pos, entry_block), gt.fields,
                                   entry);
          !st.ok()) {
        return st;
      }
      pos += entry_block;
    }
  }
  return Status::OK();
}

}  // namespace protowire::sbe

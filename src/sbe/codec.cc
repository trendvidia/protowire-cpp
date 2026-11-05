// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/sbe.h"

#include <string>
#include <utility>

#include "header_bounds.h"
#include "protowire/sbe/annotations.h"

namespace protowire::sbe {

namespace pb = google::protobuf;

namespace {
constexpr size_t kHeaderSize = 8;  // blockLength, templateId, schemaId, version (uint16 each)
inline uint16_t LoadU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
}  // namespace

StatusOr<Codec> Codec::New(std::vector<const pb::FileDescriptor*> files, CodecOptions opts) {
  Codec c;
  if (opts.max_message_size > 0) c.max_message_size_ = opts.max_message_size;
  if (opts.max_repeated_count > 0) c.max_repeated_ = opts.max_repeated_count;
  c.validator_ = opts.validator;
  for (const pb::FileDescriptor* fd : files) {
    auto schema = GetFileUint32Option(fd, kExtSchemaID);
    if (!schema.has_value()) {
      if (std::string stale = StaleFileError(fd); !stale.empty()) return Status::Error(stale);
      return Status::Error("sbe: file " + std::string(fd->name()) +
                           " missing (sbe.schema_id) option");
    }
    auto version = GetFileUint32Option(fd, kExtVersion).value_or(0);
    if (*schema > static_cast<uint32_t>(kMaxUint16)) {
      return Status::Error("sbe: file " + std::string(fd->name()) +
                           ": (sbe.schema_id) = " + std::to_string(*schema) + " is past " +
                           std::to_string(kMaxUint16) + ", the largest schemaId SBE can state");
    }
    if (version > static_cast<uint32_t>(kMaxUint16)) {
      return Status::Error("sbe: file " + std::string(fd->name()) +
                           ": (sbe.version) = " + std::to_string(version) + " is past " +
                           std::to_string(kMaxUint16) + ", the largest version SBE can state");
    }
    for (int i = 0; i < fd->message_type_count(); ++i) {
      Status st = c.RegisterMessage(
          fd->message_type(i), static_cast<uint16_t>(*schema), static_cast<uint16_t>(version));
      if (!st.ok()) return st;
    }
  }
  return c;
}

Status Codec::RegisterMessage(const pb::Descriptor* md, uint16_t schema_id, uint16_t version) {
  if (!GetMessageUint32Option(md, kExtTemplateID).has_value()) {
    // Only a message with a template_id is registered; one that carries it at
    // the retired number is a stale descriptor, not a plain message.
    if (std::string stale = StaleMessageError(md); !stale.empty()) return Status::Error(stale);
  } else {
    auto tmpl = BuildTemplate(md, schema_id, version);
    if (!tmpl.ok()) return tmpl.status();
    auto owned = std::make_unique<MessageTemplate>(std::move(tmpl).consume());
    // draft -01 § SBE Annotations: "sbe.template_id MUST be unique among
    // messages within a schema". A second message with the same id would
    // replace the first in by_id_, and NewView would then read one as the
    // other. The same message seen again, as when a file is passed twice,
    // is not a duplicate.
    std::string name(md->full_name());
    const uint32_t key = IDKey(schema_id, owned->template_id);
    if (auto prev = by_id_.find(key); prev != by_id_.end() && prev->second.name != name) {
      return Status::Error("sbe: messages " + prev->second.name + " and " + name +
                           " share (sbe.template_id) = " + std::to_string(owned->template_id) +
                           " in schema " + std::to_string(schema_id) +
                           "; it must be unique within a schema");
    }
    by_id_[key] = ByID{name, owned.get()};
    by_name_[name] = std::move(owned);
  }
  for (int i = 0; i < md->nested_type_count(); ++i) {
    Status st = RegisterMessage(md->nested_type(i), schema_id, version);
    if (!st.ok()) return st;
  }
  return Status::OK();
}

const MessageTemplate* Codec::TemplateByName(std::string_view name) const {
  auto it = by_name_.find(std::string(name));
  if (it == by_name_.end()) return nullptr;
  return it->second.get();
}

// ValidateGroups applies HARDENING.md § SBE validation steps 3 and 4 to
// every repeating group, before anything iterates: the header fits, the
// wire block is at least the template's, a non-zero count has a non-zero
// block, count × block fits the remaining input (computed as a division so
// the product cannot overflow), and count is within MaxRepeatedCount.
Status Codec::ValidateGroups(std::span<const uint8_t> data,
                             const MessageTemplate& tmpl,
                             size_t pos) const {
  for (const GroupTemplate& gt : tmpl.groups) {
    if (data.size() < pos + 4) return Status::Error("sbe: data too short for group header");
    const size_t entry_block = LoadU16(data.data() + pos);
    const size_t count = LoadU16(data.data() + pos + 2);
    pos += 4;
    if (entry_block < gt.block_length) {
      return Status::Error("sbe: group " + std::string(gt.fd->name()) + " wire blockLength " +
                           std::to_string(entry_block) + " < schema blockLength " +
                           std::to_string(gt.block_length));
    }
    if (count > 0 && entry_block == 0) {
      return Status::Error("sbe: group " + std::string(gt.fd->name()) + " declares " +
                           std::to_string(count) + " entries of 0 bytes");
    }
    const size_t remaining = data.size() - pos;
    if (entry_block > 0 && count > remaining / entry_block) {
      return Status::Error("sbe: group " + std::string(gt.fd->name()) + " declares " +
                           std::to_string(count) + " entries × " + std::to_string(entry_block) +
                           " bytes, " + std::to_string(remaining) + " bytes remaining");
    }
    if (count > static_cast<size_t>(max_repeated_)) {
      return Status::Error("sbe: group " + std::string(gt.fd->name()) + " declares " +
                           std::to_string(count) +
                           " entries, MaxRepeatedCount=" + std::to_string(max_repeated_));
    }
    pos += entry_block * count;
  }
  return Status::OK();
}

StatusOr<View> Codec::NewView(std::span<const uint8_t> data) const {
  if (data.size() > static_cast<size_t>(max_message_size_)) {
    return Status::Error("sbe: input of " + std::to_string(data.size()) +
                         " bytes exceeds MaxMessageSize=" + std::to_string(max_message_size_));
  }
  if (data.size() < kHeaderSize) {
    return Status::Error("sbe: data too short for header");
  }
  uint16_t block_length = LoadU16(data.data());
  uint16_t template_id = LoadU16(data.data() + 2);
  uint16_t schema_id = LoadU16(data.data() + 4);
  // The header's schema id and template id together name the template: a
  // template id is unique only within a schema.
  auto found = by_id_.find(IDKey(schema_id, template_id));
  if (found == by_id_.end()) {
    return Status::Error("sbe: unknown template ID " + std::to_string(template_id) + " in schema " +
                         std::to_string(schema_id));
  }
  const MessageTemplate* tmpl = found->second.tmpl;
  // Schema evolution may push the wire block past the template's (newer
  // schemas append fields); a smaller one would underrun field reads.
  if (block_length < tmpl->block_length) {
    return Status::Error("sbe: wire blockLength " + std::to_string(block_length) +
                         " < schema blockLength " + std::to_string(tmpl->block_length) +
                         " for template " + std::to_string(template_id));
  }
  size_t end = kHeaderSize + block_length;
  if (data.size() < end) {
    return Status::Error("sbe: data too short for root block");
  }
  if (Status st = ValidateGroups(data, *tmpl, end); !st.ok()) return st;
  return View(data, data.subspan(kHeaderSize, block_length), tmpl, &tmpl->fields, &tmpl->groups);
}

}  // namespace protowire::sbe

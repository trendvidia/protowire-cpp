// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/sbe.h"

#include <string>
#include <utility>

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
  for (const pb::FileDescriptor* fd : files) {
    auto schema = GetFileUint32Option(fd, kExtSchemaID);
    if (!schema.has_value()) {
      return Status::Error(std::string(fd->name()) + " missing (sbe.schema_id) file option");
    }
    auto version = GetFileUint32Option(fd, kExtVersion).value_or(0);
    for (int i = 0; i < fd->message_type_count(); ++i) {
      Status st = c.RegisterMessage(
          fd->message_type(i), static_cast<uint16_t>(*schema), static_cast<uint16_t>(version));
      if (!st.ok()) return st;
    }
  }
  return c;
}

Status Codec::RegisterMessage(const pb::Descriptor* md, uint16_t schema_id, uint16_t version) {
  if (GetMessageUint32Option(md, kExtTemplateID).has_value()) {
    auto tmpl = BuildTemplate(md, schema_id, version);
    if (!tmpl.ok()) return tmpl.status();
    auto owned = std::make_unique<MessageTemplate>(std::move(tmpl).consume());
    by_id_[owned->template_id] = owned.get();
    by_name_[std::string(md->full_name())] = std::move(owned);
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
    if (data.size() < pos + 4) return Status::Error("sbe: truncated group header");
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
  auto it = by_id_.find(template_id);
  if (it == by_id_.end()) {
    return Status::Error("sbe: unknown template id " + std::to_string(template_id));
  }
  // Schema evolution may push the wire block past the template's (newer
  // schemas append fields); a smaller one would underrun field reads.
  if (block_length < it->second->block_length) {
    return Status::Error("sbe: wire blockLength " + std::to_string(block_length) +
                         " < schema blockLength " + std::to_string(it->second->block_length) +
                         " for template " + std::to_string(template_id));
  }
  size_t end = kHeaderSize + block_length;
  if (data.size() < end) {
    return Status::Error("sbe: data too short for root block");
  }
  if (Status st = ValidateGroups(data, *it->second, end); !st.ok()) return st;
  return View(data,
              data.subspan(kHeaderSize, block_length),
              it->second,
              &it->second->fields,
              &it->second->groups);
}

}  // namespace protowire::sbe

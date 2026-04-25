#include "protowire/sbe.h"

#include <utility>

#include "protowire/sbe/annotations.h"

namespace protowire::sbe {

namespace pb = google::protobuf;

namespace {
constexpr size_t kHeaderSize = 8;       // blockLength, templateId, schemaId, version (uint16 each)
inline uint16_t LoadU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
}  // namespace

StatusOr<Codec> Codec::New(
    std::vector<const pb::FileDescriptor*> files) {
  Codec c;
  for (const pb::FileDescriptor* fd : files) {
    auto schema = GetFileUint32Option(fd, kExtSchemaID);
    if (!schema.has_value()) {
      return Status::Error(std::string(fd->name()) +
                           " missing (sbe.schema_id) file option");
    }
    auto version = GetFileUint32Option(fd, kExtVersion).value_or(0);
    for (int i = 0; i < fd->message_type_count(); ++i) {
      Status st = c.RegisterMessage(fd->message_type(i),
                                    static_cast<uint16_t>(*schema),
                                    static_cast<uint16_t>(version));
      if (!st.ok()) return st;
    }
  }
  return c;
}

Status Codec::RegisterMessage(const pb::Descriptor* md, uint16_t schema_id,
                              uint16_t version) {
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

StatusOr<View> Codec::NewView(std::span<const uint8_t> data) const {
  if (data.size() < kHeaderSize) {
    return Status::Error("sbe: data too short for header");
  }
  uint16_t block_length = LoadU16(data.data());
  uint16_t template_id = LoadU16(data.data() + 2);
  auto it = by_id_.find(template_id);
  if (it == by_id_.end()) {
    return Status::Error("sbe: unknown template id " +
                         std::to_string(template_id));
  }
  size_t end = kHeaderSize + block_length;
  if (data.size() < end) {
    return Status::Error("sbe: data too short for root block");
  }
  return View(data, data.subspan(kHeaderSize, block_length), it->second,
              &it->second->fields, &it->second->groups);
}

}  // namespace protowire::sbe

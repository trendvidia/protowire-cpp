#include "protowire/sbe/template.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "protowire/sbe/annotations.h"

namespace protowire::sbe {

namespace pb = google::protobuf;

namespace {

std::vector<const pb::FieldDescriptor*> SortedFields(const pb::Descriptor* md) {
  std::vector<const pb::FieldDescriptor*> out;
  out.reserve(md->field_count());
  for (int i = 0; i < md->field_count(); ++i) out.push_back(md->field(i));
  std::sort(out.begin(), out.end(), [](auto* a, auto* b) {
    return a->number() < b->number();
  });
  return out;
}

StatusOr<std::pair<std::string, uint16_t>> FieldEncodingSize(
    const pb::FieldDescriptor* fd) {
  if (auto enc = GetFieldStringOption(fd, kExtEncoding); enc.has_value()) {
    const std::string& e = *enc;
    if (e == kEncInt8 || e == kEncUint8) return std::make_pair(e, uint16_t{1});
    if (e == kEncInt16 || e == kEncUint16) return std::make_pair(e, uint16_t{2});
    if (e == kEncInt32 || e == kEncUint32 || e == kEncFloat)
      return std::make_pair(e, uint16_t{4});
    if (e == kEncInt64 || e == kEncUint64 || e == kEncDouble)
      return std::make_pair(e, uint16_t{8});
    return Status::Error("unknown encoding \"" + e + "\"");
  }
  switch (fd->cpp_type()) {
    case pb::FieldDescriptor::CPPTYPE_BOOL:
      return std::make_pair(std::string(kEncUint8), uint16_t{1});
    case pb::FieldDescriptor::CPPTYPE_INT32:
      return std::make_pair(std::string(kEncInt32), uint16_t{4});
    case pb::FieldDescriptor::CPPTYPE_INT64:
      return std::make_pair(std::string(kEncInt64), uint16_t{8});
    case pb::FieldDescriptor::CPPTYPE_UINT32:
      return std::make_pair(std::string(kEncUint32), uint16_t{4});
    case pb::FieldDescriptor::CPPTYPE_UINT64:
      return std::make_pair(std::string(kEncUint64), uint16_t{8});
    case pb::FieldDescriptor::CPPTYPE_FLOAT:
      return std::make_pair(std::string(kEncFloat), uint16_t{4});
    case pb::FieldDescriptor::CPPTYPE_DOUBLE:
      return std::make_pair(std::string(kEncDouble), uint16_t{8});
    case pb::FieldDescriptor::CPPTYPE_ENUM:
      return std::make_pair(std::string(kEncUint8), uint16_t{1});
    case pb::FieldDescriptor::CPPTYPE_STRING: {
      auto len = GetFieldUint32Option(fd, kExtLength);
      if (!len.has_value()) {
        return Status::Error("string/bytes field requires (sbe.length)");
      }
      return std::make_pair(std::string(kEncChar),
                            static_cast<uint16_t>(*len));
    }
    default:
      return Status::Error("unsupported proto type for SBE");
  }
}

StatusOr<std::vector<FieldTemplate>> BuildCompositeFields(
    const pb::Descriptor* md, uint16_t* total_size);

StatusOr<std::vector<FieldTemplate>> BuildCompositeFields(
    const pb::Descriptor* md, uint16_t* total_size) {
  std::vector<FieldTemplate> out;
  uint16_t offset = 0;
  for (const pb::FieldDescriptor* fd : SortedFields(md)) {
    if (fd->is_repeated() || fd->is_map()) {
      return Status::Error("composite contains list/map field");
    }
    if (fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
      uint16_t sub_size = 0;
      auto subs = BuildCompositeFields(fd->message_type(), &sub_size);
      if (!subs.ok()) return subs.status();
      FieldTemplate ft;
      ft.fd = fd;
      ft.offset = offset;
      ft.size = sub_size;
      ft.composite = std::move(subs).consume();
      out.push_back(std::move(ft));
      offset += sub_size;
      continue;
    }
    auto es = FieldEncodingSize(fd);
    if (!es.ok()) return es.status();
    FieldTemplate ft;
    ft.fd = fd;
    ft.offset = offset;
    ft.encoding = es->first;
    ft.size = es->second;
    out.push_back(ft);
    offset += ft.size;
  }
  *total_size = offset;
  return out;
}

StatusOr<GroupTemplate> BuildGroupTemplate(const pb::FieldDescriptor* fd) {
  const pb::Descriptor* md = fd->message_type();
  GroupTemplate gt;
  gt.fd = fd;
  uint16_t offset = 0;
  for (const pb::FieldDescriptor* f : SortedFields(md)) {
    if (f->is_map() || f->is_repeated()) {
      return Status::Error("group field cannot be repeated/map");
    }
    if (f->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
      uint16_t sub_size = 0;
      auto subs = BuildCompositeFields(f->message_type(), &sub_size);
      if (!subs.ok()) return subs.status();
      FieldTemplate ft;
      ft.fd = f;
      ft.offset = offset;
      ft.size = sub_size;
      ft.composite = std::move(subs).consume();
      gt.fields.push_back(std::move(ft));
      offset += sub_size;
      continue;
    }
    auto es = FieldEncodingSize(f);
    if (!es.ok()) return es.status();
    FieldTemplate ft;
    ft.fd = f;
    ft.offset = offset;
    ft.encoding = es->first;
    ft.size = es->second;
    gt.fields.push_back(ft);
    offset += ft.size;
  }
  gt.block_length = offset;
  return gt;
}

}  // namespace

StatusOr<MessageTemplate> BuildTemplate(const pb::Descriptor* md,
                                       uint16_t schema_id, uint16_t version) {
  auto tid = GetMessageUint32Option(md, kExtTemplateID);
  if (!tid.has_value()) {
    return Status::Error(std::string(md->full_name()) +
                         " missing (sbe.template_id)");
  }
  MessageTemplate tmpl;
  tmpl.template_id = static_cast<uint16_t>(*tid);
  tmpl.schema_id = schema_id;
  tmpl.version = version;

  uint16_t offset = 0;
  for (const pb::FieldDescriptor* fd : SortedFields(md)) {
    if (fd->is_map()) {
      return Status::Error("map field not supported in SBE");
    }
    if (fd->is_repeated() &&
        fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
      auto gt = BuildGroupTemplate(fd);
      if (!gt.ok()) return gt.status();
      tmpl.groups.push_back(std::move(gt).consume());
      continue;
    }
    if (fd->is_repeated()) {
      return Status::Error("repeated scalar not supported in SBE");
    }
    if (fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
      uint16_t sub_size = 0;
      auto subs = BuildCompositeFields(fd->message_type(), &sub_size);
      if (!subs.ok()) return subs.status();
      FieldTemplate ft;
      ft.fd = fd;
      ft.offset = offset;
      ft.size = sub_size;
      ft.composite = std::move(subs).consume();
      tmpl.fields.push_back(std::move(ft));
      offset += sub_size;
      continue;
    }
    auto es = FieldEncodingSize(fd);
    if (!es.ok()) return es.status();
    FieldTemplate ft;
    ft.fd = fd;
    ft.offset = offset;
    ft.encoding = es->first;
    ft.size = es->second;
    tmpl.fields.push_back(ft);
    offset += ft.size;
  }
  tmpl.block_length = offset;
  return tmpl;
}

}  // namespace protowire::sbe

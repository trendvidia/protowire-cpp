// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/sbe/template.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "encoding_range.h"
#include "header_bounds.h"
#include "protowire/detail/go_quote.h"
#include "protowire/sbe/annotations.h"

namespace protowire::sbe {

namespace pb = google::protobuf;

namespace {

std::vector<const pb::FieldDescriptor*> SortedFields(const pb::Descriptor* md) {
  std::vector<const pb::FieldDescriptor*> out;
  out.reserve(md->field_count());
  for (int i = 0; i < md->field_count(); ++i) out.push_back(md->field(i));
  // The comparator orders by the proto field's `number()` (a stable
  // schema-defined integer), not by pointer address — clang-tidy's
  // bugprone-nondeterministic-pointer-iteration-order flags any std::sort
  // over a pointer container, but the resulting order is fully
  // deterministic across runs.
  // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order)
  std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->number() < b->number(); });
  return out;
}

// "pkg.Msg.field", as Go's "%s.%s" of a message's full name and a field's
// name writes it.
std::string FieldPath(const pb::Descriptor* md, const pb::FieldDescriptor* fd) {
  return std::string(md->full_name()) + "." + std::string(fd->name());
}

Status Wrap(const std::string& prefix, const Status& st) {
  return Status::Error(prefix + ": " + st.message());
}

// EndOfField returns the offset after a field of size bytes placed at
// offset, refusing a field that ends past what a uint16 blockLength can
// state. Every offset in the block is then a uint16 as well.
StatusOr<int> EndOfField(int offset, uint16_t size) {
  const int end = offset + size;
  if (end > kMaxUint16) {
    return Status::Error("ends at byte " + std::to_string(end) + " of its block, past " +
                         std::to_string(kMaxUint16) + ", the largest blockLength SBE can state");
  }
  return end;
}

StatusOr<std::pair<std::string, uint16_t>> FieldEncodingSize(const pb::FieldDescriptor* fd) {
  if (auto enc = GetFieldStringOption(fd, kExtEncoding); enc.has_value()) {
    const std::string& e = *enc;
    // The draft defines (sbe.encoding) as narrowing a numeric type, and says
    // nothing of crossing between integers and floating point
    // (protowire#549), so neither binds.
    const std::string kind(fd->type_name());
    switch (fd->cpp_type()) {
      case pb::FieldDescriptor::CPPTYPE_STRING:
        return Status::Error("(sbe.encoding) = " + detail::GoQuote(e) + " on a " + kind +
                             " field; it narrows a numeric field, and a " + kind +
                             " field takes (sbe.length)");
      case pb::FieldDescriptor::CPPTYPE_FLOAT:
      case pb::FieldDescriptor::CPPTYPE_DOUBLE:
        if (IsIntEncoding(e)) {
          return Status::Error("(sbe.encoding) = " + detail::GoQuote(e) +
                               " is an integer primitive and the field is " + kind);
        }
        break;
      default:
        if (e == kEncFloat || e == kEncDouble) {
          return Status::Error("(sbe.encoding) = " + detail::GoQuote(e) +
                               " is a floating-point primitive and the field is " + kind);
        }
        break;
    }
    if (e == kEncInt8 || e == kEncUint8) return std::make_pair(e, uint16_t{1});
    if (e == kEncInt16 || e == kEncUint16) return std::make_pair(e, uint16_t{2});
    if (e == kEncInt32 || e == kEncUint32 || e == kEncFloat) return std::make_pair(e, uint16_t{4});
    if (e == kEncInt64 || e == kEncUint64 || e == kEncDouble) return std::make_pair(e, uint16_t{8});
    return Status::Error("unknown encoding " + detail::GoQuote(e));
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
        return Status::Error(std::string(fd->type_name()) +
                             " field requires (sbe.length) annotation");
      }
      if (*len > static_cast<uint32_t>(kMaxUint16)) {
        return Status::Error("(sbe.length) = " + std::to_string(*len) + " is past " +
                             std::to_string(kMaxUint16) +
                             ", the largest blockLength SBE can state");
      }
      return std::make_pair(std::string(kEncChar), static_cast<uint16_t>(*len));
    }
    default:
      return Status::Error("unsupported proto kind " + std::string(fd->type_name()));
  }
}

StatusOr<std::vector<FieldTemplate>> BuildCompositeFields(const pb::Descriptor* md,
                                                          uint16_t* total_size);

StatusOr<std::vector<FieldTemplate>> BuildCompositeFields(const pb::Descriptor* md,
                                                          uint16_t* total_size) {
  std::vector<FieldTemplate> out;
  int offset = 0;
  for (const pb::FieldDescriptor* fd : SortedFields(md)) {
    const std::string where = "composite field " + FieldPath(md, fd);
    if (fd->is_repeated() || fd->is_map()) {
      return Status::Error("composite " + std::string(md->full_name()) +
                           " contains list/map field " + std::string(fd->name()));
    }
    if (fd->real_containing_oneof() != nullptr) {
      return Status::Error("composite " + std::string(md->full_name()) + " contains oneof field " +
                           std::string(fd->name()));
    }
    if (fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
      uint16_t sub_size = 0;
      auto subs = BuildCompositeFields(fd->message_type(), &sub_size);
      if (!subs.ok()) return subs.status();
      auto end = EndOfField(offset, sub_size);
      if (!end.ok()) return Wrap(where, end.status());
      FieldTemplate ft;
      ft.fd = fd;
      ft.offset = static_cast<uint16_t>(offset);
      ft.size = sub_size;
      ft.composite = std::move(subs).consume();
      out.push_back(std::move(ft));
      offset = *end;
      continue;
    }
    auto es = FieldEncodingSize(fd);
    if (!es.ok()) return Wrap(where, es.status());
    auto end = EndOfField(offset, es->second);
    if (!end.ok()) return Wrap(where, end.status());
    FieldTemplate ft;
    ft.fd = fd;
    ft.offset = static_cast<uint16_t>(offset);
    ft.encoding = es->first;
    ft.size = es->second;
    out.push_back(ft);
    offset = *end;
  }
  *total_size = static_cast<uint16_t>(offset);
  return out;
}

StatusOr<GroupTemplate> BuildGroupTemplate(const pb::FieldDescriptor* fd) {
  const pb::Descriptor* md = fd->message_type();
  GroupTemplate gt;
  gt.fd = fd;
  int offset = 0;
  for (const pb::FieldDescriptor* f : SortedFields(md)) {
    if (f->is_map()) {
      return Status::Error("sbe: map field in group " + std::string(md->full_name()) +
                           " not supported");
    }
    if (f->is_repeated()) {
      return Status::Error("sbe: nested repeated field in group " + std::string(md->full_name()) +
                           " not supported");
    }
    if (f->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
      const std::string where = "sbe: composite in group " + FieldPath(md, f);
      uint16_t sub_size = 0;
      auto subs = BuildCompositeFields(f->message_type(), &sub_size);
      if (!subs.ok()) return Wrap(where, subs.status());
      auto end = EndOfField(offset, sub_size);
      if (!end.ok()) return Wrap(where, end.status());
      FieldTemplate ft;
      ft.fd = f;
      ft.offset = static_cast<uint16_t>(offset);
      ft.size = sub_size;
      ft.composite = std::move(subs).consume();
      gt.fields.push_back(std::move(ft));
      offset = *end;
      continue;
    }
    const std::string where = "sbe: group field " + FieldPath(md, f);
    auto es = FieldEncodingSize(f);
    if (!es.ok()) return Wrap(where, es.status());
    auto end = EndOfField(offset, es->second);
    if (!end.ok()) return Wrap(where, end.status());
    FieldTemplate ft;
    ft.fd = f;
    ft.offset = static_cast<uint16_t>(offset);
    ft.encoding = es->first;
    ft.size = es->second;
    gt.fields.push_back(ft);
    offset = *end;
  }
  gt.block_length = static_cast<uint16_t>(offset);
  return gt;
}

}  // namespace

StatusOr<MessageTemplate> BuildTemplate(const pb::Descriptor* md,
                                        uint16_t schema_id,
                                        uint16_t version) {
  const std::string name(md->full_name());
  auto tid = GetMessageUint32Option(md, kExtTemplateID);
  if (!tid.has_value()) {
    return Status::Error("sbe: message " + name + " missing (sbe.template_id)");
  }
  if (*tid > static_cast<uint32_t>(kMaxUint16)) {
    return Status::Error("sbe: message " + name + ": (sbe.template_id) = " + std::to_string(*tid) +
                         " is past " + std::to_string(kMaxUint16) +
                         ", the largest templateId SBE can state");
  }
  MessageTemplate tmpl;
  tmpl.template_id = static_cast<uint16_t>(*tid);
  tmpl.schema_id = schema_id;
  tmpl.version = version;

  int offset = 0;
  for (const pb::FieldDescriptor* fd : SortedFields(md)) {
    if (fd->is_map()) {
      return Status::Error("sbe: map field " + FieldPath(md, fd) + " not supported");
    }
    if (fd->real_containing_oneof() != nullptr) {
      return Status::Error("sbe: oneof field " + FieldPath(md, fd) + " not supported");
    }
    if (fd->is_repeated() && fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
      auto gt = BuildGroupTemplate(fd);
      if (!gt.ok()) return gt.status();
      tmpl.groups.push_back(std::move(gt).consume());
      continue;
    }
    if (fd->is_repeated()) {
      return Status::Error("sbe: repeated scalar field " + FieldPath(md, fd) +
                           " not supported; wrap in a message");
    }
    if (fd->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
      const std::string where = "sbe: composite " + FieldPath(md, fd);
      uint16_t sub_size = 0;
      auto subs = BuildCompositeFields(fd->message_type(), &sub_size);
      if (!subs.ok()) return Wrap(where, subs.status());
      auto end = EndOfField(offset, sub_size);
      if (!end.ok()) return Wrap(where, end.status());
      FieldTemplate ft;
      ft.fd = fd;
      ft.offset = static_cast<uint16_t>(offset);
      ft.size = sub_size;
      ft.composite = std::move(subs).consume();
      tmpl.fields.push_back(std::move(ft));
      offset = *end;
      continue;
    }
    const std::string where = "sbe: field " + FieldPath(md, fd);
    auto es = FieldEncodingSize(fd);
    if (!es.ok()) return Wrap(where, es.status());
    auto end = EndOfField(offset, es->second);
    if (!end.ok()) return Wrap(where, end.status());
    FieldTemplate ft;
    ft.fd = fd;
    ft.offset = static_cast<uint16_t>(offset);
    ft.encoding = es->first;
    ft.size = es->second;
    tmpl.fields.push_back(ft);
    offset = *end;
  }
  tmpl.block_length = static_cast<uint16_t>(offset);
  return tmpl;
}

}  // namespace protowire::sbe

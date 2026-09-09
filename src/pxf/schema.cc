// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/schema.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/descriptor.h>

#include "protowire/pxf/annotations.h"

namespace protowire::pxf {

namespace {

namespace pb = google::protobuf;

// Case-sensitive set of names PXF reserves as value keywords and
// therefore forbids as schema element names.
bool IsReservedName(std::string_view name) {
  return name == "null" || name == "true" || name == "false";
}

void WalkEnumsForFile(const std::string& path,
                      const pb::FileDescriptor* fd,
                      std::vector<Violation>* out) {
  for (int i = 0; i < fd->enum_type_count(); ++i) {
    const pb::EnumDescriptor* e = fd->enum_type(i);
    for (int j = 0; j < e->value_count(); ++j) {
      const pb::EnumValueDescriptor* v = e->value(j);
      if (IsReservedName(v->name())) {
        out->push_back(Violation{
            path, std::string(v->full_name()), std::string(v->name()), ViolationKind::kEnumValue});
      }
    }
  }
}

void WalkEnumsForMessage(const std::string& path,
                         const pb::Descriptor* md,
                         std::vector<Violation>* out) {
  for (int i = 0; i < md->enum_type_count(); ++i) {
    const pb::EnumDescriptor* e = md->enum_type(i);
    for (int j = 0; j < e->value_count(); ++j) {
      const pb::EnumValueDescriptor* v = e->value(j);
      if (IsReservedName(v->name())) {
        out->push_back(Violation{
            path, std::string(v->full_name()), std::string(v->name()), ViolationKind::kEnumValue});
      }
    }
  }
}

// CheckKeyOption validates the placement of a (pxf.key) annotation on f
// per draft -01 §3.13: the annotated field must be a repeated
// message-typed field, and the annotation value must name a singular
// string field of the element message.
void CheckKeyOption(const std::string& path,
                    const pb::FieldDescriptor* f,
                    const std::string& key_name,
                    std::vector<Violation>* out) {
  auto violation = [&](std::string detail) {
    out->push_back(Violation{
        path, std::string(f->full_name()), key_name, ViolationKind::kKeyOption, std::move(detail)});
  };
  if (!f->is_repeated() || f->is_map() || f->cpp_type() != pb::FieldDescriptor::CPPTYPE_MESSAGE) {
    violation("(pxf.key) is valid only on repeated message-typed fields");
    return;
  }
  const pb::FieldDescriptor* kf = f->message_type()->FindFieldByName(key_name);
  if (kf == nullptr) {
    violation("element message " + std::string(f->message_type()->full_name()) +
              " has no field \"" + key_name + "\"");
    return;
  }
  if (kf->is_repeated() || kf->is_map() || kf->type() != pb::FieldDescriptor::TYPE_STRING) {
    violation("key field " + std::string(kf->full_name()) + " must be a singular string field");
  }
}

void WalkMessages(const std::string& path, const pb::Descriptor* md, std::vector<Violation>* out) {
  for (int i = 0; i < md->field_count(); ++i) {
    const pb::FieldDescriptor* f = md->field(i);
    if (IsReservedName(f->name())) {
      out->push_back(Violation{
          path, std::string(f->full_name()), std::string(f->name()), ViolationKind::kField});
    }
    if (auto key = KeyFieldName(f); key.has_value()) {
      CheckKeyOption(path, f, *key, out);
    }
  }
  // Skip synthetic oneofs (those generated for proto3 optional fields).
  // libprotobuf exposes a real oneof count and a "real" count; the
  // synthetic entries sit at the tail. Iterating [0, real_oneof_decl_count)
  // gives us only the user-declared oneofs, mirroring the Go reference's
  // IsSynthetic() filter.
  int real_oneofs =
      md->real_oneof_decl_count() > 0 ? md->real_oneof_decl_count() : md->oneof_decl_count();
  for (int i = 0; i < real_oneofs; ++i) {
    const pb::OneofDescriptor* o = md->oneof_decl(i);
    if (IsReservedName(o->name())) {
      out->push_back(Violation{
          path, std::string(o->full_name()), std::string(o->name()), ViolationKind::kOneof});
    }
  }
  for (int i = 0; i < md->nested_type_count(); ++i) {
    WalkMessages(path, md->nested_type(i), out);
  }
  WalkEnumsForMessage(path, md, out);
}

}  // namespace

const char* ViolationKindName(ViolationKind k) {
  switch (k) {
    case ViolationKind::kField:
      return "message field";
    case ViolationKind::kOneof:
      return "oneof";
    case ViolationKind::kEnumValue:
      return "enum value";
    case ViolationKind::kKeyOption:
      return "(pxf.key) placement";
  }
  return "unknown";
}

std::string Violation::ToString() const {
  if (kind == ViolationKind::kKeyOption) {
    return file + ": field \"" + element + "\" carries (pxf.key) = \"" + name + "\": " + detail +
           " (draft -01 §3.13)";
  }
  return file + ": " + ViolationKindName(kind) + " \"" + element + "\" uses PXF-reserved name \"" +
         name + "\" (draft §3.13)";
}

std::vector<Violation> ValidateDescriptor(const pb::Descriptor* desc) {
  if (desc == nullptr) return {};
  return ValidateFile(desc->file());
}

std::vector<Violation> ValidateFile(const pb::FileDescriptor* fd) {
  if (fd == nullptr) return {};
  std::vector<Violation> out;
  std::string path(fd->name());
  for (int i = 0; i < fd->message_type_count(); ++i) {
    WalkMessages(path, fd->message_type(i), &out);
  }
  WalkEnumsForFile(path, fd, &out);
  // Stable, deterministic output keyed by element FQN.
  std::sort(out.begin(), out.end(), [](const Violation& a, const Violation& b) {
    return a.element < b.element;
  });
  return out;
}

bool IsFutureReservedDirective(std::string_view name) {
  // Names the spec reserves for future allocation (draft §3.4.6).
  // Names with their own production (`type`, `dataset`, `proto`) and
  // the spec-registered `entry` aren't included here — they're handled
  // by the lexer or the named_directive shape.
  return name == "table" || name == "datasource" || name == "view" || name == "procedure" ||
         name == "function" || name == "permissions";
}

}  // namespace protowire::pxf

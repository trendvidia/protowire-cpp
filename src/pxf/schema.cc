// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/schema.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/descriptor.h>

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

void WalkMessages(const std::string& path, const pb::Descriptor* md, std::vector<Violation>* out) {
  for (int i = 0; i < md->field_count(); ++i) {
    const pb::FieldDescriptor* f = md->field(i);
    if (IsReservedName(f->name())) {
      out->push_back(Violation{
          path, std::string(f->full_name()), std::string(f->name()), ViolationKind::kField});
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
  }
  return "unknown";
}

std::string Violation::ToString() const {
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

}  // namespace protowire::pxf

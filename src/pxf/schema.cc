// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/schema.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/unknown_field_set.h>

#include "carrier.h"
#include "parser_internal.h"
#include "protowire/pxf/annotations.h"
#include "protowire/pxf/wellknown.h"

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

// CheckDefaultPlacement validates a default on f (draft -01
// §annotation-extensions, "Default Placement"): it carries one literal, so
// it is valid only on a field one literal can denote. A literal that does
// not parse as the field's type stays a decode-time error. The rejected set
// is the set ApplyDefault in decode_fast.cc cannot honor; keep them in step.
void CheckDefaultPlacement(const std::string& path,
                           const pb::FieldDescriptor* f,
                           const FieldAnnotations& a,
                           std::vector<Violation>* out) {
  const std::string annotation = a.default_from_carrier ? "@default" : "(pxf.default)";
  auto violation = [&](std::string detail) {
    out->push_back(Violation{path,
                             std::string(f->full_name()),
                             *a.def,
                             ViolationKind::kDefaultOption,
                             annotation + " " + std::move(detail),
                             a.default_from_carrier});
  };
  if (f->is_map()) {
    violation("is not valid on map fields: one literal cannot denote a map");
  } else if (f->is_repeated()) {
    violation("is not valid on repeated fields: one literal cannot denote a list");
  } else if (f->type() == pb::FieldDescriptor::TYPE_GROUP) {
    violation("is not valid on group fields");
  } else if (f->type() == pb::FieldDescriptor::TYPE_MESSAGE && !IsDefaultable(f->message_type())) {
    violation("is not valid on message type " + std::string(f->message_type()->full_name()) +
              ": no PXF literal denotes it");
  }
}

// OneofDefault is one oneof member carrying a default.
struct OneofDefault {
  const pb::OneofDescriptor* oneof;
  const pb::FieldDescriptor* field;
  std::string def;
  bool from_carrier;
};

// CheckOneofDefaultCap reports every member of a oneof that carries a
// default when a sibling does too (draft -01 §annotation-extensions, "Oneof
// Members"). With two, one must win, and declaration order or field number
// would attach meaning to a detail authors may change. `annotated` is in
// field order, and a group is reported at its first member, so the order
// does not depend on anything but the schema.
void CheckOneofDefaultCap(const std::string& path,
                          const std::vector<OneofDefault>& annotated,
                          std::vector<Violation>* out) {
  for (size_t i = 0; i < annotated.size(); ++i) {
    const pb::OneofDescriptor* oneof = annotated[i].oneof;
    bool first = true;
    for (size_t j = 0; j < i; ++j) first = first && annotated[j].oneof != oneof;
    if (!first) continue;
    std::string names;
    int count = 0;
    for (const OneofDefault& m : annotated) {
      if (m.oneof != oneof) continue;
      if (count++ > 0) names += ", ";
      names += std::string(m.field->name());
    }
    if (count < 2) continue;
    const std::string detail = "at most one member of oneof \"" + std::string(oneof->name()) +
                               "\" may carry a default; " + std::to_string(count) + " do (" +
                               names + ")";
    for (const OneofDefault& m : annotated) {
      if (m.oneof != oneof) continue;
      out->push_back(Violation{path,
                               std::string(m.field->full_name()),
                               m.def,
                               ViolationKind::kDefaultOption,
                               detail,
                               m.from_carrier});
    }
  }
}

void WalkMessages(const std::string& path, const pb::Descriptor* md, std::vector<Violation>* out) {
  std::vector<OneofDefault> oneof_defaults;
  for (int i = 0; i < md->field_count(); ++i) {
    const pb::FieldDescriptor* f = md->field(i);
    if (IsReservedName(f->name())) {
      out->push_back(Violation{
          path, std::string(f->full_name()), std::string(f->name()), ViolationKind::kField});
    }
    if (auto key = KeyFieldName(f); key.has_value()) {
      CheckKeyOption(path, f, *key, out);
    }
    const FieldAnnotations a = ReadFieldAnnotations(f);
    if (!a.default_problem.empty()) {
      out->push_back(Violation{path,
                               std::string(f->full_name()),
                               "",
                               ViolationKind::kDefaultOption,
                               a.default_problem,
                               a.default_from_carrier});
    }
    if (a.def.has_value()) CheckDefaultPlacement(path, f, a, out);
    if (a.required && a.def.has_value()) {
      const std::string required = a.required_from_carrier ? "@required" : "(pxf.required)";
      const std::string def = a.default_from_carrier ? "@default" : "(pxf.default)";
      out->push_back(Violation{path,
                               std::string(f->full_name()),
                               *a.def,
                               ViolationKind::kRequiredOption,
                               required + " and " + def +
                                   " are exclusive on one field: the first rejects a document "
                                   "where the field is absent, and the second treats it as "
                                   "supplied",
                               a.required_from_carrier});
    }
    // real_containing_oneof leaves out proto3 `optional`, whose synthetic
    // oneof has no sibling to choose instead.
    if (const pb::OneofDescriptor* oneof = f->real_containing_oneof(); oneof != nullptr) {
      if (a.required) {
        const std::string required = a.required_from_carrier ? "@required" : "(pxf.required)";
        out->push_back(Violation{path,
                                 std::string(f->full_name()),
                                 std::string(oneof->name()),
                                 ViolationKind::kRequiredOption,
                                 required + " is not valid on a member of oneof \"" +
                                     std::string(oneof->name()) +
                                     "\": read per field it demands that one arm always be "
                                     "chosen, which makes every other arm undecodable",
                                 a.required_from_carrier});
      }
      if (a.def.has_value()) {
        oneof_defaults.push_back(OneofDefault{oneof, f, *a.def, a.default_from_carrier});
      }
    }
  }
  CheckOneofDefaultCap(path, oneof_defaults, out);
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

// Retired option numbers. Before protowire v1.12.0 the family's extensions
// sat in an unregistered 50000-59999 range; they moved to the registered
// block 1314-1363 in one change (protowire#244). A descriptor compiled
// before that fails silently: (pxf.required) stops being enforced and
// (pxf.default) stops applying. So STABILITY.md promise 3 names the old
// numbers, and draft -01 says to diagnose them. This mirrors
// encoding/pxf/retired.go in protowire-go.
//
// That range is also where projects that never registered numbers put
// theirs, so a number counts only on the Options kind its allocation used,
// and only in a file importing protowire's annotations.
struct RetiredNumber {
  int number;
  const char* was;   // the option as it was spelled
  int now;           // its number in the registered block
  const char* file;  // the annotations file to recompile against
};

constexpr const char* kSbeFile = "sbe/annotations.proto";
constexpr const char* kPxfFile = "pxf/annotations.proto";
constexpr const char* kSchemaFile = "protowire/schema/v1/descriptor.proto";
constexpr const char* kProtocheckFile = "protocheck's annotations";

constexpr RetiredNumber kRetiredFile[] = {
    {50100, "(sbe.schema_id)", 1319, kSbeFile},
    {50101, "(sbe.version)", 1320, kSbeFile},
    {50400, "the file_annotations carrier", 1327, kSchemaFile},
    {50401, "the functions carrier", 1328, kSchemaFile},
    {50402, "the annotation_decls carrier", 1329, kSchemaFile},
    {50403, "the type_decls carrier", 1330, kSchemaFile},
    {50404, "the source_map carrier", 1331, kSchemaFile},
};
constexpr RetiredNumber kRetiredMessage[] = {
    {50200, "(sbe.template_id)", 1321, kSbeFile},
    {50400, "the message_annotations carrier", 1327, kSchemaFile},
    {51001, "a protocheck message constraint", 1348, kProtocheckFile},
};
constexpr RetiredNumber kRetiredField[] = {
    {50000, "(pxf.required)", 1314, kPxfFile},
    {50001, "(pxf.default)", 1315, kPxfFile},
    {50002, "(pxf.key)", 1316, kPxfFile},
    {50300, "(sbe.length)", 1322, kSbeFile},
    {50301, "(sbe.encoding)", 1323, kSbeFile},
    {50400, "the field_annotations carrier", 1327, kSchemaFile},
    {51000, "a protocheck field constraint", 1347, kProtocheckFile},
};
constexpr RetiredNumber kRetiredOneof[] = {
    {50400, "the oneof_annotations carrier", 1327, kSchemaFile},
    {51002, "a protocheck oneof constraint", 1349, kProtocheckFile},
};
// Enum, enum value, service and method options carried only the carrier.
constexpr RetiredNumber kRetiredCarrierOnly[] = {
    {50400, "the *_annotations carrier", 1327, kSchemaFile},
};

// ImportsProtowire reports whether fd imports one of protowire's annotation
// files, in either era.
bool ImportsProtowire(const pb::FileDescriptor* fd) {
  for (int i = 0; i < fd->dependency_count(); ++i) {
    const pb::FileDescriptor* dep = fd->dependency(i);
    if (dep == nullptr) continue;
    const std::string_view name = dep->name();
    if (name == kPxfFile || name == kSbeFile || name == kSchemaFile ||
        name == "protowire/schema/v1/annotations.proto") {
      return true;
    }
  }
  return false;
}

// RetiredIn appends a violation for each number of `table` among the unknown
// fields of opts. Nothing declares these numbers any more, so that is where
// they land.
template <size_t N>
void RetiredIn(const std::string& path,
               const std::string& element,
               const char* kind,
               const pb::Message& opts,
               const RetiredNumber (&table)[N],
               std::vector<Violation>* out) {
  const pb::UnknownFieldSet& unknown = opts.GetReflection()->GetUnknownFields(opts);
  for (int i = 0; i < unknown.field_count(); ++i) {
    const int number = unknown.field(i).number();
    for (const RetiredNumber& r : table) {
      if (r.number != number) continue;
      const std::string num = std::to_string(number);
      out->push_back(Violation{path,
                               element,
                               num,
                               ViolationKind::kRetiredNumber,
                               "option number " + num + " on " + kind + " was " + r.was +
                                   " before protowire v1.12.0 and is " + std::to_string(r.now) +
                                   " now; the descriptor predates the registered extension block "
                                   "and must be recompiled against the current " +
                                   r.file});
    }
  }
}

void WalkRetiredEnum(const std::string& path,
                     const pb::EnumDescriptor* e,
                     std::vector<Violation>* out) {
  RetiredIn(
      path, std::string(e->full_name()), "EnumOptions", e->options(), kRetiredCarrierOnly, out);
  for (int i = 0; i < e->value_count(); ++i) {
    const pb::EnumValueDescriptor* v = e->value(i);
    RetiredIn(path,
              std::string(v->full_name()),
              "EnumValueOptions",
              v->options(),
              kRetiredCarrierOnly,
              out);
  }
}

void WalkRetiredMessage(const std::string& path,
                        const pb::Descriptor* md,
                        std::vector<Violation>* out) {
  RetiredIn(
      path, std::string(md->full_name()), "MessageOptions", md->options(), kRetiredMessage, out);
  for (int i = 0; i < md->field_count(); ++i) {
    const pb::FieldDescriptor* f = md->field(i);
    RetiredIn(path, std::string(f->full_name()), "FieldOptions", f->options(), kRetiredField, out);
  }
  for (int i = 0; i < md->oneof_decl_count(); ++i) {
    const pb::OneofDescriptor* o = md->oneof_decl(i);
    RetiredIn(path, std::string(o->full_name()), "OneofOptions", o->options(), kRetiredOneof, out);
  }
  for (int i = 0; i < md->enum_type_count(); ++i) WalkRetiredEnum(path, md->enum_type(i), out);
  for (int i = 0; i < md->nested_type_count(); ++i) {
    WalkRetiredMessage(path, md->nested_type(i), out);
  }
}

void WalkRetired(const std::string& path,
                 const pb::FileDescriptor* fd,
                 std::vector<Violation>* out) {
  RetiredIn(path, path, "FileOptions", fd->options(), kRetiredFile, out);
  for (int i = 0; i < fd->message_type_count(); ++i) {
    WalkRetiredMessage(path, fd->message_type(i), out);
  }
  for (int i = 0; i < fd->enum_type_count(); ++i) WalkRetiredEnum(path, fd->enum_type(i), out);
  for (int i = 0; i < fd->service_count(); ++i) {
    const pb::ServiceDescriptor* s = fd->service(i);
    RetiredIn(path,
              std::string(s->full_name()),
              "ServiceOptions",
              s->options(),
              kRetiredCarrierOnly,
              out);
    for (int j = 0; j < s->method_count(); ++j) {
      const pb::MethodDescriptor* m = s->method(j);
      RetiredIn(path,
                std::string(m->full_name()),
                "MethodOptions",
                m->options(),
                kRetiredCarrierOnly,
                out);
    }
  }
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
      return "keyed field option";
    case ViolationKind::kRequiredOption:
      return "required field option";
    case ViolationKind::kDefaultOption:
      return "default field option";
    case ViolationKind::kRetiredNumber:
      return "retired option number";
  }
  return "unknown";
}

std::string Violation::ToString() const {
  if (kind == ViolationKind::kKeyOption) {
    return file + ": field \"" + element + "\": invalid (pxf.key) = \"" + name + "\": " + detail +
           " (draft -01 §3.13)";
  }
  if (kind == ViolationKind::kRequiredOption) {
    return file + ": field \"" + element + "\": invalid " +
           (annotation_form ? "@required" : "(pxf.required)") + ": " + detail +
           " (draft -01 §annotation-extensions)";
  }
  if (kind == ViolationKind::kDefaultOption) {
    const std::string annotation = annotation_form ? "@default" : "(pxf.default)";
    const std::string quoted = name.empty() ? "" : " = \"" + name + "\"";
    return file + ": field \"" + element + "\": invalid " + annotation + quoted + ": " + detail +
           " (draft -01 §annotation-extensions)";
  }
  if (kind == ViolationKind::kRetiredNumber) {
    return file + ": \"" + element + "\": " + detail + " (STABILITY.md promise 3)";
  }
  return file + ": " + ViolationKindName(kind) + " \"" + element + "\" uses PXF-reserved name \"" +
         name + "\" (draft §3.13)";
}

std::vector<Violation> ValidateDescriptor(const pb::Descriptor* desc) {
  if (desc == nullptr) return {};
  return ValidateFile(desc->file());
}

namespace {

// FileViolations appends one file's own violations, in walk order.
void FileViolations(const pb::FileDescriptor* fd, std::vector<Violation>* out) {
  std::string path(fd->name());
  for (int i = 0; i < fd->message_type_count(); ++i) {
    WalkMessages(path, fd->message_type(i), out);
  }
  WalkEnumsForFile(path, fd, out);
  if (ImportsProtowire(fd)) WalkRetired(path, fd, out);
}

// ClosureWalk carries the state of one ValidateFile traversal over the
// import closure (draft -01 § Scope of Bind-Time Checks). Deduplication
// is by path rather than by descriptor identity: within one closure a
// path names one file, and the diamond — A imports B and C, both
// importing D — is the common shape that would otherwise report D's
// violations twice. Import closures are small, so a linear scan of the
// seen paths beats a map.
struct ClosureWalk {
  std::vector<std::string> seen;
  std::vector<Violation> out;

  void Walk(const pb::FileDescriptor* fd) {
    if (fd == nullptr) return;
    std::string path(fd->name());
    // Google's own files — descriptor.proto and the well-known types —
    // are in nearly every closure (every annotated schema imports
    // pxf/annotations.proto and, through it, descriptor.proto: 54
    // messages and enums no PXF document can name) and cannot carry a
    // violation: they declare no null / true / false and import none of
    // the annotations. Walking descriptor.proto measured ~1.1 µs, a
    // quarter of a small document's decode, so it is skipped; the
    // assumption is pinned by PxfSchema.GoogleProtobufFilesAreConformant,
    // which validates each of them directly.
    if (path.rfind("google/protobuf/", 0) == 0) return;
    if (std::find(seen.begin(), seen.end(), path) != seen.end()) return;
    seen.push_back(std::move(path));
    FileViolations(fd, &out);
    for (int i = 0; i < fd->dependency_count(); ++i) Walk(fd->dependency(i));
  }
};

}  // namespace

std::vector<Violation> ValidateFile(const pb::FileDescriptor* fd) {
  if (fd == nullptr) return {};
  ClosureWalk w;
  w.Walk(fd);
  std::vector<Violation> out = std::move(w.out);
  // Stable, so the ties one field can produce (a reserved name and a
  // (pxf.key) placement share an element) keep walk order. File first,
  // so a multi-file closure reports one file's violations together;
  // within a file this orders exactly as the single-file walk did.
  std::stable_sort(out.begin(), out.end(), [](const Violation& a, const Violation& b) {
    if (a.file != b.file) return a.file < b.file;
    return a.element < b.element;
  });
  return out;
}

Status CheckDescriptorSchema(const pb::Descriptor* desc) {
  auto vs = ValidateDescriptor(desc);
  if (vs.empty()) return Status::OK();
  std::string msg = "PXF schema violations:\n  ";
  for (size_t i = 0; i < vs.size(); ++i) {
    if (i != 0) msg.append("\n  ");
    msg.append(vs[i].ToString());
  }
  return Status::Error(0, 0, msg);
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

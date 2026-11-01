// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "dataset_paths.h"

#include <string_view>

#include "protowire/pxf/wellknown.h"

namespace protowire::pxf {

namespace {

namespace pb = google::protobuf;

bool HasEmptySegment(std::string_view c) {
  return c.empty() || c.front() == '.' || c.back() == '.' || c.find("..") != std::string_view::npos;
}

// IsPathPrefix reports whether path goes inside the field prefix names:
// "a" is a path prefix of "a.b", and not of "ab".
bool IsPathPrefix(std::string_view prefix, std::string_view path) {
  return path.size() > prefix.size() && path[prefix.size()] == '.' &&
         path.substr(0, prefix.size()) == prefix;
}

// IsLiteralType reports a message type written as a literal of its own. A
// pxf.Secret is not one: it has a block form people write, so a path may
// go into it, and a path into an Any names the Any's own fields.
bool IsLiteralType(const pb::Descriptor* d) {
  return IsTimestamp(d) || IsDuration(d) || WrapperInnerCppType(d) != -1 || IsBigInt(d) ||
         IsDecimal(d) || IsBigFloat(d);
}

std::string Quoted(std::string_view s) {
  return "\"" + std::string(s) + "\"";
}

}  // namespace

std::string ColumnPathError(const std::vector<std::string>& columns, size_t* index) {
  for (size_t i = 0; i < columns.size(); ++i) {
    const std::string& c = columns[i];
    *index = i;
    if (HasEmptySegment(c)) {
      return "@dataset column " + Quoted(c) + ": a column path has an empty segment (draft §3.4.4)";
    }
    for (size_t j = 0; j < i; ++j) {
      const std::string& prev = columns[j];
      if (prev == c) return "@dataset column " + Quoted(c) + " appears twice (draft §3.4.4)";
      if (IsPathPrefix(prev, c) || IsPathPrefix(c, prev)) {
        return "@dataset column " + Quoted(c) + " overlaps column " + Quoted(prev) +
               ": a field and a path inside it cannot both be columns (draft §3.4.4)";
      }
    }
  }
  return "";
}

std::string CheckColumnPaths(const pb::Descriptor* desc, const std::vector<std::string>& columns) {
  size_t index = 0;
  if (std::string msg = ColumnPathError(columns, &index); !msg.empty()) return msg;
  for (const std::string& c : columns) {
    const pb::Descriptor* md = desc;
    std::string_view rest = c;
    for (;;) {
      const size_t dot = rest.find('.');
      const std::string_view seg = rest.substr(0, dot);
      const pb::FieldDescriptor* fd = md->FindFieldByName(std::string(seg));
      if (fd == nullptr) {
        return "@dataset column " + Quoted(c) + ": " + std::string(md->full_name()) +
               " has no field " + Quoted(seg);
      }
      if (dot == std::string_view::npos) break;
      const std::string_view tail = rest.substr(dot + 1);
      const std::string_view at = std::string_view(c).substr(0, c.size() - tail.size() - 1);
      if (fd->is_map()) {
        return "@dataset column " + Quoted(c) + ": " + Quoted(at) +
               " is a map field, and a column path cannot go through one; use a value cell for " +
               Quoted(at) + " (draft §3.4.4)";
      }
      if (fd->is_repeated()) {
        return "@dataset column " + Quoted(c) + ": " + Quoted(at) +
               " is a repeated field, and a column path cannot go through one; use a value cell "
               "for " +
               Quoted(at) + " (draft §3.4.4)";
      }
      if (fd->cpp_type() != pb::FieldDescriptor::CPPTYPE_MESSAGE) {
        return "@dataset column " + Quoted(c) + ": " + Quoted(at) + " is not a message field";
      }
      if (IsLiteralType(fd->message_type())) {
        return "@dataset column " + Quoted(c) + ": " + Quoted(at) + " is a " +
               std::string(fd->message_type()->full_name()) +
               ", which is written as a literal; a column path cannot go into it (draft §3.4.4)";
      }
      md = fd->message_type();
      rest = tail;
    }
  }
  return "";
}

}  // namespace protowire::pxf

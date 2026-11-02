// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Mirrors protowire-go's dataset_write.go, which fixes the text and the error
// messages byte for byte.

#include "protowire/pxf/dataset_writer.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "carrier.h"
#include "dataset_paths.h"
#include "encode_internal.h"
#include "parser_internal.h"
#include "protowire/detail/go_quote.h"
#include "protowire/pxf/wellknown.h"

namespace protowire::pxf {

namespace {

namespace pb = google::protobuf;
using FieldDescriptor = pb::FieldDescriptor;
using Message = pb::Message;

// DatasetColumn is one resolved column: its name as written and the fields
// from the row type down to the leaf (one for a plain column).
struct DatasetColumn {
  std::string name;
  std::vector<const FieldDescriptor*> path;
  // Leaves of the columns with the same parent, this one included.
  std::vector<const FieldDescriptor*> siblings;

  const FieldDescriptor* leaf() const { return path.back(); }

  bool SameParent(const DatasetColumn& o) const {
    return path.size() == o.path.size() && std::equal(path.begin(), path.end() - 1, o.path.begin());
  }

  // ParentIn walks the path in m to the message holding the leaf. It returns
  // nullptr when a parent on the way is unset.
  const Message* ParentIn(const Message& m) const {
    const Message* at = &m;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
      const pb::Reflection* r = at->GetReflection();
      if (!r->HasField(*at, path[i])) return nullptr;
      at = &r->GetMessage(*at, path[i]);
    }
    return at;
  }

  // MarkPrefixes adds the dotted names of the parents: "a" and "a.b" for
  // "a.b.c".
  void MarkPrefixes(std::unordered_set<std::string>* filled) const {
    for (size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', dot + 1)) {
      filled->insert(name.substr(0, dot));
    }
  }
};

// CellOptions is what a row needs of a field's PXF annotations.
struct CellOptions {
  bool has_default = false;
  bool required = false;
};

CellOptions CellOptionsOf(const FieldDescriptor* fd) {
  FieldAnnotations a = ReadFieldAnnotations(fd);
  return {a.def.has_value(), a.required};
}

// Has is protoreflect's Message.Has: a list or map is set when it has entries.
bool Has(const Message& m, const FieldDescriptor* fd) {
  const pb::Reflection* r = m.GetReflection();
  return fd->is_repeated() ? r->FieldSize(m, fd) > 0 : r->HasField(m, fd);
}

// DatasetColumns resolves columns against desc, refusing what a v1 row cannot
// carry; see AppendDatasetHeader.
StatusOr<std::vector<DatasetColumn>> DatasetColumns(const pb::Descriptor* desc,
                                                    const std::vector<std::string>& columns) {
  if (columns.empty()) return Status::Error("pxf: @dataset needs at least one column");
  // A schema the reader would refuse is refused here instead of written: a
  // field with a reserved name makes the header unreadable, and a oneof with
  // two defaulted members would get two null cells.
  if (Status st = CheckDescriptorSchema(desc); !st.ok()) {
    return Status::Error("pxf: @dataset " + std::string(desc->full_name()) + ": " + st.message());
  }
  // The reader's rules for column paths (protowire#342), with its messages,
  // so the writer cannot write a header its reader rejects.
  if (std::string msg = CheckColumnPaths(desc, columns); !msg.empty()) {
    return Status::Error("pxf: " + msg);
  }
  std::vector<DatasetColumn> cols;
  cols.reserve(columns.size());
  for (const std::string& c : columns) {
    DatasetColumn col;
    col.name = c;
    const pb::Descriptor* md = desc;
    for (size_t start = 0;;) {
      const size_t dot = c.find('.', start);
      // CheckColumnPaths resolved every segment.
      const FieldDescriptor* fd = md->FindFieldByName(c.substr(start, dot - start));
      col.path.push_back(fd);
      if (dot == std::string::npos) break;
      md = fd->message_type();
      start = dot + 1;
    }
    cols.push_back(std::move(col));
  }
  for (DatasetColumn& c : cols) {
    for (const DatasetColumn& o : cols) {
      if (c.SameParent(o)) c.siblings.push_back(o.leaf());
    }
  }
  for (int i = 0; i < desc->field_count(); ++i) {
    const FieldDescriptor* fd = desc->field(i);
    if (!CellOptionsOf(fd).required) continue;
    const bool covered = std::any_of(
        cols.begin(), cols.end(), [fd](const DatasetColumn& c) { return c.path[0] == fd; });
    if (!covered) {
      return Status::Error("pxf: @dataset columns omit " + detail::GoQuote(fd->name()) +
                           ", which is (pxf.required): every row would be rejected");
    }
  }
  return cols;
}

// WriteCell writes the cell for fd of m, in a row of the columns cols; see
// AppendDatasetRow.
Status WriteCell(const Message& m,
                 const FieldDescriptor* fd,
                 const std::string& path,
                 const std::vector<const FieldDescriptor*>& cols,
                 CellOptions o,
                 const NullSet* null_set,
                 std::string* out) {
  const bool is_msg = fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE;
  if (null_set != nullptr && null_set->count(path) > 0 && Nullable(fd)) {
    *out += "null";
    return Status::OK();
  }
  const pb::Reflection* r = m.GetReflection();
  if (Has(m, fd)) {
    if (fd->is_repeated()) return WriteValueCell(m, fd, null_set, out);
    if (!is_msg) {
      WriteCellScalar(m, fd, out);
      return Status::OK();
    }
    const Message& sub = r->GetMessage(m, fd);
    if (!IsLiteralMessage(sub)) return WriteValueCell(m, fd, null_set, out);
    return WriteCellLiteral(sub, out);
  }
  if (!o.has_default && !o.required) return Status::OK();  // empty: absent, as it reads back
  if (!fd->has_presence()) {
    if (fd->is_repeated()) {
      // Go hands the empty list to its scalar writer here. For a map or a list
      // of messages that is this error. For a list of scalars Go panics, or
      // for strings writes the list's address, neither of which can be matched.
      return Status::Error("unsupported kind: " + std::string(fd->type_name()));
    }
    WriteCellScalar(m, fd, out);  // the zero value, explicitly
    return Status::OK();
  }
  if (o.required) return Status::Error("unset, and the field is (pxf.required)");
  if (const pb::OneofDescriptor* od = fd->real_containing_oneof(); od != nullptr) {
    const FieldDescriptor* set = r->GetOneofFieldDescriptor(m, od);
    if (set != nullptr && std::find(cols.begin(), cols.end(), set) != cols.end()) {
      // A member written in this row keeps the default off. (At most one
      // member of a oneof may carry a default, which DatasetColumns has
      // checked, so no second null can arise.)
      return Status::OK();
    }
  }
  // An unset scalar with presence is an empty cell, which reads back as its
  // default.
  if (is_msg) *out += "null";
  return Status::OK();
}

// RowPresence refuses a row that the reader would reject for a missing
// (pxf.required) field below the root, the one way a dotted column can make a
// row unreadable. A sub-message is present on reading exactly when one of the
// cells under it is non-empty (draft §3.4.4, protowire#342). So a sub-message
// that is present in msg but whose cells all come out empty reads back
// absent: that loss is accepted unless the sub-message is required, and then
// the row is refused. And a sub-message that reads back present needs each of
// its required fields as a column, or under one.
class RowPresence {
 public:
  RowPresence(const std::vector<DatasetColumn>& cols, const std::unordered_set<std::string>& filled)
      : cols_(cols), filled_(filled) {}

  Status Check(const pb::Descriptor* md, const std::string& at) const {
    for (int i = 0; i < md->field_count(); ++i) {
      const FieldDescriptor* fd = md->field(i);
      std::string path = std::string(fd->name());
      if (!at.empty()) path = at + "." + path;
      bool exact = false;
      bool prefix = false;
      Covered(path, &exact, &prefix);
      const bool required = CellOptionsOf(fd).required;
      const bool filled = filled_.count(path) > 0;
      if (prefix && !filled && required) {
        return Status::Error("pxf: @dataset columns under " + detail::GoQuote(path) +
                             " are all empty in this row, so it reads back unset, and it is "
                             "(pxf.required)");
      }
      if (!at.empty() && required && !exact && !prefix) {
        return Status::Error("pxf: @dataset " + detail::GoQuote(at) +
                             " reads back present, and its (pxf.required) field " +
                             detail::GoQuote(path) + " is not a column: the row would be rejected");
      }
      if (prefix && filled) {
        if (Status st = Check(fd->message_type(), path); !st.ok()) return st;
      }
    }
    return Status::OK();
  }

 private:
  void Covered(const std::string& path, bool* exact, bool* prefix) const {
    for (const DatasetColumn& c : cols_) {
      if (c.name == path) {
        *exact = true;
        *prefix = false;
        return;
      }
      if (c.name.size() > path.size() && c.name[path.size()] == '.' &&
          c.name.compare(0, path.size(), path) == 0) {
        *prefix = true;
      }
    }
  }

  const std::vector<DatasetColumn>& cols_;
  const std::unordered_set<std::string>& filled_;
};

}  // namespace

// CellUnset decides how a value cell writes fd, which msg does not set. Inside
// a cell, as in a row, the reader applies (pxf.default) to an absent field and
// checks (pxf.required), so an unset annotated field is written the way
// WriteCell writes one: a field without presence as its zero value (*write), a
// message-typed field as null, and an optional or oneof scalar not at all,
// which reads back as its default. A oneof member is not written when a
// sibling is set: the sibling keeps its default off. An unset required field
// with presence cannot be written. An unannotated field is not written.
Status CellUnset(const Message& msg,
                 const FieldDescriptor* fd,
                 const std::string& path,
                 bool* write,
                 bool* null) {
  *write = false;
  *null = false;
  const CellOptions o = CellOptionsOf(fd);
  if (!o.has_default && !o.required) return Status::OK();
  if (!fd->has_presence()) {
    *write = true;
    return Status::OK();
  }
  if (o.required) return Status::Error(path + " is unset, and the field is (pxf.required)");
  if (const pb::OneofDescriptor* od = fd->real_containing_oneof();
      od != nullptr && msg.GetReflection()->GetOneofFieldDescriptor(msg, od) != nullptr) {
    return Status::OK();
  }
  *null = fd->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE;
  return Status::OK();
}

Status AppendDatasetHeader(std::string* dst,
                           const pb::Descriptor* desc,
                           const std::vector<std::string>& columns) {
  auto cols = DatasetColumns(desc, columns);
  if (!cols.ok()) return cols.status();
  *dst += "@dataset ";
  dst->append(std::string_view(desc->full_name()));
  *dst += " (";
  for (size_t i = 0; i < columns.size(); ++i) {
    if (i > 0) *dst += ", ";
    *dst += columns[i];
  }
  *dst += ")\n";
  return Status::OK();
}

Status AppendDatasetRow(std::string* dst,
                        const Message& msg,
                        const std::vector<std::string>& columns) {
  const pb::Descriptor* desc = msg.GetDescriptor();
  auto cols = DatasetColumns(desc, columns);
  if (!cols.ok()) return cols.status();
  NullSet nulls;
  const NullSet* null_set = nullptr;
  if (const FieldDescriptor* nfd = FindNullMaskField(desc);
      nfd != nullptr && msg.GetReflection()->HasField(msg, nfd)) {
    for (auto& path : ReadNullPaths(msg, nfd)) nulls.insert(std::move(path));
    null_set = &nulls;
  }
  std::unordered_set<std::string> filled;  // path prefixes with a non-empty cell
  std::string row = "(";
  for (size_t i = 0; i < cols->size(); ++i) {
    const DatasetColumn& c = (*cols)[i];
    if (i > 0) row += ", ";
    const Message* parent = c.ParentIn(msg);
    if (parent == nullptr) continue;  // a parent is unset: an empty cell, and it reads back unset
    const size_t before = row.size();
    Status st =
        WriteCell(*parent, c.leaf(), c.name, c.siblings, CellOptionsOf(c.leaf()), null_set, &row);
    if (!st.ok()) {
      return Status::Error("pxf: @dataset column " + detail::GoQuote(c.name) + ": " + st.message());
    }
    if (row.size() > before) c.MarkPrefixes(&filled);
  }
  row += ")\n";
  if (Status st = RowPresence(*cols, filled).Check(desc, ""); !st.ok()) return st;
  dst->append(row);
  return Status::OK();
}

}  // namespace protowire::pxf

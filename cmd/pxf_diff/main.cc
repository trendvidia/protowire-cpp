// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// pxf_diff runs this port over a manifest of PXF documents and prints what it
// did, one section per document. tools/godiff prints the same sections for
// protowire-go, so scripts/go_differential.sh only has to diff two text
// files. The two programs have to stay line for line alike.
//
// Manifest: one document per line, as root TAB dir TAB protos TAB type TAB
// file. Paths are relative to root, protos is a comma-separated list, and type
// names the message of a document with no @type, or is "-".

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

#include "protowire/detail/go_quote.h"
#include "protowire/pxf.h"
#include "protowire/pxf/format.h"
#include "protowire/pxf/go_wire.h"

namespace {

namespace fs = std::filesystem;
namespace gpb = google::protobuf;
namespace pxf = protowire::pxf;
using protowire::Status;

class NoErrors : public gpb::compiler::MultiFileErrorCollector {
 public:
#if GOOGLE_PROTOBUF_VERSION >= 5026000
  void RecordError(absl::string_view, int, int, absl::string_view) override { failed = true; }
#else
  void AddError(const std::string&, int, int, const std::string&) override { failed = true; }
#endif
  bool failed = false;
};

class Schema : public pxf::TypeResolver {
 public:
  Schema(const std::string& root,
         const std::string& dir,
         const std::vector<std::string>& protos,
         const std::string& proto_dir) {
    tree_.MapPath("", WKT_PROTO_DIR);
    tree_.MapPath("", proto_dir);
    tree_.MapPath("", root);
    tree_.MapPath("", (fs::path(root) / dir).string());
    importer_ = std::make_unique<gpb::compiler::Importer>(&tree_, &errors_);
    for (const auto& p : protos) {
      if (importer_->Import(p) == nullptr) failed_ = true;
    }
    factory_ = std::make_unique<gpb::DynamicMessageFactory>(importer_->pool());
  }

  bool failed() const { return failed_ || errors_.failed; }

  const gpb::Descriptor* Find(const std::string& name) const {
    return importer_->pool()->FindMessageTypeByName(name);
  }

  std::unique_ptr<gpb::Message> New(const gpb::Descriptor* d) const {
    return std::unique_ptr<gpb::Message>(factory_->GetPrototype(d)->New());
  }

  const gpb::Descriptor* FindMessageByURL(std::string_view url) override {
    return Find(std::string(url.substr(url.rfind('/') + 1)));
  }

 private:
  gpb::compiler::DiskSourceTree tree_;
  NoErrors errors_;
  bool failed_ = false;
  std::unique_ptr<gpb::compiler::Importer> importer_;
  std::unique_ptr<gpb::DynamicMessageFactory> factory_;
};

std::string Quote(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

std::string Text(const Status& st) {
  if (st.ok()) return "ok";
  if (auto p = st.position()) {
    return Quote(std::to_string(p->first) + ":" + std::to_string(p->second) + ": " + st.message());
  }
  return Quote(st.message());
}

std::string Join(std::vector<std::string> v, const char* sep) {
  std::sort(v.begin(), v.end());
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::string BytesOf(const gpb::Message& m) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : pxf::SerializeGoOrder(m)) {
    out += kHex[c >> 4];
    out += kHex[c & 0xF];
  }
  return out;
}

void PrintText(const char* label, const protowire::StatusOr<std::string>& text) {
  if (!text.ok()) {
    std::cout << label << ": " << Text(text.status()) << "\n";
    return;
  }
  std::cout << label << ":\n";
  std::istringstream in(*text);
  std::string line;
  while (std::getline(in, line)) std::cout << "  | " << Quote(line) << "\n";
}

std::string TypeOf(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("@type ", 0) == 0) {
      std::string t = line.substr(6);
      while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\t')) t.pop_back();
      return t;
    }
  }
  return "";
}

void RunMessage(Schema& s, const gpb::Descriptor* d, const std::string& text) {
  pxf::UnmarshalOptions opts;
  opts.type_resolver = &s;
  auto plain = s.New(d);
  std::cout << "unmarshal: " << Text(pxf::Unmarshal(text, plain.get(), opts)) << "\n";
  {
    pxf::UnmarshalOptions hooked = opts;
    hooked.on_secret_field = [](std::string_view path, std::string_view value) {
      std::cout << "secret: " << Quote(std::string(path)) << " = " << Quote(std::string(value))
                << "\n";
      return Status::OK();
    };
    auto m = s.New(d);
    auto rr = pxf::UnmarshalFull(text, m.get(), hooked);
    std::cout << "hooked: " << Text(rr.status()) << "\n";
    if (rr.ok()) {
      std::cout << "bytes: " << BytesOf(*m) << "\n";
      std::cout << "set: " << Quote(Join(rr->SetFields(), " ")) << "\n";
    }
    pxf::UnmarshalOptions raw = opts;
    raw.skip_post_decode = true;
    auto rm = s.New(d);
    auto rs = pxf::UnmarshalFull(text, rm.get(), raw);
    std::cout << "skip-post: " << Text(rs.status()) << "\n";
    if (rs.ok()) std::cout << "bytes: " << BytesOf(*rm) << "\n";
  }
  auto full = s.New(d);
  auto r = pxf::UnmarshalFull(text, full.get(), opts);
  std::cout << "full: " << Text(r.status()) << "\n";
  if (!r.ok()) return;
  std::cout << "set: " << Quote(Join(r->SetFields(), " ")) << "\n";
  std::cout << "null: " << Quote(Join(r->NullFields(), " ")) << "\n";
  std::cout << "bytes: " << BytesOf(*full) << "\n";
  pxf::MarshalOptions m;
  m.type_resolver = &s;
  PrintText("marshal", pxf::Marshal(*full, m));
  m.emit_defaults = true;
  PrintText("marshal-defaults", pxf::Marshal(*full, m));
  m.emit_defaults = false;
  m.null_fields = &*r;
  PrintText("marshal-nulls", pxf::Marshal(*full, m));
}

// RunUnvalidated repeats the decode with bind-time validation off, where a
// schema the checks would refuse still reaches the codec.
void RunUnvalidated(Schema& s, const gpb::Descriptor* d, const std::string& text) {
  pxf::UnmarshalOptions opts;
  opts.type_resolver = &s;
  opts.skip_validate = true;
  auto plain = s.New(d);
  std::cout << "unvalidated: " << Text(pxf::Unmarshal(text, plain.get(), opts)) << "\n";
  pxf::UnmarshalOptions hooked = opts;
  hooked.on_secret_field = [](std::string_view path, std::string_view value) {
    std::cout << "secret: " << Quote(std::string(path)) << " = " << Quote(std::string(value))
              << "\n";
    return Status::OK();
  };
  auto hm = s.New(d);
  auto hr = pxf::UnmarshalFull(text, hm.get(), hooked);
  std::cout << "unvalidated-hooked: " << Text(hr.status()) << "\n";
  auto full = s.New(d);
  auto r = pxf::UnmarshalFull(text, full.get(), opts);
  std::cout << "unvalidated-full: " << Text(r.status()) << "\n";
  if (!r.ok()) return;
  std::cout << "bytes: " << BytesOf(*full) << "\n";
  pxf::MarshalOptions m;
  m.type_resolver = &s;
  PrintText("unvalidated-marshal", pxf::Marshal(*full, m));
  m.null_fields = &*r;
  PrintText("unvalidated-marshal-nulls", pxf::Marshal(*full, m));
}

class StubValidator : public protowire::check::Validator {
 public:
  explicit StubValidator(protowire::check::Report rep) : rep_(std::move(rep)) {}
  protowire::StatusOr<protowire::check::Report> Validate(const gpb::Message&) override {
    return rep_;
  }

 private:
  protowire::check::Report rep_;
};

// RunSeams covers the entry points that allocate the message and the validator
// seam.
void RunSeams(Schema& s, const gpb::Descriptor* d, const std::string& text) {
  pxf::UnmarshalOptions opts;
  opts.type_resolver = &s;
  auto m = pxf::UnmarshalDescriptor(text, d, opts);
  std::cout << "descriptor: " << Text(m.status()) << "\n";
  if (m.ok()) std::cout << "bytes: " << BytesOf(**m) << "\n";
  auto f = pxf::UnmarshalFullDescriptor(text, d, opts);
  std::cout << "descriptor-full: " << Text(f.status()) << "\n";
  if (f.ok()) {
    std::cout << "bytes: " << BytesOf(*f->message) << "\n";
    std::cout << "present: " << Quote(Join(f->result.PresentFields(), " ")) << "\n";
  }
  using protowire::check::Report;
  const std::vector<Report> reports = {
      Report{},
      Report{{{"", "", "bad"}}},
      Report{{{"a.b", "test.rule", "bad"}}},
      Report{{{"a", "", "one"}, {"", "r", "two"}}},
  };
  for (size_t i = 0; i < reports.size(); ++i) {
    StubValidator v(reports[i]);
    opts.validator = &v;
    auto vm = s.New(d);
    std::cout << "validated " << i << ": " << Text(pxf::Unmarshal(text, vm.get(), opts)) << "\n";
    auto fm = s.New(d);
    std::cout << "validated-full " << i << ": "
              << Text(pxf::UnmarshalFull(text, fm.get(), opts).status()) << "\n";
  }
}

void RunDatasets(Schema& s, const pxf::Document& doc) {
  for (const auto& ds : doc.datasets) {
    const gpb::Descriptor* d = s.Find(ds.type);
    std::vector<std::string> cols = ds.columns;
    std::string joined;
    for (size_t i = 0; i < cols.size(); ++i) joined += (i ? ", " : "") + cols[i];
    std::cout << "dataset: " << ds.type << " (" << joined << ")\n";
    if (d == nullptr) continue;
    std::string header;
    if (Status st = pxf::AppendDatasetHeader(&header, d, ds.columns); st.ok()) {
      PrintText("header", header);
    } else {
      PrintText("header", st);
    }
    pxf::UnmarshalOptions opts;
    opts.type_resolver = &s;
    for (size_t i = 0; i < ds.rows.size(); ++i) {
      auto m = s.New(d);
      Status st = pxf::BindRow(m.get(), ds.columns, ds.rows[i], opts);
      std::cout << "row " << i << ": " << Text(st) << "\n";
      if (!st.ok()) continue;
      std::cout << "bytes: " << BytesOf(*m) << "\n";
      std::string row;
      if (Status written = pxf::AppendDatasetRow(&row, *m, ds.columns); written.ok()) {
        PrintText("written", row);
      } else {
        PrintText("written", written);
      }
    }
  }
}

void Run(const std::string& root,
         const std::string& dir,
         const std::string& proto_dir,
         const std::string& protos,
         const std::string& deftype,
         const std::string& file) {
  std::cout << "=== " << file << "\n";
  std::ifstream in(fs::path(root) / file, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  auto doc = pxf::Parse(text);
  std::cout << "parse: " << Text(doc.status()) << "\n";
  if (doc.ok()) PrintText("format", pxf::FormatDocument(*doc));
  if (protos.empty()) return;
  std::vector<std::string> names;
  std::istringstream split(protos);
  std::string name;
  while (std::getline(split, name, ',')) names.push_back(name);
  Schema s(root, dir, names, proto_dir);
  if (s.failed()) {
    std::cout << "compile: failed\n";
    return;
  }
  if (doc.ok() && !doc->datasets.empty()) {
    RunDatasets(s, *doc);
    return;
  }
  std::string type_name = TypeOf(text);
  if (type_name.empty()) type_name = deftype;
  const gpb::Descriptor* d = s.Find(type_name);
  if (d == nullptr) {
    std::cout << "type: none\n";
    return;
  }
  std::cout << "type: " << d->full_name() << "\n";
  auto tolerant = pxf::ParseTolerant(text);
  for (const auto& k : pxf::KeyedDiagnostics(tolerant.document, d)) {
    std::cout << "keyed: " << pxf::KeyedErrorKindName(k.kind) << " field=" << Quote(k.field)
              << " key=" << Quote(k.key) << " " << Quote(k.ToString()) << "\n";
  }
  if (doc.ok()) {
    pxf::CanonicalizeKeyed(&*doc, d);
    PrintText("canonical", pxf::FormatDocument(*doc));
  }
  RunMessage(s, d, text);
  RunUnvalidated(s, d, text);
  RunSeams(s, d, text);
}

// AstDump writes a document's whole AST with every span, in a form the Go side
// repeats byte for byte.
class AstDump {
 public:
  explicit AstDump(std::string* out) : out_(*out) {}

  void Document(const pxf::Document& doc) {
    out_ += "type_url " + Quote(doc.type_url) + " body " + std::to_string(doc.body_offset) + "\n";
    Comments("", doc.leading_comments);
    for (const auto& x : doc.directives) {
      out_ += "directive " + Pos(x.pos) + " " + x.name + " " + x.type + " [" + Spaced(x.prefixes) +
              "] " + Quote(x.body) + " " + Bool(x.has_body) + "\n";
      Comments("", x.leading_comments);
    }
    for (const auto& x : doc.protos) {
      out_ += "proto " + Pos(x.pos) + " " + pxf::ProtoShapeName(x.shape) + " " + x.type_name + " " +
              Quote(x.body) + "\n";
      Comments("", x.leading_comments);
    }
    for (const auto& x : doc.datasets) {
      out_ += "dataset " + Pos(x.pos) + " " + x.type + " [" + Spaced(x.columns) + "]\n";
      Comments("", x.leading_comments);
      for (const auto& r : x.rows) {
        out_ += "row " + Pos(r.pos) + "\n";
        for (const auto& c : r.cells) {
          if (c.has_value()) {
            Value(" ", *c);
          } else {
            out_ += " empty\n";
          }
        }
      }
    }
    Entries("", doc.entries);
  }

 private:
  static std::string Pos(const pxf::Position& p) {
    return std::to_string(p.line) + ":" + std::to_string(p.column) + ":" + std::to_string(p.offset);
  }
  static std::string Bool(bool b) { return b ? "true" : "false"; }
  static std::string Spaced(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? " " : "") + v[i];
    return out;
  }
  void Comments(const std::string& ind, const std::vector<pxf::Comment>& cs) {
    for (const auto& c : cs) {
      out_ += ind + "comment " + Pos(c.pos) + " " + Quote(c.text) + "\n";
    }
  }
  void Kind(const pxf::StringVal& v, const std::string&) {
    out_ += "string " + Quote(v.value) + "\n";
  }
  void Kind(const pxf::IntVal& v, const std::string&) {
    out_ += "int " + v.raw + " p" + v.prec + "\n";
  }
  void Kind(const pxf::FloatVal& v, const std::string&) {
    out_ += "float " + v.raw + " p" + v.prec + "\n";
  }
  void Kind(const pxf::BoolVal& v, const std::string&) { out_ += "bool " + Bool(v.value) + "\n"; }
  void Kind(const pxf::BytesVal& v, const std::string&) {
    static constexpr char kHex[] = "0123456789abcdef";
    out_ += "bytes ";
    for (uint8_t c : v.value) {
      out_ += kHex[c >> 4];
      out_ += kHex[c & 0xF];
    }
    out_ += "\n";
  }
  void Kind(const pxf::NullVal&, const std::string&) { out_ += "null\n"; }
  void Kind(const pxf::IdentVal& v, const std::string&) { out_ += "ident " + v.name + "\n"; }
  void Kind(const pxf::TimestampVal& v, const std::string&) { out_ += "timestamp " + v.raw + "\n"; }
  void Kind(const pxf::DurationVal& v, const std::string&) { out_ += "duration " + v.raw + "\n"; }
  void Kind(const pxf::BadVal&, const std::string&) { out_ += "bad\n"; }
  void Kind(const pxf::ListVal& v, const std::string& ind) {
    out_ += "list\n";
    for (const auto& e : v.elements) Value(ind + " ", e);
  }
  void Kind(const pxf::BlockVal& v, const std::string& ind) {
    out_ += "blockval\n";
    Entries(ind + " ", v.entries);
  }
  void Value(const std::string& ind, const pxf::ValuePtr& v) {
    auto [start, end] = pxf::ValueSpan(v);
    out_ += ind + Pos(start) + "-" + Pos(end) + " ";
    std::visit([&](const auto& p) { Kind(*p, ind); }, v);
  }
  void Entry(const pxf::Assignment& e, const std::string& ind) {
    out_ += "assign " + Quote(e.key) + " " + Bool(e.key_quoted) +
            " trail=" + Quote(e.trailing_comment) + "\n";
    Comments(ind, e.leading_comments);
    Value(ind + " ", e.value);
  }
  void Entry(const pxf::MapEntry& e, const std::string& ind) {
    out_ += "map " + Quote(e.key) + " " + Bool(e.key_quoted) +
            " trail=" + Quote(e.trailing_comment) + "\n";
    Comments(ind, e.leading_comments);
    Value(ind + " ", e.value);
  }
  void Entry(const pxf::TypeEntry& e, const std::string& ind) {
    out_ += "type " + Quote(e.url) + " trail=" + Quote(e.trailing_comment) + "\n";
    Comments(ind, e.leading_comments);
  }
  void Entry(const pxf::Block& e, const std::string& ind) {
    out_ += "block " + Quote(e.name) + " " + Bool(e.name_quoted) + "\n";
    Comments(ind, e.leading_comments);
    Entries(ind + " ", e.entries);
  }
  void Entries(const std::string& ind, const std::vector<pxf::EntryPtr>& es) {
    for (const auto& e : es) {
      auto [start, end] = pxf::EntrySpan(e);
      out_ += ind + Pos(start) + "-" + Pos(end) + " ";
      std::visit([&](const auto& p) { Entry(*p, ind); }, e);
    }
  }

  std::string& out_;
};

// The same list as defaultTable in tools/godiff. The literals are a schema's,
// so they cover what strconv and package time accept and refuse.
const char* const kDefaultTable[] = {"",
                                     "0",
                                     "1",
                                     "-1",
                                     "+1",
                                     "+",
                                     "-",
                                     "007",
                                     "5x",
                                     "x",
                                     "true",
                                     "false",
                                     "True",
                                     "RED",
                                     "COLOR_UNSPECIFIED",
                                     "2147483647",
                                     "2147483648",
                                     "-2147483648",
                                     "-2147483649",
                                     "4294967295",
                                     "4294967296",
                                     "9223372036854775807",
                                     "9223372036854775808",
                                     "-9223372036854775808",
                                     "-9223372036854775809",
                                     "18446744073709551615",
                                     "18446744073709551616",
                                     "99999999999999999999999x",
                                     "1.5",
                                     "-1.5",
                                     ".5",
                                     "5.",
                                     ".",
                                     "1e3",
                                     "1E3",
                                     "1e",
                                     "1e+",
                                     "1e999",
                                     "-1e999",
                                     "1e-999",
                                     "1e39",
                                     "3.5e38",
                                     "0x10",
                                     "0x1p4",
                                     "0x1P-2",
                                     "0x",
                                     "0xp1",
                                     "1_000",
                                     "1__0",
                                     "_1",
                                     "1_",
                                     "0x_1p0",
                                     "0x1_0p0",
                                     "inf",
                                     "+inf",
                                     "-Inf",
                                     "INF",
                                     "infinity",
                                     "+Infinity",
                                     "infin",
                                     "nan",
                                     "NaN",
                                     "+nan",
                                     "-nan",
                                     "aGk=",
                                     "aGk",
                                     "a-_b",
                                     "!!",
                                     "a",
                                     "2024-01-02T03:04:05Z",
                                     "2024-01-02T3:04:05Z",
                                     "2024-01-02T03:04:05.5+01:00",
                                     "2024-13-02T03:04:05Z",
                                     "2024-02-30T03:04:05Z",
                                     "2023-02-29T00:00:00Z",
                                     "2024-00-02T03:04:05Z",
                                     "2024-01-00T03:04:05Z",
                                     "2024-01-02T24:04:05Z",
                                     "2024-01-02T03:60:05Z",
                                     "2024-01-02T03:04:60Z",
                                     "2024-01-02",
                                     "2024-01-02T",
                                     "2024-01-02T03",
                                     "2024-01-02T03:04",
                                     "2024-01-02T03:04:05",
                                     "2024-01-02T03:04:05+25:00",
                                     "2024-01-02T03:04:05+01:61",
                                     "2024-01-02T03:04:05*01:00",
                                     "2024-01-02T03:04:05+0100",
                                     "2024-01-02T03:04:05Zjunk",
                                     "2024-01-02t03:04:05Z",
                                     "2o24-01-02T03:04:05Z",
                                     "202",
                                     "2024x01",
                                     "2024-1-02T03:04:05Z",
                                     "2024-01-2T03:04:05Z",
                                     "2024-01-02 03:04:05Z",
                                     "2024-01-02T03:4:05Z",
                                     "2024-01-02T03:04:5Z",
                                     "1s",
                                     "1h30m",
                                     "1.5ms",
                                     "-1s",
                                     "+1s",
                                     "1",
                                     "5q",
                                     "1s2",
                                     ".s",
                                     "1.s",
                                     "s",
                                     "9999999999h",
                                     "2\xc2\xb5s",
                                     "2\xce\xbcs",
                                     "5\xc3\xa9",
                                     "\"q\"",
                                     "a\\b",
                                     "tab\there",
                                     "12345678901234567890",
                                     "-12345678901234567890",
                                     "1.25",
                                     "-1.25",
                                     "1.2.3",
                                     "1p53",
                                     "1.5p10",
                                     "1e5"};

// Defaults applies every literal to every field of diff.v1.Kinds.
int Defaults(const std::string& root, const std::string& dir, const std::string& proto_dir) {
  Schema s(root, dir, {"diff.proto"}, proto_dir);
  const gpb::Descriptor* d = s.Find("diff.v1.Kinds");
  if (s.failed() || d == nullptr) return 2;
  for (int i = 0; i < d->field_count(); ++i) {
    const gpb::FieldDescriptor* fd = d->field(i);
    std::vector<std::string> table(std::begin(kDefaultTable), std::end(kDefaultTable));
    table.push_back(std::string(5000, '9'));
    table.push_back("-" + std::string(5000, '9'));
    table.push_back("1." + std::string(5000, '0'));
    for (const std::string& def : table) {
      auto m = s.New(d);
      Status st = pxf::ApplyDefault(m.get(), fd, def);
      const std::string label = std::string(fd->name()) + " " + Quote(def) + ": ";
      if (st.ok()) {
        std::cout << label << "ok " << BytesOf(*m) << "\n";
      } else {
        std::cout << label << Text(st) << "\nbytes: " << BytesOf(*m) << "\n";
      }
    }
  }
  return 0;
}

// Tolerant runs Parse and ParseTolerant over text, every prefix of it and every
// text with one byte removed: the states an editor buffer goes through.
void Tolerant(const std::string& root, const std::string& file) {
  std::ifstream in(fs::path(root) / file, std::ios::binary);
  if (!in) return;
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  const auto one = [&](const char* label, size_t n, const std::string& input) {
    std::string out = "=== " + file + " " + label + " " + std::to_string(n) + "\n";
    AstDump d(&out);
    auto doc = pxf::Parse(input);
    out += "parse: " + Text(doc.status()) + "\n";
    if (doc.ok()) d.Document(*doc);
    auto t = pxf::ParseTolerant(input);
    for (const auto& e : t.errors) {
      out += "error " + std::to_string(e.pos.offset) + " " + Quote(e.ToString()) + "\n";
    }
    d.Document(t.document);
    std::cout << out;
  };
  for (size_t i = 0; i <= text.size(); ++i) one("prefix", i, text.substr(0, i));
  for (size_t i = 0; i < text.size(); ++i) {
    one("drop", i, text.substr(0, i) + text.substr(i + 1));
  }
}

}  // namespace

// QuoteRunes prints every rune through the quoter, so that the differential
// run covers all of Unicode rather than the runes a fixture happens to hold.
void QuoteRunes() {
  for (uint32_t r = 0; r <= 0x10FFFF; ++r) {
    if (r >= 0xD800 && r <= 0xDFFF) continue;
    std::string s;
    if (r < 0x80) {
      s += static_cast<char>(r);
    } else if (r < 0x800) {
      s += static_cast<char>(0xC0 | (r >> 6));
      s += static_cast<char>(0x80 | (r & 0x3F));
    } else if (r < 0x10000) {
      s += static_cast<char>(0xE0 | (r >> 12));
      s += static_cast<char>(0x80 | ((r >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (r & 0x3F));
    } else {
      s += static_cast<char>(0xF0 | (r >> 18));
      s += static_cast<char>(0x80 | ((r >> 12) & 0x3F));
      s += static_cast<char>(0x80 | ((r >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (r & 0x3F));
    }
    std::cout << r << " " << protowire::detail::GoQuote(s) << "\n";
  }
  // Every prefix of a sequence, and the forms UTF-8 forbids.
  for (const char* bad : {"\xff",
                          "\xc0\xaf",
                          "\xe0\x80\x80",
                          "\xed\xa0\x80",
                          "\xf4\x90\x80\x80",
                          "\xe2\x82",
                          "\xf0\x9f\x98",
                          "a\x80"
                          "b"}) {
    std::cout << protowire::detail::GoQuote(bad) << "\n";
  }
}

int RunRewrite(const char* dir);  // rewrite.cc

int RunStreams(const std::string& root, const std::string& dir, const std::string& proto_dir);
int RunPbBig(const std::string& file);                                // pbbig.cc
int RunSbeXML(const std::string& dir, const std::string& proto_dir);  // sbexml.cc

int main(int argc, char** argv) {
  if (argc == 5 && std::string_view(argv[1]) == "--streams") {
    return RunStreams(argv[2], argv[3], argv[4]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "--rewrite") return RunRewrite(argv[2]);
  if (argc == 3 && std::string_view(argv[1]) == "--pb-big") return RunPbBig(argv[2]);
  if (argc == 4 && std::string_view(argv[1]) == "--sbe-xml") return RunSbeXML(argv[2], argv[3]);
  if (argc == 2 && std::string_view(argv[1]) == "--quote-runes") {
    QuoteRunes();
    return 0;
  }
  if (argc == 5 && std::string_view(argv[1]) == "--defaults") {
    return Defaults(argv[2], argv[3], argv[4]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "--tolerant") {
    std::ifstream manifest(argv[2]);
    std::string line;
    while (std::getline(manifest, line)) {
      std::vector<std::string> p;
      std::istringstream cols(line);
      std::string col;
      while (std::getline(cols, col, '\t')) p.push_back(col);
      Tolerant(p[0], p[4]);
    }
    return 0;
  }
  if (argc != 3) {
    std::cerr << "usage: pxf_diff <manifest> <proto-dir>\n";
    return 2;
  }
  std::ifstream manifest(argv[1]);
  if (!manifest) {
    std::cerr << "cannot read " << argv[1] << "\n";
    return 2;
  }
  std::string line;
  while (std::getline(manifest, line)) {
    std::vector<std::string> p;
    std::istringstream cols(line);
    std::string col;
    while (std::getline(cols, col, '\t')) p.push_back(col);
    Run(p[0], p[1], argv[2], p[2], p[3], p[4]);
  }
  return 0;
}

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
  if (doc.ok()) {
    pxf::CanonicalizeKeyed(&*doc, d);
    PrintText("canonical", pxf::FormatDocument(*doc));
  }
  RunMessage(s, d, text);
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

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--quote-runes") {
    QuoteRunes();
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

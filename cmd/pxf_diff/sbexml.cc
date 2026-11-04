// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// SBE XML schema conversion over the fixtures in a directory. Line for line
// tools/godiff/sbexml.go.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>

#include "protowire/sbe.h"

namespace {

namespace gpb = google::protobuf;

class Silent : public gpb::compiler::MultiFileErrorCollector {
 public:
#if GOOGLE_PROTOBUF_VERSION >= 5026000
  void RecordError(absl::string_view, int, int, absl::string_view) override {}
#else
  void AddError(const std::string&, int, int, const std::string&) override {}
#endif
};

}  // namespace

int RunSbeXML(const std::string& dir, const std::string& proto_dir) {
  std::vector<std::string> names;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    names.push_back(e.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    const auto ends = [&name](const char* suffix) {
      const std::string s(suffix);
      return name.size() >= s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0;
    };
    if (ends(".xml")) {
      std::ifstream in(dir + "/" + name, std::ios::binary);
      std::stringstream data;
      data << in.rdbuf();
      std::cout << "=== xml " << name << "\n";
      auto out = protowire::sbe::XMLToProto(data.str());
      if (!out.ok()) {
        std::cout << "error: " << out.status().message() << "\n";
        continue;
      }
      std::cout << *out;
    } else if (ends(".proto")) {
      std::cout << "=== proto " << name << "\n";
      gpb::compiler::DiskSourceTree tree;
      tree.MapPath("", dir);
      tree.MapPath("", proto_dir);
      tree.MapPath("", WKT_PROTO_DIR);
      Silent errors;
      gpb::compiler::Importer importer(&tree, &errors);
      const gpb::FileDescriptor* fd = importer.Import(name);
      if (fd == nullptr) {
        std::cout << "compile error\n";
        continue;
      }
      auto out = protowire::sbe::ProtoToXML({fd});
      if (!out.ok()) {
        std::cout << "error: " << out.status().message() << "\n";
        continue;
      }
      std::cout << *out;
    }
  }
  return 0;
}

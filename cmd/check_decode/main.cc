// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// check_decode is the C++ port's per-port `check-decode` binary driven by
// the protowire HARDENING conformance corpus. See:
//
//   protowire/docs/HARDENING.md
//   protowire/scripts/cross_security_check.sh
//   protowire/testdata/adversarial/README.md
//
// Contract:
//
//   check_decode --format <pxf|pb|sbe|envelope> \
//                --schema <fully.qualified.MessageType> \
//                --proto  <path-to-adversarial.proto> \
//                --input  <path>
//
//   Exit 0 → input was accepted (decode succeeded)
//   Exit 1 → input was rejected (decode returned a clean error)
//   Other  → bug in the decoder (panic / abort / OOM / hang / SIGSEGV / ...)
//
// PXF and PB are wired through here; SBE and envelope return "not
// implemented" and exit 2 (configuration error). Mirrors the Go
// reference at protowire-go/scripts/check_decode/main.go.
//
// PXF tests protowire-cpp's pxf::Unmarshal against malformed text
// inputs. PB tests standard libprotobuf DynamicMessage::ParseFromArray
// — protowire-cpp's `pb` codec is struct-tag-driven and doesn't accept
// descriptor-bound inputs, so we test the descriptor-driven path here
// (which is what the spec's adversarial PB corpus exercises against).

#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include "protowire/pxf.h"

namespace {

namespace pb = google::protobuf;

// Compat shim for the MultiFileErrorCollector signature change between
// protobuf 3.x (AddError) and 4.x (RecordError). Same idea as the test
// harness's protoc_compat.h, inlined here so check_decode doesn't drag
// in the test/ include tree.
#if defined(GOOGLE_PROTOBUF_VERSION) && GOOGLE_PROTOBUF_VERSION >= 4000000
#define PROTOWIRE_RECORD_ERROR_SIGNATURE \
  void RecordError(absl::string_view filename, int line, int column, absl::string_view msg) override
#else
#define PROTOWIRE_RECORD_ERROR_SIGNATURE \
  void AddError(const std::string& filename, int line, int column, const std::string& msg) override
#endif

class CollectErrors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_RECORD_ERROR_SIGNATURE {
    if (!last.empty()) last.append("\n");
    last.append(std::string(filename));
    last.append(":");
    last.append(std::to_string(line));
    last.append(":");
    last.append(std::to_string(column));
    last.append(": ");
    last.append(std::string(msg));
  }
  std::string last;
};

std::string ReadFile(const std::string& path, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *err = "read input: cannot open " + path;
    return {};
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  if (!in.good() && !in.eof()) {
    *err = "read input: read error on " + path;
    return {};
  }
  return buf.str();
}

bool DirExists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// LoadDescriptor mirrors the Go reference: compile --proto via
// libprotoc's Importer, then find the FQN under the import pool.
// adversarial.proto imports `sbe/annotations.proto` from the spec
// repo's canonical proto root, so we add <protowire>/proto/ to the
// import path when reachable.
const pb::Descriptor* LoadDescriptor(const std::string& proto_path,
                                     const std::string& schema,
                                     pb::compiler::Importer* importer,
                                     pb::compiler::DiskSourceTree* tree,
                                     CollectErrors* errors,
                                     std::string* err) {
  namespace fs = std::filesystem;
  fs::path abs = fs::absolute(proto_path);
  fs::path dir = abs.parent_path();
  fs::path base = abs.filename();

  tree->MapPath("", dir.string());
  fs::path spec_proto = dir / ".." / ".." / "proto";
  if (DirExists(spec_proto.string())) {
    tree->MapPath("", spec_proto.lexically_normal().string());
  }
  // The adversarial corpus's `sbe/annotations.proto` imports
  // `google/protobuf/descriptor.proto`. Pin the WKT directory from
  // CMake (Protobuf_INCLUDE_DIR) so Importer can resolve it.
#ifdef WKT_PROTO_DIR
  if (DirExists(WKT_PROTO_DIR)) {
    tree->MapPath("", WKT_PROTO_DIR);
  }
#endif

  const pb::FileDescriptor* fd = importer->Import(base.string());
  if (fd == nullptr) {
    *err = "compile " + proto_path + ": " + errors->last;
    return nullptr;
  }
  const pb::Descriptor* desc = importer->pool()->FindMessageTypeByName(schema);
  if (desc == nullptr) {
    *err = "schema \"" + schema + "\" not found in " + proto_path;
    return nullptr;
  }
  return desc;
}

bool PxfDecode(std::string_view data,
               const std::string& schema,
               const std::string& proto_path,
               std::string* err) {
  if (proto_path.empty()) {
    *err = "--proto is required for format=pxf";
    return false;
  }
  pb::compiler::DiskSourceTree tree;
  CollectErrors errors;
  pb::compiler::Importer importer(&tree, &errors);
  const pb::Descriptor* desc = LoadDescriptor(proto_path, schema, &importer, &tree, &errors, err);
  if (desc == nullptr) return false;

  pb::DynamicMessageFactory factory(importer.pool());
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(desc)->New());
  auto status = protowire::pxf::Unmarshal(data, msg.get());
  if (!status.ok()) {
    *err = std::string(status.message());
    return false;
  }
  return true;
}

bool PbDecode(std::string_view data,
              const std::string& schema,
              const std::string& proto_path,
              std::string* err) {
  if (proto_path.empty()) {
    *err = "--proto is required for format=pb";
    return false;
  }
  pb::compiler::DiskSourceTree tree;
  CollectErrors errors;
  pb::compiler::Importer importer(&tree, &errors);
  const pb::Descriptor* desc = LoadDescriptor(proto_path, schema, &importer, &tree, &errors, err);
  if (desc == nullptr) return false;

  pb::DynamicMessageFactory factory(importer.pool());
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(desc)->New());
  // Descriptor-driven PB decode goes through standard libprotobuf
  // here. protowire-cpp's `pb` codec is struct-tag-based and doesn't
  // accept DynamicMessage inputs, so the cross-port harness's PB
  // hardening tier exercises libprotobuf's parser via this path. The
  // adversarial corpus's depth / length / overflow probes hit the
  // same parsing primitives any libprotobuf-based consumer would use.
  if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
    *err = "pb decode failed";
    return false;
  }
  return true;
}

[[noreturn]] void Usage(int code) {
  std::fprintf(stderr,
               "usage: check_decode --format <pxf|pb|sbe|envelope> "
               "--schema <FQN> --input <path> [--proto <path>]\n");
  std::exit(code);
}

}  // namespace

int main(int argc, char** argv) {
  std::string format, schema, proto_path, input;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto take = [&](std::string* out) {
      if (i + 1 >= argc) Usage(2);
      *out = argv[++i];
    };
    if (arg == "--format")
      take(&format);
    else if (arg == "--schema")
      take(&schema);
    else if (arg == "--proto")
      take(&proto_path);
    else if (arg == "--input")
      take(&input);
    else if (arg == "-h" || arg == "--help")
      Usage(0);
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      Usage(2);
    }
  }
  if (format.empty() || schema.empty() || input.empty()) Usage(2);

  std::string err;
  std::string data = ReadFile(input, &err);
  if (!err.empty()) {
    std::fprintf(stderr, "reject: %s\n", err.c_str());
    return 1;
  }

  bool ok = false;
  if (format == "pxf") {
    ok = PxfDecode(data, schema, proto_path, &err);
  } else if (format == "pb") {
    ok = PbDecode(data, schema, proto_path, &err);
  } else if (format == "sbe" || format == "envelope") {
    // Not implemented in this port's check_decode tier; the cross-port
    // harness will simply not have a verdict for this row (manifest
    // entries can mark per-port skips).
    std::fprintf(
        stderr, "reject: format=%s not yet implemented in C++ check_decode\n", format.c_str());
    return 2;
  } else {
    std::fprintf(stderr, "reject: unsupported format: %s\n", format.c_str());
    return 2;
  }

  if (!ok) {
    std::fprintf(stderr, "reject: %s\n", err.c_str());
    return 1;
  }
  return 0;
}

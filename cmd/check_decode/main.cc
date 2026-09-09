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
//                --input  <path> \
//                [--limit NAME=VALUE ...]
//
//   Exit 0 → input was accepted (decode succeeded)
//   Exit 1 → input was rejected (decode returned a clean error)
//   Other  → bug in the decoder (panic / abort / OOM / hang / SIGSEGV / ...)
//
// Mirrors the Go reference at protowire-go/scripts/check_decode/main.go:
// PXF and SBE bind the message from --proto, compiled at runtime; PB goes
// through this port's struct-driven `pb` codec with hand-mirrored types
// for adversarial.proto, as the Go reference does with its tagged structs
// (the codec reads PROTOWIRE_FIELDS, not descriptors). Envelope is not
// implemented in the reference either and is reported as a rejection.
//
// --limit NAME=VALUE lowers one HARDENING limit for this run —
// MaxMessageSize, MaxNestingDepth, MaxNumericLiteralDigits,
// MaxBytesLiteralLength, MaxRepeatedCount — through the package's per-call
// options, so the corpus can prove a 64 MiB cap with a 2 KiB fixture
// (protowire#299). Until protowire-cpp#26 the SBE leg exited 2, which the
// harness reads as a crash, and the PXF decoder had no depth cap at all;
// see CHANGELOG.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include "protowire/pb.h"
#include "protowire/pb_big.h"
#include "protowire/pxf.h"
#include "protowire/sbe.h"

namespace {

namespace pb = google::protobuf;

// Limits collected from --limit NAME=VALUE; 0 means the package default.
struct Limits {
  int max_message_size = 0;
  int max_nesting_depth = 0;
  int max_numeric_literal_digits = 0;
  int max_bytes_literal_length = 0;
  int max_repeated_count = 0;

  // Set parses one NAME=VALUE; returns false with *err set when the name
  // is unknown or the value is not a positive integer.
  bool Set(std::string_view kv, std::string* err) {
    size_t eq = kv.find('=');
    if (eq == std::string_view::npos) {
      *err = "--limit wants NAME=VALUE, got \"" + std::string(kv) + "\"";
      return false;
    }
    std::string_view name = kv.substr(0, eq);
    std::string value(kv.substr(eq + 1));
    int* slot = nullptr;
    if (name == "MaxMessageSize") slot = &max_message_size;
    if (name == "MaxNestingDepth") slot = &max_nesting_depth;
    if (name == "MaxNumericLiteralDigits") slot = &max_numeric_literal_digits;
    if (name == "MaxBytesLiteralLength") slot = &max_bytes_literal_length;
    if (name == "MaxRepeatedCount") slot = &max_repeated_count;
    if (slot == nullptr) {
      *err = "--limit: unknown limit \"" + std::string(name) + "\"";
      return false;
    }
    char* end = nullptr;
    long n = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != 0 || n <= 0 || n > 2147483647L) {
      *err = "--limit " + std::string(name) + ": want a positive integer, got \"" + value + "\"";
      return false;
    }
    *slot = static_cast<int>(n);
    return true;
  }
};

// Hand-mirrored C++ types for adversarial.proto. protowire-cpp's
// pb::Unmarshal reads PROTOWIRE_FIELDS via templates — it does not consume
// descriptors — so the PB path needs concrete types here, as the Go
// reference's tagged structs do. Drift between these and the .proto is
// caught by the conformance run itself: a missing or wrong number fails
// the manifest's accept/reject expectations.
struct Tree {
  std::unique_ptr<Tree> child;
  std::string label;
  std::vector<Tree> children;
  PROTOWIRE_FIELDS(Tree,
                   PROTOWIRE_FIELD(1, child),
                   PROTOWIRE_FIELD(2, label),
                   PROTOWIRE_FIELD(3, children))
};

struct StringHolder {
  std::string value;
  PROTOWIRE_FIELDS(StringHolder, PROTOWIRE_FIELD(1, value))
};

struct BytesHolder {
  std::vector<uint8_t> value;
  PROTOWIRE_FIELDS(BytesHolder, PROTOWIRE_FIELD(1, value))
};

struct BigIntHolder {
  int64_t value = 0;
  PROTOWIRE_FIELDS(BigIntHolder, PROTOWIRE_FIELD(1, value))
};

struct ListHolder {
  std::vector<int32_t> values;
  PROTOWIRE_FIELDS(ListHolder, PROTOWIRE_FIELD(1, values))
};

// BigNumHolder mirrors adversarial.v1.BigNumHolder (protowire#279): the
// arbitrary-precision carriers.
struct BigNumHolder {
  protowire::pb::BigInt big_int;
  protowire::pb::Decimal decimal;
  protowire::pb::BigFloat big_float;
  PROTOWIRE_FIELDS(BigNumHolder,
                   PROTOWIRE_FIELD(1, big_int),
                   PROTOWIRE_FIELD(2, decimal),
                   PROTOWIRE_FIELD(3, big_float))
};

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

// SanitizeUserPath canonicalizes a user-supplied path (resolving `..`
// segments and symlinks) and confirms it points to a regular file.
// The conformance CLI accepts any path the harness names by design,
// so this is a shape check — not an access-control check. It turns
// "open a directory and fail with a confusing errno" into a clean
// reject the harness can parse.
bool SanitizeUserPath(const std::string& path, std::string* sanitized, std::string* err) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path canonical = fs::canonical(fs::path(path), ec);
  if (ec) {
    *err = "resolve path: " + ec.message() + ": " + path;
    return false;
  }
  if (!fs::is_regular_file(canonical, ec)) {
    *err = "not a regular file: " + path;
    return false;
  }
  *sanitized = canonical.string();
  return true;
}

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
  std::error_code ec;
  return std::filesystem::is_directory(path, ec);
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
               const Limits& lim,
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
  protowire::pxf::UnmarshalOptions opts;
  opts.max_message_size = lim.max_message_size;
  opts.max_nesting_depth = lim.max_nesting_depth;
  opts.max_numeric_literal_digits = lim.max_numeric_literal_digits;
  opts.max_bytes_literal_length = lim.max_bytes_literal_length;
  opts.max_repeated_count = lim.max_repeated_count;
  auto status = protowire::pxf::Unmarshal(data, msg.get(), opts);
  if (!status.ok()) {
    *err = std::string(status.message());
    return false;
  }
  return true;
}

template <class T>
bool PbDecodeAs(std::string_view data, const Limits& lim, std::string* err) {
  T msg;
  protowire::pb::UnmarshalOptions opts;
  opts.max_message_size = lim.max_message_size;
  opts.max_nesting_depth = lim.max_nesting_depth;
  opts.max_numeric_literal_digits = lim.max_numeric_literal_digits;
  opts.max_repeated_count = lim.max_repeated_count;
  std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  auto status = protowire::pb::Unmarshal(bytes, msg, opts);
  if (!status.ok()) {
    *err = std::string(status.message());
    return false;
  }
  return true;
}

bool PbDecode(std::string_view data,
              const std::string& schema,
              const Limits& lim,
              std::string* err) {
  if (schema == "adversarial.v1.Tree") return PbDecodeAs<Tree>(data, lim, err);
  if (schema == "adversarial.v1.StringHolder") return PbDecodeAs<StringHolder>(data, lim, err);
  if (schema == "adversarial.v1.BytesHolder") return PbDecodeAs<BytesHolder>(data, lim, err);
  if (schema == "adversarial.v1.BigIntHolder") return PbDecodeAs<BigIntHolder>(data, lim, err);
  if (schema == "adversarial.v1.ListHolder") return PbDecodeAs<ListHolder>(data, lim, err);
  if (schema == "adversarial.v1.BigNumHolder") return PbDecodeAs<BigNumHolder>(data, lim, err);
  *err = "unknown schema for pb: " + schema;
  return false;
}

// SbeDecode binds the codec from the same runtime-compiled proto the PXF
// path uses (adversarial.proto carries the sbe options), so the corpus's
// SBE rows are decoded rather than refused for want of a codec.
bool SbeDecode(std::string_view data,
               const std::string& schema,
               const std::string& proto_path,
               const Limits& lim,
               std::string* err) {
  if (proto_path.empty()) {
    *err = "--proto is required for format=sbe";
    return false;
  }
  pb::compiler::DiskSourceTree tree;
  CollectErrors errors;
  pb::compiler::Importer importer(&tree, &errors);
  const pb::Descriptor* desc = LoadDescriptor(proto_path, schema, &importer, &tree, &errors, err);
  if (desc == nullptr) return false;

  protowire::sbe::CodecOptions opts;
  opts.max_message_size = lim.max_message_size;
  opts.max_repeated_count = lim.max_repeated_count;
  auto codec = protowire::sbe::Codec::New({desc->file()}, opts);
  if (!codec.ok()) {
    *err = std::string(codec.status().message());
    return false;
  }
  pb::DynamicMessageFactory factory(importer.pool());
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(desc)->New());
  std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  auto status = codec->Unmarshal(bytes, msg.get());
  if (!status.ok()) {
    *err = std::string(status.message());
    return false;
  }
  return true;
}

[[noreturn]] void Usage(int code) {
  std::fprintf(stderr,
               "usage: check_decode --format <pxf|pb|sbe|envelope> "
               "--schema <FQN> --input <path> [--proto <path>] [--limit NAME=VALUE ...]\n");
  std::exit(code);
}

}  // namespace

int main(int argc, char** argv) {
  std::string format, schema, proto_path, input;
  Limits lim;
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
    else if (arg == "--limit") {
      std::string kv;
      take(&kv);
      std::string err;
      if (!lim.Set(kv, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        Usage(2);
      }
    } else if (arg == "-h" || arg == "--help")
      Usage(0);
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      Usage(2);
    }
  }
  if (format.empty() || schema.empty() || input.empty()) Usage(2);

  std::string err;
  std::string safe_input;
  if (!SanitizeUserPath(input, &safe_input, &err)) {
    std::fprintf(stderr, "reject: %s\n", err.c_str());
    return 1;
  }
  std::string data = ReadFile(safe_input, &err);
  if (!err.empty()) {
    std::fprintf(stderr, "reject: %s\n", err.c_str());
    return 1;
  }

  bool ok = false;
  if (format == "pxf") {
    ok = PxfDecode(data, schema, proto_path, lim, &err);
  } else if (format == "pb") {
    ok = PbDecode(data, schema, lim, &err);
  } else if (format == "sbe") {
    ok = SbeDecode(data, schema, proto_path, lim, &err);
  } else if (format == "envelope") {
    // As in the Go reference: not implemented, reported as a rejection.
    err = "envelope decode not yet implemented in this port";
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

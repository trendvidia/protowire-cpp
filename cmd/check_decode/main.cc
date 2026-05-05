// check_decode is the C++ reference for the per-port `check-decode` binary
// driven by the protowire HARDENING.md conformance corpus. See:
//
//   protowire/docs/HARDENING.md
//   protowire/scripts/cross_security_check.sh
//   protowire/testdata/adversarial/README.md
//
// Contract:
//
//   check_decode --format <pxf|pb|sbe|envelope>
//                --schema <fully.qualified.MessageType>
//                --proto  <path-to-adversarial.proto>
//                --input  <path>
//
//   Exit 0 → input was accepted
//   Exit 1 → input was rejected (clean error)
//   Other  → bug in the decoder (panic / abort / OOM / hang / SIGSEGV / ...)
//
// The C++ port reads the sibling `<stem>.binpb` (FileDescriptorSet) of
// `--proto` because Google's protobuf C++ runtime cannot compile `.proto`
// text at runtime. The corpus generator emits both files together.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include "protowire/pb.h"
#include "protowire/pxf.h"

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "reject: read %s: %s\n", path.c_str(),
                 std::strerror(errno));
    std::exit(1);
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

// Hand-mirrored types for adversarial.proto. protowire::pb::Unmarshal is a
// template that requires PROTOWIRE_FIELDS — there is no descriptor-driven
// dynamic dispatch — so the four schemas are re-encoded here. Drift between
// this file and adversarial.proto must be caught by the conformance run
// itself: a wrong field number flips the manifest's accept/reject expectation.

struct Tree {
  std::unique_ptr<Tree> child;
  std::string label;

  PROTOWIRE_FIELDS(Tree, PROTOWIRE_FIELD(1, child), PROTOWIRE_FIELD(2, label))
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

const google::protobuf::Descriptor* LoadDescriptor(
    const std::string& proto_path,
    google::protobuf::DescriptorPool* pool,
    const std::string& schema) {
  // Resolve sibling .binpb (FileDescriptorSet).
  std::string fds_path = proto_path;
  auto dot = fds_path.find_last_of('.');
  if (dot == std::string::npos) {
    std::fprintf(stderr, "reject: --proto has no extension: %s\n",
                 proto_path.c_str());
    std::exit(1);
  }
  fds_path.replace(dot, std::string::npos, ".binpb");

  std::string fds_bytes = ReadFile(fds_path);
  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromString(fds_bytes)) {
    std::fprintf(stderr, "reject: parse FileDescriptorSet %s\n",
                 fds_path.c_str());
    std::exit(1);
  }
  for (int i = 0; i < fds.file_size(); ++i) {
    if (pool->BuildFile(fds.file(i)) == nullptr) {
      std::fprintf(stderr, "reject: BuildFile failed for %s\n",
                   fds.file(i).name().c_str());
      std::exit(1);
    }
  }
  const auto* desc = pool->FindMessageTypeByName(schema);
  if (desc == nullptr) {
    std::fprintf(stderr, "reject: schema %s not in %s\n", schema.c_str(),
                 fds_path.c_str());
    std::exit(1);
  }
  return desc;
}

int PxfDecode(const std::string& input, const std::string& schema,
              const std::string& proto) {
  google::protobuf::DescriptorPool pool;
  const auto* desc = LoadDescriptor(proto, &pool, schema);
  google::protobuf::DynamicMessageFactory factory(&pool);
  std::unique_ptr<google::protobuf::Message> msg(factory.GetPrototype(desc)->New());

  std::string text = ReadFile(input);
  auto status = protowire::pxf::Unmarshal(text, msg.get());
  if (!status.ok()) {
    std::fprintf(stderr, "reject: pxf: %s\n", status.message().c_str());
    return 1;
  }
  return 0;
}

template <class T>
int PbDecodeAs(const std::string& input) {
  std::string raw = ReadFile(input);
  std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(raw.data()),
                                 raw.size());
  T value;
  auto status = protowire::pb::Unmarshal(bytes, value);
  if (!status.ok()) {
    std::fprintf(stderr, "reject: pb: %s\n", status.message().c_str());
    return 1;
  }
  return 0;
}

int PbDecode(const std::string& input, const std::string& schema) {
  if (schema == "adversarial.v1.Tree")          return PbDecodeAs<Tree>(input);
  if (schema == "adversarial.v1.StringHolder")  return PbDecodeAs<StringHolder>(input);
  if (schema == "adversarial.v1.BytesHolder")   return PbDecodeAs<BytesHolder>(input);
  if (schema == "adversarial.v1.BigIntHolder")  return PbDecodeAs<BigIntHolder>(input);
  std::fprintf(stderr, "reject: unknown schema for pb: %s\n", schema.c_str());
  return 1;
}

void Usage() {
  std::fprintf(stderr,
               "usage: check_decode --format <pxf|pb|sbe|envelope> "
               "--schema <full.name> --proto <path> --input <path>\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string format, schema, proto, input;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string_view k = argv[i];
    std::string_view v = argv[i + 1];
    if (k == "--format") format = v;
    else if (k == "--schema") schema = v;
    else if (k == "--proto") proto = v;
    else if (k == "--input") input = v;
    else { Usage(); return 2; }
  }
  if (format.empty() || schema.empty() || input.empty()) {
    Usage();
    return 2;
  }

  if (format == "pxf") {
    if (proto.empty()) {
      std::fprintf(stderr, "reject: --proto required for format=pxf\n");
      return 1;
    }
    return PxfDecode(input, schema, proto);
  }
  if (format == "pb") {
    return PbDecode(input, schema);
  }
  if (format == "envelope" || format == "sbe") {
    std::fprintf(stderr,
                 "reject: %s decode not yet implemented in this reference\n",
                 format.c_str());
    return 1;
  }
  std::fprintf(stderr, "reject: unsupported format: %s\n", format.c_str());
  return 1;
}

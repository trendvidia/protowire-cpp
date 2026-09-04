// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Cross-port wire-compatibility dumper, driven by protowire's
// scripts/cross_envelope_check.sh. Every port carries the same program and
// the script compares their output byte for byte. Mirrors
// protowire-go/scripts/dump_envelope.
//
//   dump_envelope                        canonical Envelope → pb hex
//   dump_envelope --pb  FDS MESSAGE DOC  PXF DOC decoded against MESSAGE in FDS → pb hex
//   dump_envelope --sbe FDS MESSAGE DOC  same → SBE hex
//
// The fixture modes apply the PXF annotations the descriptor carries, which
// is how the gate proves this port reads (pxf.required) = 1314,
// (pxf.default) = 1315 and the SBE numbers 1319–1323 from a descriptor it did
// not compile itself (STABILITY.md promise 3, protowire#244). A port looking
// for the wrong number decodes to different bytes, or accepts a document it
// must reject.
//
// Exit 0 with hex on stdout; 1 with "reject: <reason>" on stderr when the
// schema rejects DOC; 2 for anything that is the harness's fault.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include "protowire/envelope.h"
#include "protowire/pb.h"
#include "protowire/pxf.h"
#include "protowire/sbe.h"

namespace {

[[noreturn]] void Fatal(int code, const std::string& msg) {
  std::fprintf(stderr, "dump_envelope: %s\n", msg.c_str());
  std::exit(code);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) Fatal(2, path + ": cannot read");
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

void PrintHex(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) std::printf("%02x", p[i]);
  std::printf("\n");
}

int DumpEnvelope() {
  using protowire::envelope::Envelope;

  Envelope e = Envelope::Err(402, "INSUFFICIENT_FUNDS", "balance too low", {"$3.50", "$10.00"});
  e.data = {0xDE, 0xAD, 0xBE, 0xEF};
  e.error->WithField("amount", "MIN_VALUE", "below minimum", {"10.00"})
      .WithMeta("request_id", "req-123");

  auto bytes = protowire::pb::Marshal(e);
  PrintHex(bytes.data(), bytes.size());
  return 0;
}

int DumpFixture(const char* mode, const char* fds_path, const char* message, const char* doc_path) {
  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromString(ReadFile(fds_path))) {
    Fatal(2, std::string(fds_path) + ": not a FileDescriptorSet");
  }
  google::protobuf::DescriptorPool pool;
  for (int i = 0; i < fds.file_size(); ++i) {
    if (pool.BuildFile(fds.file(i)) == nullptr) {
      Fatal(2, std::string(fds_path) + ": BuildFile failed for " + fds.file(i).name());
    }
  }
  const google::protobuf::Descriptor* md = pool.FindMessageTypeByName(message);
  if (md == nullptr) Fatal(2, std::string(fds_path) + ": " + message + " not found");
  const std::string doc = ReadFile(doc_path);

  google::protobuf::DynamicMessageFactory factory(&pool);
  std::unique_ptr<google::protobuf::Message> msg(factory.GetPrototype(md)->New());

  // The full decode is the one that validates (pxf.required) and applies
  // (pxf.default); plain Unmarshal leaves both to the caller.
  auto decoded = protowire::pxf::UnmarshalFull(doc, msg.get());
  if (!decoded.ok()) {
    std::fprintf(stderr, "reject: %s\n", decoded.status().message().c_str());
    return 1;
  }

  if (std::strcmp(mode, "--pb") == 0) {
    std::string out;
    if (!msg->SerializeToString(&out)) Fatal(2, "pb serialization failed");
    PrintHex(reinterpret_cast<const uint8_t*>(out.data()), out.size());
    return 0;
  }

  auto codec_or = protowire::sbe::Codec::New({md->file()});
  if (!codec_or.ok()) Fatal(2, codec_or.status().message());
  auto codec = std::move(codec_or).consume();
  auto bytes_or = codec.Marshal(*msg);
  if (!bytes_or.ok()) Fatal(2, bytes_or.status().message());
  auto bytes = std::move(bytes_or).consume();
  PrintHex(bytes.data(), bytes.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 1) return DumpEnvelope();
  if (argc == 5 && (std::strcmp(argv[1], "--pb") == 0 || std::strcmp(argv[1], "--sbe") == 0)) {
    return DumpFixture(argv[1], argv[2], argv[3], argv[4]);
  }
  Fatal(2, "usage: dump_envelope [--pb|--sbe FDS MESSAGE DOC]");
}

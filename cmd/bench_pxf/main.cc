// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Cross-port PXF microbench: C++ implementation.
//
// Reads `<testdata>/bench-test.binpb` (FileDescriptorSet) and
// `<testdata>/bench-test.pxf` (text payload), times unmarshal +
// marshal of `bench.v1.Config` for at least `--seconds` (default 3),
// and prints one JSON line per op. The other ports' bench-pxf
// binaries print the same shape; the
// `protowire/scripts/cross_pxf_bench.sh` runner aggregates them.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include "protowire/pxf.h"

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "bench-pxf: failed to open %s\n", path.c_str());
    std::exit(1);
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

const google::protobuf::Descriptor* LoadConfigDescriptor(const std::string& fds_bytes,
                                                         google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromString(fds_bytes)) {
    std::fprintf(stderr, "bench-pxf: failed to parse FileDescriptorSet\n");
    std::exit(1);
  }
  for (int i = 0; i < fds.file_size(); ++i) {
    if (pool->BuildFile(fds.file(i)) == nullptr) {
      std::fprintf(stderr, "bench-pxf: BuildFile failed for %s\n", fds.file(i).name().c_str());
      std::exit(1);
    }
  }
  const auto* desc = pool->FindMessageTypeByName("bench.v1.Config");
  if (desc == nullptr) {
    std::fprintf(stderr, "bench-pxf: bench.v1.Config not found\n");
    std::exit(1);
  }
  return desc;
}

template <typename Fn>
std::pair<long long, std::chrono::nanoseconds> TimeLoop(std::chrono::nanoseconds target, Fn&& fn) {
  using clock = std::chrono::steady_clock;
  auto start = clock::now();
  auto deadline = start + target;
  long long iters = 0;
  for (;;) {
    for (int i = 0; i < 64; ++i) fn();
    iters += 64;
    if (clock::now() >= deadline) break;
  }
  return {iters, clock::now() - start};
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 3.0;
  std::string testdata = "testdata";
  for (int i = 1; i < argc; ++i) {
    std::string_view a(argv[i]);
    if (a == "--seconds" && i + 1 < argc) {
      seconds = std::strtod(argv[++i], nullptr);
    } else if (a == "--testdata" && i + 1 < argc) {
      testdata = argv[++i];
    } else {
      std::fprintf(stderr, "bench-pxf: unknown arg %s\n", argv[i]);
      return 2;
    }
  }

  std::string fds_bytes = ReadFile(testdata + "/bench-test.binpb");
  std::string pxf_text = ReadFile(testdata + "/bench-test.pxf");

  google::protobuf::DescriptorPool pool;
  const auto* desc = LoadConfigDescriptor(fds_bytes, &pool);
  google::protobuf::DynamicMessageFactory factory(&pool);

  auto target = std::chrono::nanoseconds(static_cast<long long>(seconds * 1e9));

  auto fresh_msg = [&]() {
    return std::unique_ptr<google::protobuf::Message>(factory.GetPrototype(desc)->New());
  };

  // Warm-up.
  {
    auto m = fresh_msg();
    auto st = protowire::pxf::Unmarshal(pxf_text, m.get());
    if (!st.ok()) {
      std::fprintf(stderr, "bench-pxf: warm-up unmarshal: %s\n", st.message().c_str());
      return 1;
    }
  }

  auto [u_iters, u_elapsed] = TimeLoop(target, [&]() {
    auto m = fresh_msg();
    auto st = protowire::pxf::Unmarshal(pxf_text, m.get());
    if (!st.ok()) std::abort();
  });
  long long u_ns_per_op = u_elapsed.count() / u_iters;
  double u_seconds = u_elapsed.count() / 1e9;
  double u_mib_per_sec =
      (static_cast<double>(pxf_text.size()) * u_iters) / (1024.0 * 1024.0) / u_seconds;
  std::printf(
      "{\"port\":\"cpp\",\"op\":\"unmarshal\",\"ns_per_op\":%lld,"
      "\"mib_per_sec\":%g,\"iterations\":%lld,\"bytes\":%zu}\n",
      u_ns_per_op,
      u_mib_per_sec,
      u_iters,
      pxf_text.size());

  // Marshal: prep one populated message.
  auto seed = fresh_msg();
  auto seed_st = protowire::pxf::Unmarshal(pxf_text, seed.get());
  if (!seed_st.ok()) {
    std::fprintf(stderr, "bench-pxf: seed unmarshal: %s\n", seed_st.message().c_str());
    return 1;
  }

  auto [m_iters, m_elapsed] = TimeLoop(target, [&]() {
    auto out = protowire::pxf::Marshal(*seed);
    if (!out.ok()) std::abort();
  });
  long long m_ns_per_op = m_elapsed.count() / m_iters;
  std::printf(
      "{\"port\":\"cpp\",\"op\":\"marshal\",\"ns_per_op\":%lld,"
      "\"iterations\":%lld}\n",
      m_ns_per_op,
      m_iters);

  return 0;
}

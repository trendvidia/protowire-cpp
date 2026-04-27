// Cross-port SBE microbench: C++ implementation.
//
// Loads `<testdata>/sbe-bench.binpb` (FileDescriptorSet), populates a
// canonical `bench.v1.Order` (10 scalars + 2-entry Fill group), and
// times Codec::Marshal + Codec::Unmarshal for at least `--seconds`
// (default 3). Prints one JSON line per op.

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
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include "protowire/sbe.h"

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "bench-sbe: failed to open %s\n", path.c_str());
    std::exit(1);
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

const google::protobuf::FileDescriptor* LoadFile(
    const std::string& fds_bytes,
    google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromString(fds_bytes)) {
    std::fprintf(stderr, "bench-sbe: failed to parse FileDescriptorSet\n");
    std::exit(1);
  }
  const google::protobuf::FileDescriptor* bench_file = nullptr;
  for (int i = 0; i < fds.file_size(); ++i) {
    const auto* fd = pool->BuildFile(fds.file(i));
    if (fd == nullptr) {
      std::fprintf(stderr, "bench-sbe: BuildFile failed for %s\n",
                   fds.file(i).name().c_str());
      std::exit(1);
    }
    if (fds.file(i).name() == "sbe-bench.proto") bench_file = fd;
  }
  if (bench_file == nullptr) {
    std::fprintf(stderr, "bench-sbe: sbe-bench.proto not in FDS\n");
    std::exit(1);
  }
  return bench_file;
}

template <typename Fn>
std::pair<long long, std::chrono::nanoseconds> TimeLoop(
    std::chrono::nanoseconds target, Fn&& fn) {
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
      std::fprintf(stderr, "bench-sbe: unknown arg %s\n", argv[i]);
      return 2;
    }
  }

  std::string fds_bytes = ReadFile(testdata + "/sbe-bench.binpb");

  google::protobuf::DescriptorPool pool;
  const auto* bench_file = LoadFile(fds_bytes, &pool);
  google::protobuf::DynamicMessageFactory factory(&pool);

  auto codec_or = protowire::sbe::Codec::New({bench_file});
  if (!codec_or.ok()) {
    std::fprintf(stderr, "bench-sbe: codec: %s\n",
                 codec_or.status().message().c_str());
    return 1;
  }
  auto codec = std::move(codec_or).consume();

  const auto* order_desc = pool.FindMessageTypeByName("bench.v1.Order");
  const auto* fill_desc = pool.FindMessageTypeByName("bench.v1.Order.Fill");
  if (order_desc == nullptr || fill_desc == nullptr) {
    std::fprintf(stderr, "bench-sbe: Order/Fill not found\n");
    return 1;
  }

  auto fresh_order = [&]() {
    return std::unique_ptr<google::protobuf::Message>(
        factory.GetPrototype(order_desc)->New());
  };

  auto order = fresh_order();
  {
    using google::protobuf::Reflection;
    const Reflection* r = order->GetReflection();
    auto set = [&](const char* name, auto fn) {
      const auto* fd = order_desc->FindFieldByName(name);
      fn(r, fd);
    };
    set("order_id", [&](const Reflection* r, const auto* fd) {
      r->SetUInt64(order.get(), fd, 1001);
    });
    set("symbol", [&](const Reflection* r, const auto* fd) {
      r->SetString(order.get(), fd, "AAPL");
    });
    set("price", [&](const Reflection* r, const auto* fd) {
      r->SetInt64(order.get(), fd, 19150);
    });
    set("quantity", [&](const Reflection* r, const auto* fd) {
      r->SetUInt32(order.get(), fd, 100);
    });
    set("side", [&](const Reflection* r, const auto* fd) {
      r->SetEnumValue(order.get(), fd, 1);  // SIDE_SELL
    });
    set("active", [&](const Reflection* r, const auto* fd) {
      r->SetBool(order.get(), fd, true);
    });
    set("weight", [&](const Reflection* r, const auto* fd) {
      r->SetDouble(order.get(), fd, 0.85);
    });
    set("score", [&](const Reflection* r, const auto* fd) {
      r->SetFloat(order.get(), fd, 2.5f);
    });

    const auto* fills_fd = order_desc->FindFieldByName("fills");
    struct Fill {
      int64_t price;
      uint32_t qty;
      uint64_t id;
    };
    for (const Fill& f : {Fill{19155, 25, 5001}, Fill{19160, 50, 5002}}) {
      auto* fill = r->AddMessage(order.get(), fills_fd, &factory);
      const auto* fr = fill->GetReflection();
      fr->SetInt64(fill, fill_desc->FindFieldByName("fill_price"), f.price);
      fr->SetUInt32(fill, fill_desc->FindFieldByName("fill_qty"), f.qty);
      fr->SetUInt64(fill, fill_desc->FindFieldByName("fill_id"), f.id);
    }
  }

  auto target = std::chrono::nanoseconds(
      static_cast<long long>(seconds * 1e9));

  // Warm-up + capture wire size.
  auto wire_or = codec.Marshal(*order);
  if (!wire_or.ok()) {
    std::fprintf(stderr, "bench-sbe: warm-up marshal: %s\n",
                 wire_or.status().message().c_str());
    return 1;
  }
  auto wire_vec = std::move(wire_or).consume();
  std::vector<uint8_t> wire(wire_vec.begin(), wire_vec.end());
  size_t n = wire.size();

  auto [m_iters, m_elapsed] = TimeLoop(target, [&]() {
    auto out = codec.Marshal(*order);
    if (!out.ok()) std::abort();
  });
  long long m_ns_per_op = m_elapsed.count() / m_iters;
  std::printf(
      "{\"port\":\"cpp\",\"op\":\"sbe-marshal\",\"ns_per_op\":%lld,"
      "\"iterations\":%lld,\"bytes\":%zu}\n",
      m_ns_per_op, m_iters, n);

  std::span<const uint8_t> wire_span(wire.data(), wire.size());
  auto [u_iters, u_elapsed] = TimeLoop(target, [&]() {
    auto out = fresh_order();
    auto st = codec.Unmarshal(wire_span, out.get());
    if (!st.ok()) std::abort();
  });
  long long u_ns_per_op = u_elapsed.count() / u_iters;
  double u_secs = u_elapsed.count() / 1e9;
  double u_mib = (static_cast<double>(n) * u_iters) / (1024.0 * 1024.0) / u_secs;
  std::printf(
      "{\"port\":\"cpp\",\"op\":\"sbe-unmarshal\",\"ns_per_op\":%lld,"
      "\"mib_per_sec\":%g,\"iterations\":%lld,\"bytes\":%zu}\n",
      u_ns_per_op, u_mib, u_iters, n);

  return 0;
}

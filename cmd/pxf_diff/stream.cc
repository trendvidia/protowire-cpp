// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// The pb and sbe stream decoders over truncated and corrupted streams. Line for
// line tools/godiff/stream.go.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

#include "protowire/pb.h"
#include "protowire/pxf.h"
#include "protowire/sbe.h"

namespace {

namespace gpb = google::protobuf;
namespace pb = protowire::pb;
namespace pxf = protowire::pxf;
namespace sbe = protowire::sbe;
using protowire::Status;

// Enough to pass a stream's frames and see its error twice.
constexpr int kStreamDecodes = 6;

struct StreamMsg {
  // Named as Go's are: a decode error names the member.
  int64_t N = 0;
  std::string Text;
  std::vector<uint8_t> Data;
  PROTOWIRE_FIELDS(StreamMsg,
                   PROTOWIRE_FIELD(1, N),
                   PROTOWIRE_FIELD(2, Text),
                   PROTOWIRE_FIELD(3, Data))
};

class NoErrors : public gpb::compiler::MultiFileErrorCollector {
 public:
#if GOOGLE_PROTOBUF_VERSION >= 5026000
  void RecordError(absl::string_view, int, int, absl::string_view) override {}
#else
  void AddError(const std::string&, int, int, const std::string&) override {}
#endif
};

std::string Hex(std::string_view s) {
  std::string out;
  char b[3];
  for (char c : s) {
    std::snprintf(b, sizeof b, "%02x", static_cast<uint8_t>(c));
    out += b;
  }
  return out;
}

std::string QuoteLines(const std::string& s) {
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
  return QuoteLines(st.message());
}

// The stream itself, then every prefix of it, then each byte set to each of
// four values: what a dropped or corrupted connection delivers.
void Variants(const std::string& stream,
              const std::function<void(const std::string&, const std::string&)>& emit) {
  emit("whole", stream);
  for (size_t n = 0; n < stream.size(); ++n) {
    emit("prefix " + std::to_string(n), stream.substr(0, n));
  }
  for (size_t i = 0; i < stream.size(); ++i) {
    for (uint8_t b : {0x00, 0x7f, 0x80, 0xff}) {
      std::string c = stream;
      c[i] = static_cast<char>(b);
      emit("byte " + std::to_string(i) + "=" + Hex(std::string(1, static_cast<char>(b))), c);
    }
  }
}

int PbStreams() {
  std::vector<StreamMsg> msgs(3);
  msgs[0].N = 1;
  msgs[0].Text = "small";
  msgs[2].N = -2;
  msgs[2].Data = {0xab, 0xcd};
  std::ostringstream buf;
  pb::Encoder enc(&buf);
  for (const StreamMsg& m : msgs) {
    if (Status st = enc.Encode(m); !st.ok()) return 2;
  }
  const std::string stream = buf.str();
  std::cout << "=== pb encoded " << Hex(stream) << "\n";
  for (int limit : {0, 6}) {
    Variants(stream, [&](const std::string& label, const std::string& data) {
      std::cout << "--- pb limit " << limit << " " << label << "\n";
      std::istringstream in(data);
      pb::Decoder dec(&in);
      if (limit > 0) dec.SetMaxFrameSize(limit);
      for (int i = 0; i < kStreamDecodes; ++i) {
        StreamMsg m;
        if (Status st = dec.Decode(m); !st.ok()) {
          std::cout << Text(st) << "\n";
          continue;
        }
        std::cout << "ok " << m.N << " " << Hex(m.Text) << " "
                  << Hex(std::string(m.Data.begin(), m.Data.end())) << "\n";
      }
    });
  }
  return 0;
}

std::string MessageText(const gpb::Message& m) {
  auto out = pxf::Marshal(m);
  if (!out.ok()) return Text(out.status());
  return QuoteLines(*out);
}

int SbeStreams(const std::string& root, const std::string& dir, const std::string& proto_dir) {
  gpb::compiler::DiskSourceTree tree;
  tree.MapPath("", WKT_PROTO_DIR);
  tree.MapPath("", proto_dir);
  tree.MapPath("", root);
  tree.MapPath("", (std::filesystem::path(root) / dir).string());
  NoErrors errors;
  gpb::compiler::Importer importer(&tree, &errors);
  const gpb::FileDescriptor* file = importer.Import("sbe_stream.proto");
  if (file == nullptr) return 2;
  auto codec = sbe::Codec::New({file});
  if (!codec.ok()) return 2;
  gpb::DynamicMessageFactory factory(importer.pool());
  const gpb::Descriptor* simple = importer.pool()->FindMessageTypeByName("stream.v1.Simple");
  const gpb::Descriptor* order = importer.pool()->FindMessageTypeByName("stream.v1.Order");

  std::unique_ptr<gpb::Message> m(factory.GetPrototype(simple)->New());
  if (!pxf::Unmarshal("id = 7\nvalue = -9\n", m.get()).ok()) return 2;
  std::unique_ptr<gpb::Message> o(factory.GetPrototype(order)->New());
  if (!pxf::Unmarshal("order_id = 5\nsymbol = \"ABC\"\nfills = [{fill_price = -1\nfill_qty = 2}]\n",
                      o.get())
           .ok()) {
    return 2;
  }
  struct Case {
    std::string name;
    const gpb::Message* m;
  };
  for (const Case& c : {Case{"Simple", m.get()}, Case{"Order", o.get()}}) {
    const gpb::Descriptor* desc = c.m->GetDescriptor();
    std::ostringstream buf;
    auto enc = codec->NewEncoder(&buf);
    for (int i = 0; i < 2; ++i) {
      if (!enc.Encode(*c.m).ok()) return 2;
    }
    const std::string stream = buf.str();
    std::cout << "=== sbe " << c.name << " encoded " << Hex(stream) << "\n";
    Variants(stream, [&](const std::string& label, const std::string& data) {
      std::cout << "--- sbe " << c.name << " " << label << "\n";
      std::istringstream in(data);
      auto dec = codec->NewDecoder(&in);
      for (int i = 0; i < kStreamDecodes; ++i) {
        std::unique_ptr<gpb::Message> got(factory.GetPrototype(desc)->New());
        if (Status st = dec.Decode(got.get()); !st.ok()) {
          std::cout << Text(st) << "\n";
          continue;
        }
        std::cout << "ok " << MessageText(*got) << "\n";
      }
      if (data.size() < 6) return;
      std::cout << "descriptor: ";
      std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
      auto got = codec->UnmarshalDescriptor(bytes.subspan(6, data.size() / 2 + 3 - 6), desc);
      if (!got.ok()) {
        std::cout << Text(got.status()) << "\n";
        return;
      }
      std::cout << "ok " << MessageText(**got) << "\n";
    });
  }
  return 0;
}

}  // namespace

int RunStreams(const std::string& root, const std::string& dir, const std::string& proto_dir) {
  if (int rc = PbStreams(); rc != 0) return rc;
  return SbeStreams(root, dir, proto_dir);
}

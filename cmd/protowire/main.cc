// protowire CLI — encode/decode/validate/fmt PXF.
//
//   Standalone mode: compile .proto files via libprotobuf's Importer.
//     protowire encode -p schema.proto -m pkg.Type input.pxf > out.pb
//
//   Registry mode (PROTOWIRE_WITH_REGISTRY=ON): fetch a FileDescriptorSet
//   from a remote protoregistry gRPC service.
//     protowire encode -s host:port -n NS --schema NAME -m TYPE input.pxf

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include "CLI11.hpp"
#include "protowire/pxf.h"

#if PROTOWIRE_WITH_REGISTRY
#include <grpcpp/grpcpp.h>

#include "protoregistry/v1/registry.grpc.pb.h"
#include "protoregistry/v1/registry.pb.h"
#endif

namespace pbuf = ::google::protobuf;

namespace {

class StderrErrorCollector : public pbuf::compiler::MultiFileErrorCollector {
 public:
  void RecordError(absl::string_view filename, int line, int column,
                   absl::string_view message) override {
    std::fprintf(stderr, "%s:%d:%d: %s\n", std::string(filename).c_str(), line,
                 column, std::string(message).c_str());
    failed_ = true;
  }
  bool failed() const { return failed_; }

 private:
  bool failed_ = false;
};

std::string ReadFile(const std::string& path) {
  if (path == "-" || path.empty()) {
    std::stringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    std::exit(2);
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void WriteOut(const std::string& data) {
  std::fwrite(data.data(), 1, data.size(), stdout);
}

struct Schema {
  const pbuf::Descriptor* desc = nullptr;
  std::shared_ptr<pbuf::DescriptorPool> pool;
  std::shared_ptr<pbuf::DynamicMessageFactory> factory;
  // Keep importer alive so the descriptor pool stays valid.
  std::shared_ptr<pbuf::compiler::Importer> importer;
  std::shared_ptr<pbuf::compiler::DiskSourceTree> source_tree;
  std::shared_ptr<StderrErrorCollector> errors;
};

Schema LoadStandaloneSchema(const std::vector<std::string>& proto_files,
                            const std::string& message_type) {
  Schema s;
  s.source_tree = std::make_shared<pbuf::compiler::DiskSourceTree>();
  // Map cwd and well-known protos.
  s.source_tree->MapPath("", ".");
  s.source_tree->MapPath("", PROTOWIRE_WKT_PROTO_DIR);
  s.errors = std::make_shared<StderrErrorCollector>();
  s.importer = std::make_shared<pbuf::compiler::Importer>(s.source_tree.get(),
                                                         s.errors.get());
  for (const auto& f : proto_files) {
    if (s.importer->Import(f) == nullptr) {
      std::fprintf(stderr, "failed to import %s\n", f.c_str());
      std::exit(2);
    }
  }
  s.desc = s.importer->pool()->FindMessageTypeByName(message_type);
  if (!s.desc) {
    std::fprintf(stderr, "message type %s not found\n", message_type.c_str());
    std::exit(2);
  }
  s.factory = std::make_shared<pbuf::DynamicMessageFactory>(s.importer->pool());
  return s;
}

#if PROTOWIRE_WITH_REGISTRY
Schema LoadRegistrySchema(const std::string& server,
                          const std::string& ns_id,
                          const std::string& schema_id,
                          const std::string& message_type) {
  Schema s;
  auto channel = grpc::CreateChannel(server, grpc::InsecureChannelCredentials());
  auto stub = protoregistry::v1::RegistryService::NewStub(channel);

  protoregistry::v1::GetDescriptorRequest req;
  req.set_namespace_id(ns_id);
  req.set_schema_id(schema_id);
  req.set_version(0);  // current

  protoregistry::v1::GetDescriptorResponse resp;
  grpc::ClientContext ctx;
  auto status = stub->GetDescriptor(&ctx, req, &resp);
  if (!status.ok()) {
    std::fprintf(stderr, "registry GetDescriptor failed: %s\n",
                 status.error_message().c_str());
    std::exit(2);
  }

  s.pool = std::make_shared<pbuf::DescriptorPool>();
  for (const auto& fd : resp.file_descriptor_set().file()) {
    if (s.pool->BuildFile(fd) == nullptr) {
      std::fprintf(stderr, "failed to build %s\n", fd.name().c_str());
      std::exit(2);
    }
  }
  s.desc = s.pool->FindMessageTypeByName(message_type);
  if (!s.desc) {
    std::fprintf(stderr, "message type %s not found in registry schema\n",
                 message_type.c_str());
    std::exit(2);
  }
  s.factory = std::make_shared<pbuf::DynamicMessageFactory>(s.pool.get());
  return s;
}
#endif

int CmdEncode(const Schema& s, const std::string& input_path) {
  std::string text = ReadFile(input_path);
  std::unique_ptr<pbuf::Message> msg(s.factory->GetPrototype(s.desc)->New());
  auto st = protowire::pxf::Unmarshal(text, msg.get());
  if (!st.ok()) {
    std::fprintf(stderr, "decode error: %s\n", st.ToString().c_str());
    return 1;
  }
  std::string out;
  if (!msg->SerializeToString(&out)) {
    std::fprintf(stderr, "proto serialization failed\n");
    return 1;
  }
  WriteOut(out);
  return 0;
}

int CmdDecode(const Schema& s, const std::string& input_path) {
  std::string bin = ReadFile(input_path);
  std::unique_ptr<pbuf::Message> msg(s.factory->GetPrototype(s.desc)->New());
  if (!msg->ParseFromString(bin)) {
    std::fprintf(stderr, "binary parse failed\n");
    return 1;
  }
  auto out = protowire::pxf::Marshal(*msg);
  if (!out.ok()) {
    std::fprintf(stderr, "encode error: %s\n", out.status().ToString().c_str());
    return 1;
  }
  WriteOut(*out);
  return 0;
}

int CmdValidate(const Schema& s, const std::string& input_path) {
  std::string text = ReadFile(input_path);
  std::unique_ptr<pbuf::Message> msg(s.factory->GetPrototype(s.desc)->New());
  auto r = protowire::pxf::UnmarshalFull(text, msg.get());
  if (!r.ok()) {
    std::fprintf(stderr, "validation error: %s\n",
                 r.status().ToString().c_str());
    return 1;
  }
  std::printf("OK\n");
  return 0;
}

int CmdFmt(const Schema& s, const std::string& input_path) {
  std::string text = ReadFile(input_path);
  std::unique_ptr<pbuf::Message> msg(s.factory->GetPrototype(s.desc)->New());
  auto st = protowire::pxf::Unmarshal(text, msg.get());
  if (!st.ok()) {
    std::fprintf(stderr, "decode error: %s\n", st.ToString().c_str());
    return 1;
  }
  auto out = protowire::pxf::Marshal(*msg);
  if (!out.ok()) {
    std::fprintf(stderr, "encode error: %s\n", out.status().ToString().c_str());
    return 1;
  }
  WriteOut(*out);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"protowire — PXF encode/decode/validate/fmt CLI"};
  app.require_subcommand(1);

  // Shared schema-source flags (added to each subcommand).
  std::vector<std::string> proto_files;
  std::string message_type;
  std::string server;
  std::string ns_id;
  std::string schema_id;
  std::string input_path = "-";

  auto add_schema_flags = [&](CLI::App* sub) {
    sub->add_option("-p,--proto", proto_files,
                    ".proto files to compile (standalone mode)");
    sub->add_option("-m,--message", message_type,
                    "fully-qualified message type")->required();
    sub->add_option("-s,--server", server,
                    "protoregistry host:port (registry mode)");
    sub->add_option("-n,--namespace", ns_id, "registry namespace id");
    sub->add_option("--schema", schema_id, "registry schema id");
    sub->add_option("input", input_path,
                    "input path or '-' for stdin")->capture_default_str();
  };

  auto encode_cmd = app.add_subcommand("encode", "PXF text → proto binary");
  auto decode_cmd = app.add_subcommand("decode", "proto binary → PXF text");
  auto validate_cmd = app.add_subcommand("validate", "validate PXF text");
  auto fmt_cmd = app.add_subcommand("fmt", "round-trip-format PXF text");
  for (auto* c : {encode_cmd, decode_cmd, validate_cmd, fmt_cmd}) {
    add_schema_flags(c);
  }

  CLI11_PARSE(app, argc, argv);

  Schema schema;
  if (!proto_files.empty()) {
    schema = LoadStandaloneSchema(proto_files, message_type);
  } else if (!server.empty()) {
#if PROTOWIRE_WITH_REGISTRY
    schema = LoadRegistrySchema(server, ns_id, schema_id, message_type);
#else
    std::fprintf(stderr,
                 "registry mode requires PROTOWIRE_WITH_REGISTRY=ON\n");
    return 2;
#endif
  } else {
    std::fprintf(stderr, "must provide -p (standalone) or -s (registry)\n");
    return 2;
  }

  if (encode_cmd->parsed()) return CmdEncode(schema, input_path);
  if (decode_cmd->parsed()) return CmdDecode(schema, input_path);
  if (validate_cmd->parsed()) return CmdValidate(schema, input_path);
  if (fmt_cmd->parsed()) return CmdFmt(schema, input_path);
  return 0;
}

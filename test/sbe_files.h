// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// SbeFiles compiles .proto sources written at runtime, each importing
// sbe/annotations.proto, and keeps the pool and a message factory alive for
// the test. It mirrors the in-memory compiles of protowire-go's SBE tests.

#pragma once

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>

#include "fixture_dir.h"
#include "protoc_compat.h"

namespace protowire_test {

class SbeFiles {
 public:
  // Compile writes each (name, source) pair into the fixture directory and
  // imports them in order. It returns false, with the error in error(), if
  // any fails to compile.
  bool Compile(const std::vector<std::pair<std::string, std::string>>& srcs) {
    tree_.MapPath("", PROTO_DIR);
    tree_.MapPath("", WKT_PROTO_DIR);
    tree_.MapPath("", FixtureDir());
    for (const auto& [name, src] : srcs) {
      std::ofstream(FixtureDir() + "/" + name) << src;
    }
    importer_ = std::make_unique<google::protobuf::compiler::Importer>(&tree_, &errors_);
    for (const auto& [name, src] : srcs) {
      const google::protobuf::FileDescriptor* fd = importer_->Import(name);
      if (fd == nullptr) return false;
      files_.push_back(fd);
    }
    factory_ = std::make_unique<google::protobuf::DynamicMessageFactory>(importer_->pool());
    return true;
  }

  const std::vector<const google::protobuf::FileDescriptor*>& files() const { return files_; }
  const std::string& error() const { return errors_.last_; }

  const google::protobuf::Descriptor* Message(const std::string& full_name) const {
    return importer_->pool()->FindMessageTypeByName(full_name);
  }

  std::unique_ptr<google::protobuf::Message> New(const google::protobuf::Descriptor* md) {
    return std::unique_ptr<google::protobuf::Message>(factory_->GetPrototype(md)->New());
  }

 private:
  class Errors : public google::protobuf::compiler::MultiFileErrorCollector {
   public:
    PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
      last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
              ": " + std::string(msg);
    }
    std::string last_;
  };

  google::protobuf::compiler::DiskSourceTree tree_;
  Errors errors_;
  std::unique_ptr<google::protobuf::compiler::Importer> importer_;
  std::unique_ptr<google::protobuf::DynamicMessageFactory> factory_;
  std::vector<const google::protobuf::FileDescriptor*> files_;
};

}  // namespace protowire_test

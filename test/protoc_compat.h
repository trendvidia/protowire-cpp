// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Compat shim for the google::protobuf::compiler::MultiFileErrorCollector
// API across protobuf 3.x and 4.x. Used by every test that drives
// google::protobuf::compiler::Importer to compile a .proto fragment at
// runtime.
//
// protobuf 3.x  → virtual void AddError(const std::string& filename,
//                                       int line, int column,
//                                       const std::string& msg);
// protobuf 4.x  → virtual void RecordError(absl::string_view filename,
//                                          int line, int column,
//                                          absl::string_view msg);
//
// The override macro lets test fixtures write a single body that
// captures errors regardless of which version is linked in.
//
// Usage:
//
//   #include "protoc_compat.h"
//   class CollectErrors : public pb::compiler::MultiFileErrorCollector {
//    public:
//     PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
//       last_ = std::string(filename) + ":" + std::to_string(line) +
//               ":" + std::to_string(column) + ": " + std::string(msg);
//     }
//     std::string last_;
//   };

#pragma once

#include <google/protobuf/stubs/common.h>
#include <string>

#if GOOGLE_PROTOBUF_VERSION >= 4000000
#include <absl/strings/string_view.h>

#define PROTOWIRE_PROTOC_RECORD_ERROR(_filename, _line, _column, _msg)                          \
  void RecordError(absl::string_view _filename, int _line, int _column, absl::string_view _msg) \
      override
#else

#define PROTOWIRE_PROTOC_RECORD_ERROR(_filename, _line, _column, _msg)                         \
  void AddError(const std::string& _filename, int _line, int _column, const std::string& _msg) \
      override
#endif

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "protowire/pxf/ast.h"  // Directive, DatasetDirective, ProtoDirective

namespace protowire::pxf {

// Result tracks per-field presence after a UnmarshalFull. Field paths are
// dotted (e.g. "tls.cert_file"). Set + Null are mutually exclusive; Absent
// is the complement of both.
//
// Result also surfaces the document-root directives the decoder saw:
//   - Directives()  → generic `@<name> *(prefix) [{ ... }]` blocks, in
//     source order, excluding @type / @dataset (which have their own
//     handling).
//   - Datasets()      → `@dataset <type> ( cols ) row*` directives, in
//     source order. A document with any @dataset has no body entries,
//     so the rows are the document's payload — consumers walk
//     DatasetDirective::rows and bind each row's cells to a fresh
//     instance of DatasetDirective::type via their own schema.
class Result {
 public:
  bool IsSet(const std::string& path) const {
    return present_.count(path) > 0 && null_.count(path) == 0;
  }
  bool IsNull(const std::string& path) const { return null_.count(path) > 0; }
  bool IsAbsent(const std::string& path) const {
    return present_.count(path) == 0 && null_.count(path) == 0;
  }
  std::vector<std::string> NullFields() const {
    return std::vector<std::string>(null_.begin(), null_.end());
  }
  std::vector<std::string> SetFields() const {
    std::vector<std::string> out;
    for (const auto& p : present_) {
      if (!null_.count(p)) out.push_back(p);
    }
    return out;
  }

  void MarkPresent(std::string path) { present_.insert(std::move(path)); }
  void MarkNull(std::string path) {
    null_.insert(path);
    present_.insert(std::move(path));
  }
  bool Has(const std::string& path) const { return present_.count(path) > 0; }

  // Directive accessors (PXF v0.72+).
  const std::vector<Directive>& Directives() const { return directives_; }
  const std::vector<DatasetDirective>& Datasets() const { return datasets_; }
  const std::vector<ProtoDirective>& Protos() const { return protos_; }

  void AddDirective(Directive d) { directives_.push_back(std::move(d)); }
  void AddTable(DatasetDirective t) { datasets_.push_back(std::move(t)); }
  void AddProto(ProtoDirective p) { protos_.push_back(std::move(p)); }

 private:
  std::unordered_set<std::string> present_;
  std::unordered_set<std::string> null_;
  std::vector<Directive> directives_;
  std::vector<DatasetDirective> datasets_;
  std::vector<ProtoDirective> protos_;
};

}  // namespace protowire::pxf

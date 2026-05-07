// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace protowire::pxf {

// Result tracks per-field presence after a UnmarshalFull. Field paths are
// dotted (e.g. "tls.cert_file"). Set + Null are mutually exclusive; Absent
// is the complement of both.
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

 private:
  std::unordered_set<std::string> present_;
  std::unordered_set<std::string> null_;
};

}  // namespace protowire::pxf

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// FixtureDir returns a directory for the .proto fragments a test writes
// and compiles at runtime. It belongs to this process and is removed when
// the process exits.
//
// testing::TempDir() is shared: gtest_discover_tests runs each test as its
// own process, so under `ctest -j` two tests writing "cfg.proto" there
// overwrite each other, or one reads the other's half-written file
// (protowire-cpp#59, #52).

#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace protowire_test {

inline const std::string& FixtureDir() {
  struct Dir {
    Dir() {
      std::random_device rd;
      const uint64_t id = (uint64_t{rd()} << 32) | rd();
      char name[32];
      std::snprintf(name, sizeof(name), "protowire-%016llx", static_cast<unsigned long long>(id));
      path = (std::filesystem::path(testing::TempDir()) / name).string();
      std::filesystem::create_directories(path);
    }
    ~Dir() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
    std::string path;
  };
  static const Dir dir;
  return dir.path;
}

}  // namespace protowire_test

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// MustMarshal is pb::Marshal for a test that expects it to succeed: a refusal
// fails the test, with its text, and gives no bytes.

#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "protowire/pb.h"

namespace protowire::testing {

template <class T>
std::vector<uint8_t> MustMarshal(const T& v, pb::MarshalOptions opts = {}) {
  StatusOr<std::vector<uint8_t>> out = pb::Marshal(v, opts);
  EXPECT_TRUE(out.ok()) << out.status().message();
  if (!out.ok()) return {};
  return std::move(out).consume();
}

}  // namespace protowire::testing

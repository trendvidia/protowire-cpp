// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <string>
#include <utility>

#include "protowire/detail/status.h"

namespace protowire::detail {

struct Position {
  int line = 0;
  int column = 0;
  bool valid() const { return line > 0; }
  std::string ToString() const { return std::to_string(line) + ":" + std::to_string(column); }
};

template <class... Args>
Status MakeError(Position pos, std::string message) {
  if (pos.valid()) {
    return Status::Error(pos.line, pos.column, std::move(message));
  }
  return Status::Error(std::move(message));
}

}  // namespace protowire::detail

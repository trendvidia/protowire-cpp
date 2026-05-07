// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/detail/status.h"

namespace protowire {

std::string Status::ToString() const {
  if (ok()) return "OK";
  if (line_ > 0) {
    return std::to_string(line_) + ":" + std::to_string(column_) + ": " + message_;
  }
  return message_;
}

}  // namespace protowire

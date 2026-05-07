// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace protowire::detail {

// Decoded duration broken into seconds + nanos in [0, 1e9).
struct Duration {
  int64_t seconds = 0;
  int32_t nanos = 0;

  // Total nanoseconds. Caller must ensure it fits in int64_t.
  int64_t total_nanos() const { return seconds * 1'000'000'000LL + nanos; }
};

// Parses a Go-style duration: "300ms", "1.5h", "2h45m", "-1h30m". Units are
// "ns", "us", "µs", "ms", "s", "m", "h". "0" alone is also valid.
std::optional<Duration> ParseDuration(std::string_view s);

// Formats as a minimal Go-style duration ("1h30m45s", "500ns"). Mirrors the
// output of time.Duration.String().
std::string FormatDuration(int64_t seconds, int32_t nanos);

}  // namespace protowire::detail

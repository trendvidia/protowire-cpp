// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace protowire::detail {

// Decoded RFC 3339 timestamp. Stored as Unix seconds since 1970-01-01T00:00:00Z
// plus a nanosecond fraction in [0, 1e9).
struct Timestamp {
  int64_t seconds = 0;
  int32_t nanos = 0;
};

// Parses RFC 3339 / RFC 3339 Nano (`2024-01-15T10:30:00Z`,
// `2024-01-15T10:30:00.123456789+02:00`). Returns nullopt on error.
std::optional<Timestamp> ParseRFC3339(std::string_view s);

// Formats as RFC 3339 Nano in UTC. If nanos == 0, no fractional part is
// emitted; otherwise a precise fractional part is emitted (3, 6, or 9 digits
// depending on trailing zeros).
std::string FormatRFC3339Nano(int64_t seconds, int32_t nanos);

}  // namespace protowire::detail

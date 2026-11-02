// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Go's number and time parsers, with their acceptance and their error text.
// A (pxf.default) arrives as a string, so it is read by these and not by the
// lexer's rules, and protowire-go's errors wrap what these return.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace protowire::detail {

// strconv.ParseInt(s, 10, bits). On failure *err is Go's error text.
bool GoParseInt(std::string_view s, int bits, int64_t* out, std::string* err);

// strconv.ParseUint(s, 10, bits).
bool GoParseUint(std::string_view s, int bits, uint64_t* out, std::string* err);

// strconv.ParseFloat(s, bits), bits being 32 or 64.
bool GoParseFloat(std::string_view s, int bits, double* out, std::string* err);

// The error of time.ParseDuration for an s it refuses.
std::string GoDurationError(std::string_view s);

// The error of time.Parse(time.RFC3339, s) for an s it refuses.
std::string GoTimestampError(std::string_view s);

}  // namespace protowire::detail

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace protowire::detail {

std::string Base64EncodeStd(const uint8_t* data, size_t n);
inline std::string Base64EncodeStd(std::string_view s) {
  return Base64EncodeStd(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
inline std::string Base64EncodeStd(const std::vector<uint8_t>& v) {
  return Base64EncodeStd(v.data(), v.size());
}

// Decodes standard or RawStd (un-padded) base64. Returns nullopt on error.
std::optional<std::vector<uint8_t>> Base64DecodeStd(std::string_view s);

// Base64DecodeProto takes a @proto descriptor body, padded standard or URL
// base64. On failure *offset is the byte Go's CorruptInputError names for the
// URL decode, the last one protowire-go tries.
std::optional<std::vector<uint8_t>> Base64DecodeProto(std::string_view s, size_t* offset);

// Base64DecodeLenient takes standard or URL base64, padded or not, as PXF
// bytes literals do (protowire-go's decodeBase64Lenient). On failure *offset
// is where StdEncoding failed.
std::optional<std::vector<uint8_t>> Base64DecodeLenient(std::string_view s, size_t* offset);

}  // namespace protowire::detail

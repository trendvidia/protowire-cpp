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

}  // namespace protowire::detail

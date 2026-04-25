#include "protowire/detail/base64.h"

#include <array>

namespace protowire::detail {

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Decode table: 0..63 are valid; 64 = padding ('='); 255 = invalid.
constexpr std::array<uint8_t, 256> MakeDecodeTable() {
  std::array<uint8_t, 256> t{};
  for (int i = 0; i < 256; ++i) t[i] = 255;
  for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(kAlphabet[i])] = i;
  t[static_cast<unsigned char>('=')] = 64;
  return t;
}

constexpr auto kDecodeTable = MakeDecodeTable();

}  // namespace

std::string Base64EncodeStd(const uint8_t* data, size_t n) {
  std::string out;
  out.reserve(((n + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8) |
                 uint32_t{data[i + 2]};
    out += kAlphabet[(v >> 18) & 0x3f];
    out += kAlphabet[(v >> 12) & 0x3f];
    out += kAlphabet[(v >> 6) & 0x3f];
    out += kAlphabet[v & 0x3f];
  }
  if (i < n) {
    uint32_t v = uint32_t{data[i]} << 16;
    if (i + 1 < n) v |= uint32_t{data[i + 1]} << 8;
    out += kAlphabet[(v >> 18) & 0x3f];
    out += kAlphabet[(v >> 12) & 0x3f];
    if (i + 1 < n) {
      out += kAlphabet[(v >> 6) & 0x3f];
      out += '=';
    } else {
      out += "==";
    }
  }
  return out;
}

std::optional<std::vector<uint8_t>> Base64DecodeStd(std::string_view s) {
  // Accept both padded and unpadded; ignore trailing padding.
  // Strip whitespace? Go's StdEncoding does not, so we do not either.
  size_t len = s.size();
  if (len == 0) return std::vector<uint8_t>{};

  // If padded, length must be a multiple of 4.
  bool has_pad = !s.empty() && s.back() == '=';
  if (has_pad && (len % 4) != 0) return std::nullopt;

  std::vector<uint8_t> out;
  out.reserve(len * 3 / 4);

  uint32_t buf = 0;
  int bits = 0;
  size_t consumed = 0;
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint8_t v = kDecodeTable[c];
    if (v == 255) return std::nullopt;
    if (v == 64) {
      // padding — must come at end
      for (size_t j = i; j < len; ++j) {
        if (s[j] != '=') return std::nullopt;
      }
      break;
    }
    buf = (buf << 6) | v;
    bits += 6;
    ++consumed;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
    }
  }
  // Validate residue: any leftover bits must be zero.
  if (bits >= 6) return std::nullopt;
  if (bits > 0 && (buf & ((1u << bits) - 1)) != 0) return std::nullopt;
  // Validate consumed count: total chars (consumed + pad) must imply consistent
  // length.
  size_t pad = len - consumed;
  if (has_pad) {
    if (pad != 1 && pad != 2) return std::nullopt;
  }
  return out;
}

}  // namespace protowire::detail

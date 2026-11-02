// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Length-prefixed framing. Mirrors protowire-go's encoding/pb/stream.go.

#include <ostream>
#include <string>

#include "protowire/detail/stream.h"
#include "protowire/pb.h"

namespace protowire::pb {

namespace {

Status Corrupt(const std::string& why) {
  return Status::Error(std::string(kErrFramingCorrupt) + ": " + why);
}

enum class VarintEnd : uint8_t { kOk, kEOF, kUnexpectedEOF, kError, kOverflow };

// ReadUvarint is Go's binary.ReadUvarint, byte by byte, since a frame's length
// cannot be read past its last byte.
VarintEnd ReadUvarint(std::istream& r, uint64_t* out) {
  uint64_t x = 0;
  unsigned shift = 0;
  for (int i = 0; i < kMaxVarintBytes; ++i) {
    char c;
    size_t got;
    switch (protowire::detail::ReadFull(r, &c, 1, &got)) {
      case protowire::detail::ReadEnd::kFull:
        break;
      case protowire::detail::ReadEnd::kEOF:
        return i > 0 ? VarintEnd::kUnexpectedEOF : VarintEnd::kEOF;
      case protowire::detail::ReadEnd::kError:
        return VarintEnd::kError;
    }
    uint8_t b = static_cast<uint8_t>(c);
    if (b < 0x80) {
      if (i == kMaxVarintBytes - 1 && b > 1) return VarintEnd::kOverflow;
      *out = x | static_cast<uint64_t>(b) << shift;
      return VarintEnd::kOk;
    }
    x |= static_cast<uint64_t>(b & 0x7f) << shift;
    shift += 7;
  }
  return VarintEnd::kOverflow;
}

}  // namespace

bool IsFramingCorrupt(const Status& st) {
  return st.message().rfind(kErrFramingCorrupt, 0) == 0;
}

Status Encoder::WriteFrame(std::span<const uint8_t> body) {
  std::vector<uint8_t> hdr;
  wire::AppendVarint(hdr, body.size());
  if (!w_->write(reinterpret_cast<const char*>(hdr.data()),
                 static_cast<std::streamsize>(hdr.size()))) {
    return Status::Error("write failed");
  }
  if (body.empty()) return Status::OK();
  if (!w_->write(reinterpret_cast<const char*>(body.data()),
                 static_cast<std::streamsize>(body.size()))) {
    return Status::Error("write failed");
  }
  return Status::OK();
}

Status Decoder::ReadFrame() {
  if (!err_.ok()) return err_;
  uint64_t n = 0;
  switch (ReadUvarint(*r_, &n)) {
    case VarintEnd::kOk:
      break;
    case VarintEnd::kEOF:
      return Status::Error(kErrEOF);
    case VarintEnd::kUnexpectedEOF:
      err_ = Status::Error(kErrUnexpectedEOF);
      return err_;
    case VarintEnd::kError:
      err_ = Corrupt(protowire::detail::kErrRead);
      return err_;
    case VarintEnd::kOverflow:
      err_ = Corrupt("binary: varint overflows a 64-bit integer");
      return err_;
  }
  if (n > static_cast<uint64_t>(max_)) {
    err_ = Corrupt("frame size " + std::to_string(n) + " exceeds max " + std::to_string(max_));
    return err_;
  }
  buf_.resize(n);
  size_t got;
  switch (protowire::detail::ReadFull(*r_, reinterpret_cast<char*>(buf_.data()), n, &got)) {
    case protowire::detail::ReadEnd::kFull:
      return Status::OK();
    case protowire::detail::ReadEnd::kEOF:
      err_ = Status::Error(kErrUnexpectedEOF);
      return err_;
    case protowire::detail::ReadEnd::kError:
      break;
  }
  err_ = Status::Error(protowire::detail::kErrRead);
  return err_;
}

}  // namespace protowire::pb

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Simple Open Framing Header. Mirrors protowire-go's encoding/sbe/stream.go.

#include <array>
#include <cstdio>
#include <ostream>
#include <string>

#include "protowire/detail/stream.h"
#include "protowire/sbe.h"

namespace protowire::sbe {

namespace {

// The FIX SOFH-registered encoding type for "Simple Binary Encoding Version
// 1.0 with Little-Endian byte order". The codec writes little-endian, so a
// receiver rejects any other value.
constexpr uint16_t kSofhEncodingSbeLE = 0xEB50;
constexpr size_t kSofhHeaderLen = 6;

Status Corrupt(const std::string& why) {
  return Status::Error(std::string(kErrFramingCorrupt) + ": " + why);
}

}  // namespace

bool IsFramingCorrupt(const Status& st) {
  return st.message().rfind(kErrFramingCorrupt, 0) == 0;
}

Encoder Codec::NewEncoder(std::ostream* w) const {
  return Encoder(this, w);
}

Decoder Codec::NewDecoder(std::istream* r) const {
  return Decoder(this, r);
}

Status Encoder::Encode(const google::protobuf::Message& msg) {
  auto body = codec_->Marshal(msg);
  if (!body.ok()) return body.status();
  const uint32_t total = static_cast<uint32_t>(kSofhHeaderLen + body->size());
  const std::array<char, kSofhHeaderLen> hdr = {
      static_cast<char>(total >> 24),
      static_cast<char>(total >> 16),
      static_cast<char>(total >> 8),
      static_cast<char>(total),
      static_cast<char>(kSofhEncodingSbeLE >> 8),
      static_cast<char>(kSofhEncodingSbeLE & 0xff),
  };
  if (!w_->write(hdr.data(), hdr.size())) return Status::Error("write failed");
  if (body->empty()) return Status::OK();
  if (!w_->write(reinterpret_cast<const char*>(body->data()),
                 static_cast<std::streamsize>(body->size()))) {
    return Status::Error("write failed");
  }
  return Status::OK();
}

Status Decoder::Decode(google::protobuf::Message* msg) {
  namespace d = protowire::detail;
  if (!err_.ok()) return err_;
  std::array<char, kSofhHeaderLen> hdr;
  size_t got;
  if (d::ReadFull(*r_, hdr.data(), hdr.size(), &got) != d::ReadEnd::kFull) {
    if (got == 0 && r_->eof() && !r_->bad()) return Status::Error(kErrEOF);
    err_ = Status::Error(kErrUnexpectedEOF);
    return err_;
  }
  const auto byte = [&](int i) { return static_cast<uint32_t>(static_cast<uint8_t>(hdr[i])); };
  const uint32_t total = byte(0) << 24 | byte(1) << 16 | byte(2) << 8 | byte(3);
  const uint16_t enc = static_cast<uint16_t>(byte(4) << 8 | byte(5));
  if (enc != kSofhEncodingSbeLE) {
    char hex[7];
    std::snprintf(hex, sizeof hex, "0x%04x", enc);
    err_ = Corrupt("SOFH encoding type " + std::string(hex) + " not supported");
    return err_;
  }
  if (total < kSofhHeaderLen) {
    err_ = Corrupt("SOFH total length " + std::to_string(total) + " < header size");
    return err_;
  }
  const int64_t body_len = static_cast<int64_t>(total) - kSofhHeaderLen;
  if (body_len > max_) {
    err_ =
        Corrupt("frame size " + std::to_string(body_len) + " exceeds max " + std::to_string(max_));
    return err_;
  }
  buf_.resize(static_cast<size_t>(body_len));
  switch (d::ReadFull(*r_, reinterpret_cast<char*>(buf_.data()), buf_.size(), &got)) {
    case d::ReadEnd::kFull:
      return codec_->Unmarshal(buf_, msg);
    case d::ReadEnd::kEOF:
      err_ = Status::Error(kErrUnexpectedEOF);
      return err_;
    case d::ReadEnd::kError:
      break;
  }
  err_ = Status::Error(d::kErrRead);
  return err_;
}

}  // namespace protowire::sbe

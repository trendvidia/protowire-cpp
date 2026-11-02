// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// SBE (FIX Simple Binary Encoding) ↔ google::protobuf::Message.
//
// Driven by a `.proto` schema with SBE annotations. A Codec pre-computes
// fixed offsets for every field at construction time and produces standard
// SBE binary, wire-compatible with any SBE implementation.
//
//   const FileDescriptor* file = ...;     // imported with sbe annotations
//   auto codec = protowire::sbe::Codec::New({file});
//   auto bytes = codec->Marshal(my_message);
//   codec->Unmarshal(bytes, &my_message);

#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "protowire/check.h"
#include "protowire/detail/status.h"
#include "protowire/detail/stream.h"
#include "protowire/limits.h"
#include "protowire/sbe/template.h"

namespace protowire::sbe {

class Codec;
class Decoder;
class Encoder;
class View;
class GroupView;

// kDefaultMaxFrameSize caps the SBE body size at 16 MiB by default.
inline constexpr int kDefaultMaxFrameSize = 16 << 20;

// kErrFramingCorrupt opens the error of invalid SOFH framing: a wrong encoding
// type, a length less than the header, a frame larger than the maximum. The byte
// stream is then desynchronized and the decoder is permanently unusable.
// IsFramingCorrupt tests for it.
inline constexpr const char* kErrFramingCorrupt = "sbe: stream framing corrupt";
bool IsFramingCorrupt(const Status& st);

// CodecOptions carries the HARDENING.md § Mandatory limits the codec's
// decoders enforce, configurable per codec; 0 selects the default in
// protowire/limits.h. max_message_size caps the input to one Unmarshal or
// NewView call; max_repeated_count caps a repeating group's numInGroup,
// checked before any entry is allocated. validator, if non-null, runs on every
// message Unmarshal decodes and must outlive the codec.
struct CodecOptions {
  int max_message_size = 0;
  int max_repeated_count = 0;
  check::Validator* validator = nullptr;
};

class Codec {
 public:
  static StatusOr<Codec> New(std::vector<const google::protobuf::FileDescriptor*> files,
                             CodecOptions opts = {});

  StatusOr<std::vector<uint8_t>> Marshal(const google::protobuf::Message& msg) const;
  Status Unmarshal(std::span<const uint8_t> data, google::protobuf::Message* msg) const;

  // UnmarshalDescriptor decodes into a new message of type desc, for a caller
  // with a descriptor and no generated class. The message owns what it needs
  // and desc must outlive it.
  StatusOr<std::shared_ptr<google::protobuf::Message>> UnmarshalDescriptor(
      std::span<const uint8_t> data, const google::protobuf::Descriptor* desc) const;

  // Stream encoder and decoder over w and r. The codec and the stream must
  // outlive them.
  Encoder NewEncoder(std::ostream* w) const;
  Decoder NewDecoder(std::istream* r) const;

  // Zero-allocation reader over an SBE buffer. The view holds a span into
  // the buffer; strings and bytes returned by it borrow from that buffer.
  // The header, root block and every group header are validated here
  // (HARDENING.md § SBE validation), so the accessors never read past the
  // buffer.
  StatusOr<View> NewView(std::span<const uint8_t> data) const;

  // Direct access to a registered template by full name; used by tests.
  const MessageTemplate* TemplateByName(std::string_view full_name) const;

 private:
  Codec() = default;
  Status RegisterMessage(const google::protobuf::Descriptor* md,
                         uint16_t schema_id,
                         uint16_t version);
  Status UnmarshalInto(std::span<const uint8_t> data,
                       const MessageTemplate& tmpl,
                       google::protobuf::Message* msg) const;

  // ValidateGroups walks every group header after the root block and
  // checks it per HARDENING.md § SBE validation, returning the total size
  // of the groups region on success.
  Status ValidateGroups(std::span<const uint8_t> data,
                        const MessageTemplate& tmpl,
                        size_t pos) const;

  std::unordered_map<std::string, std::unique_ptr<MessageTemplate>> by_name_;
  std::unordered_map<uint16_t, MessageTemplate*> by_id_;
  int max_message_size_ = kMaxMessageSize;
  int max_repeated_ = kMaxRepeatedCount;
  check::Validator* validator_ = nullptr;
};

// Encoder writes SBE messages framed with a Simple Open Framing Header.
class Encoder {
 public:
  Status Encode(const google::protobuf::Message& msg);

 private:
  friend class Codec;
  Encoder(const Codec* codec, std::ostream* w) : codec_(codec), w_(w) {}

  const Codec* codec_;
  std::ostream* w_;
};

// Decoder reads SOFH-framed SBE messages.
//
// Decode returns a Status for which IsEOF is true at the end of the stream, and
// IsUnexpectedEOF when it ends inside a frame. A framing error is sticky: every
// later call returns it. A frame whose body does not decode is not, since the
// next frame is at a known boundary.
class Decoder {
 public:
  void SetMaxFrameSize(int64_t n) { max_ = n; }
  Status Decode(google::protobuf::Message* msg);

 private:
  friend class Codec;
  Decoder(const Codec* codec, std::istream* r) : codec_(codec), r_(r) {}

  const Codec* codec_;
  std::istream* r_;
  std::vector<uint8_t> buf_;  // reused; Unmarshal copies what it keeps
  int64_t max_ = kDefaultMaxFrameSize;
  Status err_;
};

// Zero-allocation read-only view over an SBE-encoded message.
class View {
 public:
  // Scalar accessors. Return zero/empty if the field name is unknown.
  bool Bool(std::string_view name) const;
  int64_t Int(std::string_view name) const;
  uint64_t Uint(std::string_view name) const;
  double Float(std::string_view name) const;
  // String reads a fixed-length char field and trims trailing zero padding.
  std::string_view String(std::string_view name) const;
  // Bytes reads a fixed-length bytes field as the full N-byte sub-span — no
  // trim. Returns an empty span if the field name is unknown.
  std::span<const uint8_t> Bytes(std::string_view name) const;

  GroupView Group(std::string_view name) const;
  View Composite(std::string_view name) const;

 private:
  friend class Codec;
  friend class GroupView;
  View(std::span<const uint8_t> data,
       std::span<const uint8_t> block,
       const MessageTemplate* tmpl,
       const std::vector<FieldTemplate>* fields,
       const std::vector<GroupTemplate>* groups)
      : data_(data), block_(block), tmpl_(tmpl), fields_(fields), groups_(groups) {}

  const FieldTemplate* FindField(std::string_view name) const;

  std::span<const uint8_t> data_;
  std::span<const uint8_t> block_;
  const MessageTemplate* tmpl_ = nullptr;
  const std::vector<FieldTemplate>* fields_ = nullptr;
  const std::vector<GroupTemplate>* groups_ = nullptr;
};

class GroupView {
 public:
  size_t Len() const { return count_; }
  View Entry(size_t i) const;

 private:
  friend class View;
  GroupView(std::span<const uint8_t> data,
            size_t block_length,
            size_t count,
            const std::vector<FieldTemplate>* fields)
      : data_(data), block_length_(block_length), count_(count), fields_(fields) {}

  std::span<const uint8_t> data_;
  size_t block_length_ = 0;
  size_t count_ = 0;
  const std::vector<FieldTemplate>* fields_ = nullptr;
};

}  // namespace protowire::sbe

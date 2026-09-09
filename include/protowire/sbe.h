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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "protowire/detail/status.h"
#include "protowire/limits.h"
#include "protowire/sbe/template.h"

namespace protowire::sbe {

class Codec;
class View;
class GroupView;

// CodecOptions carries the HARDENING.md § Mandatory limits the codec's
// decoders enforce, configurable per codec; 0 selects the default in
// protowire/limits.h. max_message_size caps the input to one Unmarshal or
// NewView call; max_repeated_count caps a repeating group's numInGroup,
// checked before any entry is allocated.
struct CodecOptions {
  int max_message_size = 0;
  int max_repeated_count = 0;
};

class Codec {
 public:
  static StatusOr<Codec> New(std::vector<const google::protobuf::FileDescriptor*> files,
                             CodecOptions opts = {});

  StatusOr<std::vector<uint8_t>> Marshal(const google::protobuf::Message& msg) const;
  Status Unmarshal(std::span<const uint8_t> data, google::protobuf::Message* msg) const;

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

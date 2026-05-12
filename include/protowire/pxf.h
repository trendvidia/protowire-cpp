// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// PXF (Proto eXpressive Format) — human-friendly text serialization
// backed by protobuf schemas. C++ port of github.com/trendvidia/protowire's
// encoding/pxf package.
//
//   #include "protowire/pxf.h"
//
//   google::protobuf::Message& msg = ...;  // from a Message Factory
//   protowire::pxf::Unmarshal(text_bytes, msg);
//   auto out = protowire::pxf::Marshal(msg);
//
// The schema is provided implicitly by the Message subclass (compiled-in
// or DynamicMessage). Well-known types (Timestamp, Duration, the wrappers)
// are detected by full name and unwrapped to their natural literals.

#pragma once

#include <string>
#include <string_view>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "protowire/detail/status.h"
#include "protowire/pxf/options.h"
#include "protowire/pxf/parser.h"  // Document, Parse
#include "protowire/pxf/result.h"
#include "protowire/pxf/schema.h"  // ValidateDescriptor, Violation

namespace protowire::pxf {

// --- Decoding --------------------------------------------------------------

Status Unmarshal(std::string_view data, google::protobuf::Message* msg, UnmarshalOptions opts = {});

StatusOr<Result> UnmarshalFull(std::string_view data,
                               google::protobuf::Message* msg,
                               UnmarshalOptions opts = {});

// --- Encoding --------------------------------------------------------------

StatusOr<std::string> Marshal(const google::protobuf::Message& msg, MarshalOptions opts = {});

// --- AST helpers (re-exports from parser.h / format.h) --------------------

// std::string FormatDocument(const Document& doc);  // see pxf/format.h

}  // namespace protowire::pxf

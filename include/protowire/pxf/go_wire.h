// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

#include <string>

#include <google/protobuf/message.h>

namespace protowire::pxf {

// SerializeGoOrder writes msg as protowire-go's deterministic marshal does,
// which orders fields differently from libprotobuf: oneof members come after
// every other field. A packed Any holds these bytes, so one document packs to
// one value in every port. Map entries sort by key.
std::string SerializeGoOrder(const google::protobuf::Message& msg);

}  // namespace protowire::pxf

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#pragma once

namespace protowire::sbe {

// kMaxUint16 is the largest value of every SBE header field: blockLength,
// templateId, schemaId and version in the message header, blockLength and
// numInGroup in a group header. A schema or a message needing a larger value
// cannot be written: narrowing it would write a header that disagrees with
// the bytes after it (protowire-go#250).
inline constexpr int kMaxUint16 = 65535;

}  // namespace protowire::sbe

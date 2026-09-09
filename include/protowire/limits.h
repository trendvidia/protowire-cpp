// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Default decode limits, per draft -01 § Mandatory Limits and the spec
// repo's docs/HARDENING.md § Mandatory limits. Every decoder in this port
// enforces them; all but kMaxVarintBytes are configurable per call through
// the package's options struct (pxf::UnmarshalOptions, pb::UnmarshalOptions,
// sbe::CodecOptions), where 0 selects the default below.

#pragma once

namespace protowire {

// PXF `{` / `[` nesting; PB submessage / map-entry nesting. The root
// message is depth 0 and every descent is one; a document exactly this
// deep is accepted and one deeper is rejected.
inline constexpr int kMaxNestingDepth = 100;

// Total input length to a single decode or parse call, in bytes.
inline constexpr int kMaxMessageSize = 64 << 20;

// Digit count of any single PXF numeric literal bound to an
// arbitrary-precision type, and the magnitude of pxf.Decimal.scale.
inline constexpr int kMaxNumericLiteralDigits = 4096;

// Decoded length of any single PXF b"…" literal, in bytes.
inline constexpr int kMaxBytesLiteralLength = kMaxMessageSize;

// Element count of any repeated field, map, or SBE group.
inline constexpr int kMaxRepeatedCount = kMaxMessageSize;

// Length of a varint. Not configurable.
inline constexpr int kMaxVarintBytes = 10;

}  // namespace protowire

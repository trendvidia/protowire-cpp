// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// What the pb and sbe stream decoders share. The texts are Go's io.EOF and
// io.ErrUnexpectedEOF, since a Status has no error chain to test.
#pragma once

#include <cstddef>
#include <ios>
#include <istream>

#include "protowire/detail/status.h"

namespace protowire {

inline constexpr const char* kErrEOF = "EOF";
inline constexpr const char* kErrUnexpectedEOF = "unexpected EOF";

// IsEOF says a stream decoder stopped at a frame boundary: the stream is
// finished, not broken. A decoder reports this as a Status so that a loop over
// Decode cannot read a message that was never there.
inline bool IsEOF(const Status& st) {
  return st.message() == kErrEOF;
}

// IsUnexpectedEOF says the stream ended inside a frame.
inline bool IsUnexpectedEOF(const Status& st) {
  return st.message() == kErrUnexpectedEOF;
}

namespace detail {

// The text of a stream that failed for a reason other than its end.
inline constexpr const char* kErrRead = "read failed";

enum class ReadEnd { kFull, kEOF, kError };

// ReadFull is Go's io.ReadFull over an istream. *got is the count read. A
// stream that ended on an earlier call is read again, since it may have grown,
// as a Go reader may return more after io.EOF.
inline ReadEnd ReadFull(std::istream& r, char* p, size_t n, size_t* got) {
  *got = 0;
  if (n == 0) return ReadEnd::kFull;
  if (r.eof() && !r.bad()) r.clear();
  r.read(p, static_cast<std::streamsize>(n));
  *got = static_cast<size_t>(r.gcount());
  if (*got == n) return ReadEnd::kFull;
  return r.eof() && !r.bad() ? ReadEnd::kEOF : ReadEnd::kError;
}

}  // namespace detail
}  // namespace protowire

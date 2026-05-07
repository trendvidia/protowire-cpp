// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Dumps a canonical envelope's pb-encoded bytes as hex, for cross-port
// wire-compat checking. The same canonical value is constructed in the
// Go and TypeScript ports.

#include <cstdint>
#include <cstdio>

#include "protowire/envelope.h"
#include "protowire/pb.h"

int main() {
  using protowire::envelope::Envelope;

  Envelope e = Envelope::Err(402, "INSUFFICIENT_FUNDS", "balance too low", {"$3.50", "$10.00"});
  e.data = {0xDE, 0xAD, 0xBE, 0xEF};
  e.error->WithField("amount", "MIN_VALUE", "below minimum", {"10.00"})
      .WithMeta("request_id", "req-123");

  auto bytes = protowire::pb::Marshal(e);
  for (uint8_t b : bytes) std::printf("%02x", b);
  std::printf("\n");
  return 0;
}

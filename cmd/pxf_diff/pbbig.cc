// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Big numbers on the pb wire. Line for line tools/godiff/pbbig.go.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "protowire/pb.h"

namespace {

namespace pb = protowire::pb;
using protowire::Status;

struct BigHolder {
  pb::BigFloat F;
  pb::Decimal D;
  pb::BigInt I;
  std::vector<pb::BigFloat> R;
  PROTOWIRE_FIELDS(BigHolder,
                   PROTOWIRE_FIELD(1, F),
                   PROTOWIRE_FIELD(2, D),
                   PROTOWIRE_FIELD(3, I),
                   PROTOWIRE_FIELD(4, R))
};

constexpr int kBigLimits[] = {0, 10, 2147483647};

std::string Hex(const std::vector<uint8_t>& b) {
  std::string out;
  char s[3];
  for (uint8_t c : b) {
    std::snprintf(s, sizeof s, "%02x", c);
    out += s;
  }
  return out;
}

// big.Float.MantExp: exponent plus the mantissa's bit length, and 0 for a zero.
std::string FloatValue(const pb::BigFloat& f) {
  int64_t bits = 0;
  for (size_t i = 0; i < f.mantissa.size(); ++i) {
    if (f.mantissa[i] == 0) continue;
    bits = static_cast<int64_t>(f.mantissa.size() - i - 1) * 8;
    for (uint8_t top = f.mantissa[i]; top != 0; top >>= 1) ++bits;
    break;
  }
  const int64_t e = bits == 0 ? 0 : f.exponent + bits;
  return std::to_string(f.prec) + (f.negative ? " true " : " false ") + std::to_string(e);
}

void DecodeBig(const std::string& kind,
               const std::vector<uint8_t>& data,
               int limit,
               bool remarshal) {
  BigHolder h;
  pb::UnmarshalOptions opts;
  opts.max_big_float_exponent = limit;
  if (Status st = pb::Unmarshal(data, h, opts); !st.ok()) {
    std::cout << st.message() << "\n";
    return;
  }
  std::string line = "ok";
  const std::string base = kind.substr(0, kind.find(':'));
  if (kind.find(':') != std::string::npos) {
    auto text = pb::FormatDecimal(h.D);
    line += " " + (text.ok() ? *text : std::string(text.status().message()));
  } else {
    auto i = pb::FormatBigInt(h.I);
    line += " " + FloatValue(h.F) + " " + (i.ok() ? *i : std::string(i.status().message()));
    for (const pb::BigFloat& r : h.R) line += " " + FloatValue(r);
  }
  if (remarshal && base != "past") line += " " + Hex(pb::Marshal(h));
  std::cout << line << "\n";
}

void Variants(const std::vector<uint8_t>& row,
              const std::function<void(const std::string&, const std::vector<uint8_t>&)>& emit) {
  emit("whole", row);
  for (size_t n = 0; n < row.size(); ++n) {
    emit("prefix " + std::to_string(n), std::vector<uint8_t>(row.begin(), row.begin() + n));
  }
  for (size_t i = 0; i < row.size(); ++i) {
    for (uint8_t b : {0x00, 0x7f, 0x80, 0xff}) {
      std::vector<uint8_t> c = row;
      c[i] = b;
      emit("byte " + std::to_string(i) + "=" + Hex({b}), c);
    }
  }
}

std::vector<uint8_t> Unhex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

}  // namespace

int RunPbBig(const std::string& file) {
  std::ifstream in(file);
  if (!in) return 2;
  std::string text;
  while (std::getline(in, text)) {
    if (text.empty() || text[0] == '#') continue;
    std::istringstream cols(text);
    std::string kind, label, hex;
    cols >> kind >> label >> hex;
    const std::vector<uint8_t> data = Unhex(hex);
    for (int limit : kBigLimits) {
      std::cout << "=== " << kind << " " << label << " limit " << limit << "\n";
      DecodeBig(kind, data, limit, limit == 0);
    }
    if (kind == "bf" || kind == "int") {
      Variants(data, [&](const std::string& variant, const std::vector<uint8_t>& d) {
        std::cout << "--- " << label << " " << variant << "\n";
        DecodeBig(kind, d, 0, true);
      });
    }
  }
  return 0;
}

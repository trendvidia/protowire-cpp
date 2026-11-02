// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// protowire-go's encoding/pb/utf8_test.go.

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "protowire/detail/stream.h"
#include "protowire/detail/wire.h"
#include "protowire/pb.h"

namespace {

namespace pb = protowire::pb;
namespace wire = protowire::wire;
using Bytes = std::vector<uint8_t>;

struct Inner {
  std::string Text;
  PROTOWIRE_FIELDS(Inner, PROTOWIRE_FIELD(1, Text))
};

struct Msg {
  std::string Name;
  std::unique_ptr<std::string> Ptr;
  std::vector<std::string> List;
  Inner Nested;
  std::map<std::string, std::string> Labels;
  Bytes Data;
  PROTOWIRE_FIELDS(Msg,
                   PROTOWIRE_FIELD(1, Name),
                   PROTOWIRE_FIELD(2, Ptr),
                   PROTOWIRE_FIELD(4, List),
                   PROTOWIRE_FIELD(5, Nested),
                   PROTOWIRE_FIELD(6, Labels),
                   PROTOWIRE_FIELD(7, Data))
};

Bytes Len(Bytes b, uint32_t num, const std::string& v) {
  wire::AppendTag(b, num, wire::kBytes);
  wire::AppendBytes(b, Bytes(v.begin(), v.end()));
  return b;
}

std::string Str(const Bytes& b) {
  return std::string(b.begin(), b.end());
}

std::string Frame(const Bytes& body) {
  Bytes out;
  wire::AppendVarint(out, body.size());
  out.insert(out.end(), body.begin(), body.end());
  return Str(out);
}

struct Site {
  const char* name;
  std::function<Bytes(const std::string&)> wire;
  const char* want;
};

// HARDENING.md § UTF-8 names PB length-delimited bytes. Each place a string can
// sit, and each way a byte sequence can be invalid.
TEST(PbUtf8, StringFieldRejectsInvalid) {
  const auto entry = [](const std::string& key, const std::string& value) {
    return Str(Len(Len({}, 1, key), 2, value));
  };
  const std::vector<Site> sites = {
      {"singular",
       [](const std::string& s) { return Len({}, 1, s); },
       "field Name: invalid UTF-8 in string"},
      {"pointer",
       [](const std::string& s) { return Len({}, 2, s); },
       "field Ptr: invalid UTF-8 in string"},
      {"list element",
       [](const std::string& s) { return Len(Len({}, 4, "ok"), 4, s); },
       "field List: invalid UTF-8 in string"},
      {"nested",
       [](const std::string& s) { return Len({}, 5, Str(Len({}, 1, s))); },
       "field Nested: field Text: invalid UTF-8 in string"},
      {"map key",
       [&](const std::string& s) { return Len({}, 6, entry(s, "v")); },
       "field Labels: map key: invalid UTF-8 in string"},
      {"map value",
       [&](const std::string& s) { return Len({}, 6, entry("k", s)); },
       "field Labels: map value: invalid UTF-8 in string"},
  };
  const std::vector<std::string> sequences = {
      "AB\xff",            // stray byte
      "\x80",              // lone continuation
      "caf\xe9",           // latin-1
      "abc\xc3",           // cut off
      "\xc0\x80",          // overlong
      "\xed\xa0\x80",      // surrogate
      "\xf4\x90\x80\x80",  // past U+10FFFF
  };
  for (const Site& site : sites) {
    for (const std::string& bad : sequences) {
      const Bytes data = site.wire(bad);
      Msg m;
      EXPECT_EQ(pb::Unmarshal(data, m).message(), site.want) << site.name;

      std::istringstream in(Frame(data));
      pb::Decoder dec(&in);
      Msg framed;
      EXPECT_EQ(dec.Decode(framed).message(), site.want) << site.name;
    }
  }
}

// The check must not refuse what the rule allows.
TEST(PbUtf8, StringFieldAcceptsValid) {
  Msg want;
  want.Name = "é";
  want.Ptr = std::make_unique<std::string>("€");
  want.List = {std::string("a\0b", 3), ""};
  want.Nested.Text = "日本語";
  want.Labels = {{"ключ", "значение"}};
  want.Data = {0xff, 0xfe, 0x80, 0xc0};
  Msg got;
  ASSERT_TRUE(pb::Unmarshal(pb::Marshal(want), got).ok());
  EXPECT_EQ(got.Name, want.Name);
  ASSERT_NE(got.Ptr, nullptr);
  EXPECT_EQ(*got.Ptr, *want.Ptr);
  EXPECT_EQ(got.List, want.List);
  EXPECT_EQ(got.Nested.Text, want.Nested.Text);
  EXPECT_EQ(got.Labels, want.Labels);
  EXPECT_EQ(got.Data, want.Data);
}

// Bad text is a bad message, not a bad stream.
TEST(PbUtf8, DecoderContinuesPastInvalid) {
  std::istringstream in(Frame(Len({}, 1, "AB\xff")) + Frame(Len({}, 1, "good")));
  pb::Decoder dec(&in);
  Msg first;
  EXPECT_EQ(dec.Decode(first).message(), "field Name: invalid UTF-8 in string");
  Msg next;
  ASSERT_TRUE(dec.Decode(next).ok());
  EXPECT_EQ(next.Name, "good");
  Msg last;
  EXPECT_TRUE(protowire::IsEOF(dec.Decode(last)));
}

}  // namespace

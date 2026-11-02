// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Ported from protowire-go's encoding/pb/stream_test.go.
#include <gtest/gtest.h>

#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "protowire/pb.h"

namespace {

using protowire::IsEOF;
using protowire::IsUnexpectedEOF;
using protowire::Status;
using protowire::pb::Decoder;
using protowire::pb::Encoder;
using protowire::pb::IsFramingCorrupt;

struct StreamMsg {
  int32_t n = 0;
  std::string text;
  std::vector<uint8_t> data;
  PROTOWIRE_FIELDS(StreamMsg,
                   PROTOWIRE_FIELD(1, n),
                   PROTOWIRE_FIELD(2, text),
                   PROTOWIRE_FIELD(3, data))
};

std::string Varint(uint64_t v) {
  std::vector<uint8_t> out;
  protowire::wire::AppendVarint(out, v);
  return std::string(out.begin(), out.end());
}

std::string Repeat(const std::string& s, int n) {
  std::string out;
  for (int i = 0; i < n; ++i) out += s;
  return out;
}

TEST(PbStream, RoundTrip) {
  std::vector<StreamMsg> msgs(3);
  msgs[0].n = 1;
  msgs[0].text = "small";
  msgs[1].n = 2;
  msgs[1].text = Repeat("medium ", 128);
  msgs[2].n = 3;
  msgs[2].data.assign(1 << 20, 0xab);

  std::stringstream buf;
  Encoder enc(&buf);
  for (const auto& m : msgs) ASSERT_TRUE(enc.Encode(m).ok());

  Decoder dec(&buf);
  for (const auto& want : msgs) {
    StreamMsg got;
    ASSERT_TRUE(dec.Decode(got).ok());
    EXPECT_EQ(got.n, want.n);
    EXPECT_EQ(got.text, want.text);
    EXPECT_EQ(got.data, want.data);
  }
  StreamMsg last;
  EXPECT_TRUE(IsEOF(dec.Decode(last)));
}

TEST(PbStream, CleanEOF) {
  std::stringstream empty;
  Decoder dec(&empty);
  StreamMsg m;
  EXPECT_TRUE(IsEOF(dec.Decode(m)));
  EXPECT_TRUE(IsEOF(dec.Decode(m)));  // an end of stream is not sticky
}

TEST(PbStream, ReadsWhatArrivesAfterEOF) {
  std::stringstream s;
  Decoder dec(&s);
  StreamMsg m;
  ASSERT_TRUE(IsEOF(dec.Decode(m)));
  s.clear();
  StreamMsg sent;
  sent.n = 9;
  ASSERT_TRUE(Encoder(&s).Encode(sent).ok());
  ASSERT_TRUE(dec.Decode(m).ok());
  EXPECT_EQ(m.n, 9);
}

TEST(PbStream, PartialPrefix) {
  std::stringstream s(std::string("\x80"));
  Decoder dec(&s);
  StreamMsg m;
  Status st = dec.Decode(m);
  EXPECT_TRUE(IsUnexpectedEOF(st)) << st.message();
  EXPECT_EQ(dec.Decode(m).message(), st.message());
}

TEST(PbStream, PartialBody) {
  std::stringstream s(Varint(100) + std::string(10, '\0'));
  Decoder dec(&s);
  StreamMsg m;
  Status st = dec.Decode(m);
  EXPECT_TRUE(IsUnexpectedEOF(st)) << st.message();
  EXPECT_EQ(dec.Decode(m).message(), st.message());
}

TEST(PbStream, Oversized) {
  std::stringstream s(Varint(1 << 30));
  Decoder dec(&s);
  dec.SetMaxFrameSize(1 << 20);
  StreamMsg m;
  Status st = dec.Decode(m);
  EXPECT_TRUE(IsFramingCorrupt(st)) << st.message();
  EXPECT_EQ(st.message(), "pb: stream framing corrupt: frame size 1073741824 exceeds max 1048576");
  EXPECT_TRUE(IsFramingCorrupt(dec.Decode(m)));
}

TEST(PbStream, DefaultCapIsBelowMaxMessageSize) {
  std::stringstream s(Varint(protowire::pb::kDefaultMaxFrameSize + 1));
  Decoder dec(&s);
  StreamMsg m;
  EXPECT_TRUE(IsFramingCorrupt(dec.Decode(m)));
}

TEST(PbStream, VarintOverflow) {
  std::stringstream s(std::string(11, '\xff'));
  Decoder dec(&s);
  StreamMsg m;
  Status st = dec.Decode(m);
  EXPECT_EQ(st.message(), "pb: stream framing corrupt: binary: varint overflows a 64-bit integer");
}

TEST(PbStream, InnerUnmarshalRecoverable) {
  // A frame with a corrupt body doesn't desync the framing: the next frame is
  // at a known boundary, so the caller can keep going.
  StreamMsg good;
  good.n = 7;
  good.text = "ok";

  std::stringstream s;
  Encoder enc(&s);
  ASSERT_TRUE(enc.Encode(good).ok());
  s << Varint(4) << std::string(4, '\xff');
  ASSERT_TRUE(enc.Encode(good).ok());

  Decoder dec(&s);
  StreamMsg got;
  ASSERT_TRUE(dec.Decode(got).ok());
  StreamMsg bad;
  Status st = dec.Decode(bad);
  ASSERT_FALSE(st.ok());
  EXPECT_FALSE(IsFramingCorrupt(st)) << st.message();
  StreamMsg third;
  ASSERT_TRUE(dec.Decode(third).ok());
  EXPECT_EQ(third.n, 7);
  EXPECT_EQ(third.text, "ok");
}

TEST(PbStream, ScratchReuse) {
  std::stringstream s;
  Encoder enc(&s);
  StreamMsg m;
  m.text = std::string(100, 'x');
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(enc.Encode(m).ok());
  // The body outlives the buffer it was read from.
  Decoder dec(&s);
  StreamMsg a, b;
  ASSERT_TRUE(dec.Decode(a).ok());
  ASSERT_TRUE(dec.Decode(b).ok());
  EXPECT_EQ(a.text, m.text);
  EXPECT_EQ(b.text, m.text);
}

TEST(PbStream, LengthBoundaries) {
  for (int size : {0, 1, 127, 128, 16383, 16384, 1 << 17}) {
    std::stringstream s;
    StreamMsg m;
    m.data.assign(size, 0xcc);
    ASSERT_TRUE(Encoder(&s).Encode(m).ok());
    Decoder dec(&s);
    StreamMsg got;
    ASSERT_TRUE(dec.Decode(got).ok()) << size;
    EXPECT_EQ(got.data, m.data) << size;
  }
}

TEST(PbStream, OptionsReachEachFrame) {
  StreamMsg m;
  m.text = std::string(100, 'x');
  std::stringstream s;
  ASSERT_TRUE(Encoder(&s).Encode(m).ok());
  protowire::pb::UnmarshalOptions opts;
  opts.max_message_size = 10;
  Decoder dec(&s, opts);
  StreamMsg got;
  Status st = dec.Decode(got);
  EXPECT_NE(st.message().find("exceeds MaxMessageSize=10"), std::string::npos) << st.message();
}

// A read that fails is not an end of stream. A streambuf that throws sets the
// stream's badbit, as a failed socket or disk read does.
class FailingBuf : public std::streambuf {
 protected:
  int_type underflow() override { throw std::ios_base::failure("disk gone"); }
};

TEST(PbStream, ReadErrorIsNotEOF) {
  FailingBuf buf;
  std::istream r(&buf);
  Decoder dec(&r);
  StreamMsg m;
  Status st = dec.Decode(m);
  EXPECT_FALSE(IsEOF(st));
  EXPECT_EQ(st.message(), "pb: stream framing corrupt: read failed");
}

TEST(PbStream, WriteErrorIsReported) {
  std::ostringstream w;
  w.setstate(std::ios::failbit);
  EXPECT_EQ(Encoder(&w).Encode(StreamMsg{}).message(), "write failed");
}

// Each of these failed or passed differently from Go, which the stream
// differential found; it runs them through Decode, this runs them directly.
Status UnmarshalBytes(std::vector<uint8_t> data) {
  StreamMsg m;
  return protowire::pb::Unmarshal(data, m);
}

TEST(PbGoParity, FieldNumberZeroIsACorruptTag) {
  EXPECT_EQ(UnmarshalBytes({0x00, 0x00}).message(), "corrupt tag");
}

TEST(PbGoParity, TenthVarintByteMustFit) {
  std::vector<uint8_t> value(9, 0xff);
  std::vector<uint8_t> data = {0x08};
  data.insert(data.end(), value.begin(), value.end());
  data.push_back(0x02);  // past 64 bits
  EXPECT_EQ(UnmarshalBytes(data).message(), "field n: corrupt int");
  data.back() = 0x01;
  EXPECT_TRUE(UnmarshalBytes(data).ok());
}

TEST(PbGoParity, UnknownGroupIsSkipped) {
  // Field 15: start group, one varint field inside, end group; then field 1.
  EXPECT_TRUE(UnmarshalBytes({0x7b, 0x08, 0x01, 0x7c, 0x08, 0x05}).ok());
  EXPECT_EQ(UnmarshalBytes({0x7b, 0x08, 0x01, 0x74}).message(), "corrupt field 15");  // wrong end
  EXPECT_EQ(UnmarshalBytes({0x7c}).message(), "corrupt field 15");  // an end with no start
}

TEST(PbGoParity, DeepGroupsAreBounded) {
  // Past the limit: this overflowed the stack on Windows when it recursed.
  std::vector<uint8_t> data(200000, 0x7b);
  EXPECT_EQ(UnmarshalBytes(data).message(), "corrupt field 15");

  // Go's limit, from running protowire.ConsumeFieldValue: 10001 groups nest
  // and 10002 do not.
  const auto nested = [](size_t n) {
    std::vector<uint8_t> out(n, 0x7b);
    out.insert(out.end(), n, 0x7c);
    return out;
  };
  EXPECT_TRUE(UnmarshalBytes(nested(10001)).ok());
  EXPECT_EQ(UnmarshalBytes(nested(10002)).message(), "corrupt field 15");
}

}  // namespace

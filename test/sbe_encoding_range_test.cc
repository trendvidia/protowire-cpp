// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// An (sbe.encoding) primitive narrows a numeric field, and a value outside
// either range is refused rather than cut to its low bits (draft -01 § SBE
// Annotations, "Emitters MUST reject values that fall outside the narrowed
// type's range"). Ported from protowire-go's
// encoding/sbe/encoding_range_test.go (protowire-go#252).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "protowire/sbe.h"
#include "sbe_files.h"

namespace {

namespace pb = google::protobuf;
using protowire::sbe::Codec;
using protowire_test::SbeFiles;

constexpr int64_t kMaxI32 = std::numeric_limits<int32_t>::max();
constexpr int64_t kMinI32 = std::numeric_limits<int32_t>::min();
constexpr int64_t kMaxI64 = std::numeric_limits<int64_t>::max();
constexpr uint64_t kMaxU32 = std::numeric_limits<uint32_t>::max();
constexpr double kMaxF32 = std::numeric_limits<float>::max();

bool RangeFile(SbeFiles* files, const std::string& body) {
  return files->Compile(
      {{"bounds.proto",
        "syntax = \"proto3\";\npackage bounds.v1;\nimport \"sbe/annotations.proto\";\noption "
        "(sbe.schema_id) = 1;\noption (sbe.version) = 0;\n" +
            body + "\n"}});
}

// RangeCodec binds one message M whose only field v is declared by decl.
struct RangeCodec {
  SbeFiles files;
  std::unique_ptr<Codec> codec;
  const pb::Descriptor* md = nullptr;
  const pb::FieldDescriptor* f = nullptr;

  bool Bind(const std::string& decl, std::string* err) {
    if (!RangeFile(
            &files,
            "enum K { K0 = 0; } message M { option (sbe.template_id) = 1; " + decl + "; }")) {
      *err = files.error();
      return false;
    }
    auto c = Codec::New(files.files());
    if (!c.ok()) {
      *err = c.status().ToString();
      return false;
    }
    codec = std::make_unique<Codec>(std::move(c).consume());
    md = files.Message("bounds.v1.M");
    f = md->FindFieldByName("v");
    return true;
  }
};

// An integer value of the kind a field holds, set and read through
// reflection, and printed as Go's %v prints it.
struct IntVal {
  enum Kind { kI32, kI64, kU32, kU64, kEnum } kind;
  int64_t i = 0;
  uint64_t u = 0;

  void Set(pb::Message* m, const pb::FieldDescriptor* fd) const {
    const pb::Reflection* r = m->GetReflection();
    switch (kind) {
      case kI32:
        r->SetInt32(m, fd, static_cast<int32_t>(i));
        break;
      case kI64:
        r->SetInt64(m, fd, i);
        break;
      case kU32:
        r->SetUInt32(m, fd, static_cast<uint32_t>(u));
        break;
      case kU64:
        r->SetUInt64(m, fd, u);
        break;
      case kEnum:
        r->SetEnumValue(m, fd, static_cast<int>(i));
        break;
    }
  }
  std::string Get(const pb::Message& m, const pb::FieldDescriptor* fd) const {
    const pb::Reflection* r = m.GetReflection();
    switch (kind) {
      case kI32:
        return std::to_string(r->GetInt32(m, fd));
      case kI64:
        return std::to_string(r->GetInt64(m, fd));
      case kU32:
        return std::to_string(r->GetUInt32(m, fd));
      case kU64:
        return std::to_string(r->GetUInt64(m, fd));
      case kEnum:
        return std::to_string(r->GetEnumValue(m, fd));
    }
    return "";
  }
  std::string Str() const {
    return kind == kU32 || kind == kU64 ? std::to_string(u) : std::to_string(i);
  }
};

IntVal I32(int64_t v) {
  return {IntVal::kI32, v, 0};
}
IntVal I64(int64_t v) {
  return {IntVal::kI64, v, 0};
}
IntVal U32(uint64_t v) {
  return {IntVal::kU32, 0, v};
}
IntVal U64(uint64_t v) {
  return {IntVal::kU64, 0, v};
}
IntVal Enum(int64_t v) {
  return {IntVal::kEnum, v, 0};
}

// TestMarshalEncodingRange: before the fix Marshal kept the low bits, so int8
// 300 was written as 44 and read back as 44 with no error. Every refused
// value is paired with the value at the bound, which must round-trip exactly.
TEST(SbeEncodingRange, Marshal) {
  struct Row {
    std::string decl;
    std::vector<IntVal> ok;
    std::vector<IntVal> refuse;
    std::string want;  // the refusal's range, as the error states it
  };
  const std::vector<Row> rows = {
      {R"(int32 v = 1 [(sbe.encoding) = "int8"])",
       {I32(127), I32(-128)},
       {I32(128), I32(-129), I32(300)},
       "int8, [-128, 127]"},
      {R"(int32 v = 1 [(sbe.encoding) = "int16"])",
       {I32(32767), I32(-32768)},
       {I32(32768), I32(-32769)},
       "int16, [-32768, 32767]"},
      {R"(int64 v = 1 [(sbe.encoding) = "int32"])",
       {I64(kMaxI32), I64(kMinI32)},
       {I64(kMaxI32 + 1), I64(kMinI32 - 1)},
       "int32, [-2147483648, 2147483647]"},
      {R"(uint64 v = 1 [(sbe.encoding) = "int64"])",
       {U64(kMaxI64)},
       {U64(uint64_t{kMaxI64} + 1)},
       "int64, [-9223372036854775808, 9223372036854775807]"},
      {R"(uint32 v = 1 [(sbe.encoding) = "uint8"])", {U32(255)}, {U32(256)}, "uint8, [0, 255]"},
      {R"(uint32 v = 1 [(sbe.encoding) = "uint16"])",
       {U32(65535)},
       {U32(65536), U32(70000)},
       "uint16, [0, 65535]"},
      {R"(uint64 v = 1 [(sbe.encoding) = "uint32"])",
       {U64(kMaxU32)},
       {U64(kMaxU32 + 1)},
       "uint32, [0, 4294967295]"},
      {R"(int64 v = 1 [(sbe.encoding) = "uint64"])",
       {I64(0), I64(kMaxI64)},
       {I64(-1)},
       "uint64, [0, 18446744073709551615]"},
      {R"(sint32 v = 1 [(sbe.encoding) = "uint8"])",
       {I32(0), I32(255)},
       {I32(-1), I32(256)},
       "uint8, [0, 255]"},
      {R"(uint64 v = 1 [(sbe.encoding) = "int8"])", {U64(127)}, {U64(128)}, "int8, [-128, 127]"},
      // An enum with no annotation is laid out as uint8.
      {"K v = 1", {Enum(0), Enum(255)}, {Enum(256), Enum(-1)}, "uint8, [0, 255]"},
      {R"(K v = 1 [(sbe.encoding) = "int16"])",
       {Enum(-32768), Enum(32767)},
       {Enum(32768)},
       "int16, [-32768, 32767]"},
  };
  for (const Row& row : rows) {
    SCOPED_TRACE(row.decl);
    RangeCodec rc;
    std::string err;
    if (!rc.Bind(row.decl, &err)) {
      ADD_FAILURE() << err;
      continue;
    }
    for (const IntVal& v : row.ok) {
      auto msg = rc.files.New(rc.md);
      v.Set(msg.get(), rc.f);
      auto data = rc.codec->Marshal(*msg);
      if (!data.ok()) {
        ADD_FAILURE() << "value " << v.Str() << ": " << data.status().ToString();
        continue;
      }
      auto back = rc.codec->UnmarshalDescriptor(*data, rc.md);
      if (!back.ok()) {
        ADD_FAILURE() << "value " << v.Str() << ": " << back.status().ToString();
        continue;
      }
      EXPECT_EQ(v.Get(**back, rc.f), v.Str());
    }
    for (const IntVal& v : row.refuse) {
      auto msg = rc.files.New(rc.md);
      v.Set(msg.get(), rc.f);
      auto data = rc.codec->Marshal(*msg);
      if (data.ok()) {
        ADD_FAILURE() << "value " << v.Str() << " was written";
        continue;
      }
      EXPECT_EQ(
          data.status().message(),
          "sbe: field bounds.v1.M.v: value " + v.Str() + " is outside the range of " + row.want);
    }
  }
}

// TestMarshalFloatRange: a double narrowed to float must be within ±max
// float. The infinities and NaN are float values and pass; a finite double
// past the bound is refused even when it would round down to it.
TEST(SbeEncodingRange, MarshalFloat) {
  RangeCodec rc;
  std::string err;
  ASSERT_TRUE(rc.Bind(R"(double v = 1 [(sbe.encoding) = "float"])", &err)) << err;
  const double past = std::nextafter(kMaxF32, std::numeric_limits<double>::infinity());
  const double inf = std::numeric_limits<double>::infinity();
  for (double v : {kMaxF32, -kMaxF32, inf, -inf, std::nan(""), 0.1}) {
    SCOPED_TRACE(v);
    auto msg = rc.files.New(rc.md);
    msg->GetReflection()->SetDouble(msg.get(), rc.f, v);
    auto data = rc.codec->Marshal(*msg);
    ASSERT_TRUE(data.ok()) << data.status().ToString();
    auto back = rc.codec->UnmarshalDescriptor(*data, rc.md);
    ASSERT_TRUE(back.ok()) << back.status().ToString();
    const double got = (*back)->GetReflection()->GetDouble(**back, rc.f);
    if (std::isnan(v)) {
      EXPECT_TRUE(std::isnan(got));
    } else {
      EXPECT_EQ(got, static_cast<double>(static_cast<float>(v)));
    }
  }
  for (double v : {past, -past, 1e39}) {
    SCOPED_TRACE(v);
    auto msg = rc.files.New(rc.md);
    msg->GetReflection()->SetDouble(msg.get(), rc.f, v);
    auto data = rc.codec->Marshal(*msg);
    ASSERT_FALSE(data.ok());
    EXPECT_NE(data.status().message().find("sbe: field bounds.v1.M.v: value"), std::string::npos);
    EXPECT_NE(data.status().message().find("is outside the range of float"), std::string::npos);
  }
  // The text is Go's %g: 1e39 and ±3.4028234663852886e+38.
  auto msg = rc.files.New(rc.md);
  msg->GetReflection()->SetDouble(msg.get(), rc.f, 1e39);
  EXPECT_EQ(rc.codec->Marshal(*msg).status().message(),
            "sbe: field bounds.v1.M.v: value 1e+39 is outside the range of float, "
            "±3.4028234663852886e+38");
}

// TestMarshalEncodingRangeNested: the refusal holds in a composite and in a
// group entry, and names the field there.
TEST(SbeEncodingRange, MarshalNested) {
  SbeFiles files;
  ASSERT_TRUE(RangeFile(&files,
                        R"(message C { int32 c = 1 [(sbe.encoding) = "int8"]; }
message E { uint32 e = 1 [(sbe.encoding) = "uint8"]; }
message M { option (sbe.template_id) = 1; C comp = 1; repeated E grp = 2; })"))
      << files.error();
  auto codec = Codec::New(files.files());
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  const pb::Descriptor* md = files.Message("bounds.v1.M");

  const auto build = [&](int32_t c, uint32_t e) {
    auto msg = files.New(md);
    const pb::Reflection* r = msg->GetReflection();
    pb::Message* comp = r->MutableMessage(msg.get(), md->FindFieldByName("comp"));
    comp->GetReflection()->SetInt32(comp, comp->GetDescriptor()->FindFieldByName("c"), c);
    pb::Message* el = r->AddMessage(msg.get(), md->FindFieldByName("grp"));
    el->GetReflection()->SetUInt32(el, el->GetDescriptor()->FindFieldByName("e"), e);
    return msg;
  };
  EXPECT_TRUE(codec->Marshal(*build(127, 255)).ok());
  EXPECT_EQ(codec->Marshal(*build(128, 255)).status().message(),
            "sbe: field bounds.v1.C.c: value 128 is outside the range of int8, [-128, 127]");
  EXPECT_EQ(codec->Marshal(*build(127, 256)).status().message(),
            "sbe: field bounds.v1.E.e: value 256 is outside the range of uint8, [0, 255]");
}

uint64_t DoubleBits(double d) {
  uint64_t b = 0;
  std::memcpy(&b, &d, 8);
  return b;
}

// TestUnmarshalEncodingRange: a primitive whose values the field cannot all
// hold is refused on read when the value does not fit, rather than cut to
// the field's low bits. Each refusal is paired with the wire value at the
// bound. The bytes are what another emitter could write; this codec's
// Marshal never does.
TEST(SbeEncodingRange, Unmarshal) {
  struct Row {
    std::string decl;
    std::vector<uint64_t> ok;  // primitive bits, little-endian in the block
    std::vector<uint64_t> refuse;
  };
  const auto neg = [](int64_t v) { return static_cast<uint64_t>(v); };
  const std::vector<Row> rows = {
      {R"(int32 v = 1 [(sbe.encoding) = "int64"])",
       {kMaxI32, neg(kMinI32)},
       {kMaxI32 + 1, neg(kMinI32 - 1)}},
      {R"(int32 v = 1 [(sbe.encoding) = "uint32"])", {kMaxI32}, {kMaxI32 + 1, kMaxU32}},
      {R"(uint32 v = 1 [(sbe.encoding) = "int8"])", {0x7f, 0}, {0xff, 0x80}},
      {R"(uint32 v = 1 [(sbe.encoding) = "uint64"])", {kMaxU32}, {kMaxU32 + 1}},
      {R"(uint64 v = 1 [(sbe.encoding) = "int64"])", {kMaxI64}, {neg(-1)}},
      {R"(int64 v = 1 [(sbe.encoding) = "uint64"])", {kMaxI64}, {uint64_t{kMaxI64} + 1}},
      {R"(K v = 1 [(sbe.encoding) = "uint32"])", {kMaxI32}, {kMaxI32 + 1}},
      {R"(float v = 1 [(sbe.encoding) = "double"])",
       {DoubleBits(kMaxF32), DoubleBits(-std::numeric_limits<double>::infinity())},
       {DoubleBits(1e39), DoubleBits(-1e39)}},
  };
  for (const Row& row : rows) {
    SCOPED_TRACE(row.decl);
    RangeCodec rc;
    std::string err;
    if (!rc.Bind(row.decl, &err)) {
      ADD_FAILURE() << err;
      continue;
    }
    auto empty = rc.files.New(rc.md);
    auto data = rc.codec->Marshal(*empty);
    if (!data.ok()) {
      ADD_FAILURE() << data.status().ToString();
      continue;
    }
    const size_t size = data->size() - 8;
    const auto put = [&](uint64_t bits) {
      std::vector<uint8_t> b = *data;
      for (size_t i = 0; i < size; ++i) b[8 + i] = static_cast<uint8_t>(bits >> (8 * i));
      return b;
    };
    for (uint64_t bits : row.ok) {
      SCOPED_TRACE(bits);
      auto back = rc.codec->UnmarshalDescriptor(put(bits), rc.md);
      if (!back.ok()) {
        ADD_FAILURE() << back.status().ToString();
        continue;
      }
      // What came back is what Marshal writes again.
      auto again = rc.codec->Marshal(**back);
      if (!again.ok()) {
        ADD_FAILURE() << again.status().ToString();
        continue;
      }
      EXPECT_EQ(*again, put(bits));
    }
    for (uint64_t bits : row.refuse) {
      SCOPED_TRACE(bits);
      auto back = rc.codec->UnmarshalDescriptor(put(bits), rc.md);
      if (back.ok()) {
        ADD_FAILURE() << "decoded";
        continue;
      }
      EXPECT_NE(back.status().message().find("sbe: field bounds.v1.M.v: wire value"),
                std::string::npos)
          << back.status().message();
      EXPECT_NE(back.status().message().find("does not fit a"), std::string::npos)
          << back.status().message();
    }
  }
}

// The full text of each refusal on read, as protowire-go writes it.
TEST(SbeEncodingRange, UnmarshalText) {
  struct Row {
    std::string decl;
    uint64_t bits;
    std::string want;
  };
  const std::vector<Row> rows = {
      {R"(uint32 v = 1 [(sbe.encoding) = "int8"])",
       0xff,
       "sbe: field bounds.v1.M.v: wire value -1 (int8) does not fit a uint32 field, [0, "
       "4294967295]"},
      {R"(int32 v = 1 [(sbe.encoding) = "uint32"])",
       kMaxU32,
       "sbe: field bounds.v1.M.v: wire value 4294967295 (uint32) does not fit a int32 field, "
       "[-2147483648, 2147483647]"},
      {R"(K v = 1 [(sbe.encoding) = "uint32"])",
       kMaxI32 + 1,
       "sbe: field bounds.v1.M.v: wire value 2147483648 (uint32) does not fit a enum field, "
       "[-2147483648, 2147483647]"},
      {R"(float v = 1 [(sbe.encoding) = "double"])",
       DoubleBits(-1e39),
       "sbe: field bounds.v1.M.v: wire value -1e+39 does not fit a float field, "
       "±3.4028234663852886e+38"},
  };
  for (const Row& row : rows) {
    SCOPED_TRACE(row.decl);
    RangeCodec rc;
    std::string err;
    if (!rc.Bind(row.decl, &err)) {
      ADD_FAILURE() << err;
      continue;
    }
    auto data = rc.codec->Marshal(*rc.files.New(rc.md));
    if (!data.ok()) {
      ADD_FAILURE() << data.status().ToString();
      continue;
    }
    for (size_t i = 0; i + 8 < data->size(); ++i) {
      (*data)[8 + i] = static_cast<uint8_t>(row.bits >> (8 * i));
    }
    EXPECT_EQ(rc.codec->UnmarshalDescriptor(*data, rc.md).status().message(), row.want);
  }
}

// TestEncodingKindBind: (sbe.encoding) narrows a numeric field. A string or
// bytes field, and a crossing between integers and floating point, are
// refused when the codec is built.
TEST(SbeEncodingRange, KindBind) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {R"(int32 v = 1 [(sbe.encoding) = "float"])",
       R"(field bounds.v1.M.v: (sbe.encoding) = "float" is a floating-point primitive and the field is int32)"},
      {R"(K v = 1 [(sbe.encoding) = "double"])",
       R"(field bounds.v1.M.v: (sbe.encoding) = "double" is a floating-point primitive and the field is enum)"},
      {R"(bool v = 1 [(sbe.encoding) = "float"])",
       R"(field bounds.v1.M.v: (sbe.encoding) = "float" is a floating-point primitive and the field is bool)"},
      {R"(double v = 1 [(sbe.encoding) = "int32"])",
       R"(field bounds.v1.M.v: (sbe.encoding) = "int32" is an integer primitive and the field is double)"},
      {R"(float v = 1 [(sbe.encoding) = "uint8"])",
       R"(field bounds.v1.M.v: (sbe.encoding) = "uint8" is an integer primitive and the field is float)"},
      {R"(string v = 1 [(sbe.length) = 4, (sbe.encoding) = "uint8"])",
       R"(field bounds.v1.M.v: (sbe.encoding) = "uint8" on a string field)"},
      {R"(bytes v = 1 [(sbe.length) = 4, (sbe.encoding) = "int32"])",
       R"(field bounds.v1.M.v: (sbe.encoding) = "int32" on a bytes field)"},
      {R"(double v = 1 [(sbe.encoding) = "char"])",
       R"(field bounds.v1.M.v: unknown encoding "char")"},
  };
  for (const auto& [decl, want] : cases) {
    SCOPED_TRACE(decl);
    SbeFiles files;
    if (!RangeFile(
            &files,
            "enum K { K0 = 0; } message M { option (sbe.template_id) = 1; " + decl + "; }")) {
      ADD_FAILURE() << files.error();
      continue;
    }
    auto codec = Codec::New(files.files());
    if (codec.ok()) {
      ADD_FAILURE() << "bound";
      continue;
    }
    EXPECT_NE(codec.status().message().find(want), std::string::npos) << codec.status().message();
  }
}

// TestEncodingMatrix pins every pairing of an integer, bool, enum or
// floating field with every (sbe.encoding): it is refused when the codec is
// built, or the value 1 round-trips.
TEST(SbeEncodingRange, Matrix) {
  const std::vector<std::string> kinds = {
      "int32", "int64", "uint32", "uint64", "sint32", "fixed64", "float", "double", "bool", "K"};
  const std::vector<std::string> encs = {
      "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float", "double"};
  int bound = 0;
  int refused = 0;
  for (const std::string& k : kinds) {
    for (const std::string& e : encs) {
      SCOPED_TRACE(k + "/" + e);
      SbeFiles files;
      if (!RangeFile(&files,
                     "enum K { K0 = 0; K1 = 1; } message M { option (sbe.template_id) = "
                     "1; " +
                         k + " v = 1 [(sbe.encoding) = \"" + e + "\"]; }")) {
        ADD_FAILURE() << files.error();
        continue;
      }
      auto codec = Codec::New(files.files());
      const bool float_kind = k == "float" || k == "double";
      const bool float_enc = e == "float" || e == "double";
      if (float_kind != float_enc) {
        EXPECT_FALSE(codec.ok());
        ++refused;
        continue;
      }
      if (!codec.ok()) {
        ADD_FAILURE() << codec.status().ToString();
        continue;
      }
      ++bound;
      const pb::Descriptor* md = files.Message("bounds.v1.M");
      const pb::FieldDescriptor* f = md->FindFieldByName("v");
      auto msg = files.New(md);
      const pb::Reflection* r = msg->GetReflection();
      switch (f->cpp_type()) {
        case pb::FieldDescriptor::CPPTYPE_INT32:
          r->SetInt32(msg.get(), f, 1);
          break;
        case pb::FieldDescriptor::CPPTYPE_INT64:
          r->SetInt64(msg.get(), f, 1);
          break;
        case pb::FieldDescriptor::CPPTYPE_UINT32:
          r->SetUInt32(msg.get(), f, 1);
          break;
        case pb::FieldDescriptor::CPPTYPE_UINT64:
          r->SetUInt64(msg.get(), f, 1);
          break;
        case pb::FieldDescriptor::CPPTYPE_FLOAT:
          r->SetFloat(msg.get(), f, 1);
          break;
        case pb::FieldDescriptor::CPPTYPE_DOUBLE:
          r->SetDouble(msg.get(), f, 1);
          break;
        case pb::FieldDescriptor::CPPTYPE_BOOL:
          r->SetBool(msg.get(), f, true);
          break;
        default:
          r->SetEnumValue(msg.get(), f, 1);
          break;
      }
      auto data = codec->Marshal(*msg);
      if (!data.ok()) {
        ADD_FAILURE() << data.status().ToString();
        continue;
      }
      auto back = codec->UnmarshalDescriptor(*data, md);
      if (!back.ok()) {
        ADD_FAILURE() << back.status().ToString();
        continue;
      }
      // What came back serializes as what was set, so the value survived the
      // round trip whatever its kind.
      EXPECT_EQ(msg->SerializeAsString(), (*back)->SerializeAsString());
    }
  }
  EXPECT_EQ(bound, 68);
  EXPECT_EQ(refused, 32);
}

}  // namespace

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// SBE XML schema conversion. Ported from protowire-go's convert_test.go; the
// byte-for-byte comparison with Go is scripts/go_differential.sh.

#include "protowire/sbe.h"

#include <gtest/gtest.h>
#include "fixture_dir.h"
#include "protoc_compat.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;
namespace sbe = protowire::sbe;

constexpr const char* kTestSBEXML = R"(<?xml version="1.0" encoding="UTF-8"?>
<sbe:messageSchema xmlns:sbe="http://fixprotocol.io/2016/sbe"
                   package="test.v1"
                   id="1"
                   version="0"
                   byteOrder="littleEndian">
    <types>
        <composite name="messageHeader">
            <type name="blockLength" primitiveType="uint16"/>
            <type name="templateId" primitiveType="uint16"/>
            <type name="schemaId" primitiveType="uint16"/>
            <type name="version" primitiveType="uint16"/>
        </composite>
        <composite name="groupSizeEncoding">
            <type name="blockLength" primitiveType="uint16"/>
            <type name="numInGroup" primitiveType="uint16"/>
        </composite>
        <enum name="Side" encodingType="uint8">
            <validValue name="Buy">0</validValue>
            <validValue name="Sell">1</validValue>
        </enum>
        <type name="str8" primitiveType="char" length="8"/>
        <composite name="Inner">
            <type name="x" primitiveType="int64"/>
            <type name="y" primitiveType="int64"/>
        </composite>
    </types>
    <sbe:message name="Order" id="1">
        <field name="orderId" id="1" type="uint64"/>
        <field name="symbol" id="2" type="str8"/>
        <field name="price" id="3" type="int64"/>
        <field name="quantity" id="4" type="uint32"/>
        <field name="side" id="5" type="Side"/>
        <field name="active" id="6" type="uint8"/>
        <field name="weight" id="7" type="double"/>
        <field name="score" id="8" type="float"/>
        <group name="fills" id="9">
            <field name="fillPrice" id="1" type="int64"/>
            <field name="fillQty" id="2" type="uint32"/>
            <field name="fillId" id="3" type="uint64"/>
        </group>
    </sbe:message>
    <sbe:message name="Simple" id="2">
        <field name="id" id="1" type="uint32"/>
        <field name="value" id="2" type="int32"/>
    </sbe:message>
    <sbe:message name="WithComposite" id="3">
        <field name="id" id="1" type="uint64"/>
        <field name="inner" id="2" type="Inner"/>
        <field name="code" id="3" type="int32"/>
    </sbe:message>
    <sbe:message name="WithNarrow" id="4">
        <field name="status" id="1" type="uint8"/>
        <field name="port" id="2" type="uint16"/>
        <field name="delta" id="3" type="int16"/>
    </sbe:message>
</sbe:messageSchema>)";

constexpr const char* kTestProto = R"(
syntax = "proto3";
package test.v1;
import "sbe/annotations.proto";

option (sbe.schema_id) = 1;
option (sbe.version) = 0;

enum Side {
  SIDE_BUY = 0;
  SIDE_SELL = 1;
}

message Order {
  option (sbe.template_id) = 1;
  uint64 order_id = 1;
  string symbol = 2 [(sbe.length) = 8];
  int64 price = 3;
  uint32 quantity = 4;
  Side side = 5;
  bool active = 6;
  double weight = 7;
  float score = 8;

  message Fill {
    int64 fill_price = 1;
    uint32 fill_qty = 2;
    uint64 fill_id = 3;
  }
  repeated Fill fills = 9;
}

message Simple {
  option (sbe.template_id) = 2;
  uint32 id = 1;
  int32 value = 2;
}

message WithComposite {
  option (sbe.template_id) = 3;
  uint64 id = 1;
  Inner inner = 2;
  int32 code = 3;
}

message Inner {
  int64 x = 1;
  int64 y = 2;
}

message WithNarrow {
  option (sbe.template_id) = 4;
  uint32 status = 1 [(sbe.encoding) = "uint8"];
  uint32 port = 2 [(sbe.encoding) = "uint16"];
  int32 delta = 3 [(sbe.encoding) = "int16"];
}
)";

// fixedArraySchema has the two fixed-length arrays that are not text: a char
// in a non-UTF-8 encoding and a uint8 array, each followed by a field whose
// offset moves if the array's length is lost (protowire-go#249).
constexpr const char* kFixedArraySchema = R"(<?xml version="1.0" encoding="UTF-8"?>
<sbe:messageSchema xmlns:sbe="http://fixprotocol.io/2016/sbe"
                   package="arr.v1" id="3" version="0" byteOrder="littleEndian">
    <types>
        <type name="Latin1" primitiveType="char" length="4" characterEncoding="ISO-8859-1"/>
        <type name="Uuid" primitiveType="uint8" length="16"/>
        <type name="Signed" primitiveType="int8" length="3"/>
        <composite name="Key">
            <type name="raw" primitiveType="uint8" length="6"/>
            <type name="seq" primitiveType="uint32"/>
        </composite>
    </types>
    <sbe:message name="Record" id="1">
        <field name="name" id="1" type="Latin1"/>
        <field name="uuid" id="2" type="Uuid"/>
        <field name="signed" id="3" type="Signed"/>
        <field name="key" id="4" type="Key"/>
        <field name="tail" id="5" type="uint32"/>
    </sbe:message>
</sbe:messageSchema>)";

class Errors : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last += std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg) + "\n";
  }
  std::string last;
};

// Compiled holds one .proto compiled from source, and what keeps it alive.
struct Compiled {
  pb::compiler::DiskSourceTree tree;
  Errors errors;
  std::unique_ptr<pb::compiler::Importer> importer;
  std::unique_ptr<pb::DynamicMessageFactory> factory;
  const pb::FileDescriptor* fd = nullptr;

  std::unique_ptr<pb::Message> New(const char* name) const {
    return std::unique_ptr<pb::Message>(
        factory->GetPrototype(fd->FindMessageTypeByName(name))->New());
  }
};

std::unique_ptr<Compiled> Compile(const std::string& src) {
  static std::atomic<int> n{0};
  const std::string name = "sbe_xml_" + std::to_string(n++) + ".proto";
  std::ofstream(protowire_test::FixtureDir() + "/" + name, std::ios::binary) << src;
  auto c = std::make_unique<Compiled>();
  c->tree.MapPath("", PROTO_DIR);
  c->tree.MapPath("", WKT_PROTO_DIR);
  c->tree.MapPath("", protowire_test::FixtureDir());
  c->importer = std::make_unique<pb::compiler::Importer>(&c->tree, &c->errors);
  c->fd = c->importer->Import(name);
  EXPECT_NE(c->fd, nullptr) << c->errors.last << src;
  if (c->fd == nullptr) return nullptr;
  c->factory = std::make_unique<pb::DynamicMessageFactory>(c->importer->pool());
  return c;
}

std::string MustXMLToProto(const std::string& xml) {
  auto out = sbe::XMLToProto(xml);
  EXPECT_TRUE(out.ok()) << out.status().ToString();
  return out.ok() ? *out : std::string();
}

bool Contains(const std::string& s, const std::string& sub) {
  return s.find(sub) != std::string::npos;
}

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
  for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
    s.replace(at, from.size(), to);
  }
  return s;
}

void Set(pb::Message* m, const char* name, auto v) {
  const pb::FieldDescriptor* fd = m->GetDescriptor()->FindFieldByName(name);
  const pb::Reflection* r = m->GetReflection();
  using V = decltype(v);
  if constexpr (std::is_same_v<V, uint32_t>) r->SetUInt32(m, fd, v);
  if constexpr (std::is_same_v<V, int32_t>) r->SetInt32(m, fd, v);
  if constexpr (std::is_same_v<V, uint64_t>) r->SetUInt64(m, fd, v);
  if constexpr (std::is_same_v<V, int64_t>) r->SetInt64(m, fd, v);
  if constexpr (std::is_same_v<V, double>) r->SetDouble(m, fd, v);
  if constexpr (std::is_same_v<V, float>) r->SetFloat(m, fd, v);
  if constexpr (std::is_same_v<V, std::string>) r->SetString(m, fd, v);
}

TEST(SbeXML, ParseXMLSchema) {
  auto schema = sbe::ParseXMLSchema(kTestSBEXML);
  ASSERT_TRUE(schema.ok()) << schema.status().ToString();
  EXPECT_EQ(schema->package, "test.v1");
  EXPECT_EQ(schema->id, 1u);
  EXPECT_EQ(schema->version, 0u);
  ASSERT_EQ(schema->types.enums.size(), 1u);
  EXPECT_EQ(schema->types.enums[0].name, "Side");
  EXPECT_EQ(schema->messages.size(), 4u);
}

TEST(SbeXML, XMLToProto) {
  const std::string proto = MustXMLToProto(kTestSBEXML);
  for (const char* want : {"option (sbe.schema_id) = 1;",
                           "option (sbe.version) = 0;",
                           "option (sbe.template_id) = 1;",
                           "string symbol = 2 [(sbe.length) = 8];",
                           "Side side = 5;",
                           "Inner inner = 2;",
                           "repeated Fill fills = 9;",
                           "(sbe.encoding) = \"uint8\""}) {
    EXPECT_TRUE(Contains(proto, want)) << want << "\n" << proto;
  }
  auto c = Compile(proto);
  ASSERT_NE(c, nullptr);
  auto codec = sbe::Codec::New({c->fd});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  auto msg = c->New("Simple");
  Set(msg.get(), "id", uint32_t{42});
  Set(msg.get(), "value", int32_t{-100});
  auto data = codec->Marshal(*msg);
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  auto got = c->New("Simple");
  ASSERT_TRUE(codec->Unmarshal(*data, got.get()).ok());
  EXPECT_EQ(got->GetReflection()->GetUInt32(*got, got->GetDescriptor()->FindFieldByName("id")),
            42u);
  EXPECT_EQ(got->GetReflection()->GetInt32(*got, got->GetDescriptor()->FindFieldByName("value")),
            -100);
}

TEST(SbeXML, XMLToProtoComposite) {
  auto c = Compile(MustXMLToProto(kTestSBEXML));
  ASSERT_NE(c, nullptr);
  auto codec = sbe::Codec::New({c->fd});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  auto msg = c->New("WithComposite");
  Set(msg.get(), "id", uint64_t{99});
  pb::Message* inner = msg->GetReflection()->MutableMessage(
      msg.get(), msg->GetDescriptor()->FindFieldByName("inner"));
  Set(inner, "x", int64_t{100});
  Set(inner, "y", int64_t{-200});
  Set(msg.get(), "code", int32_t{42});
  auto data = codec->Marshal(*msg);
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  auto got = c->New("WithComposite");
  ASSERT_TRUE(codec->Unmarshal(*data, got.get()).ok());
  EXPECT_EQ(got->SerializeAsString(), msg->SerializeAsString());
}

TEST(SbeXML, XMLToProtoNarrow) {
  auto c = Compile(MustXMLToProto(kTestSBEXML));
  ASSERT_NE(c, nullptr);
  auto codec = sbe::Codec::New({c->fd});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  auto msg = c->New("WithNarrow");
  Set(msg.get(), "status", uint32_t{200});
  Set(msg.get(), "port", uint32_t{8080});
  Set(msg.get(), "delta", int32_t{-1234});
  auto data = codec->Marshal(*msg);
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  // Header(8) + uint8(1) + uint16(2) + int16(2).
  EXPECT_EQ(data->size(), 13u);
  auto got = c->New("WithNarrow");
  ASSERT_TRUE(codec->Unmarshal(*data, got.get()).ok());
  EXPECT_EQ(got->SerializeAsString(), msg->SerializeAsString());
}

TEST(SbeXML, XMLRoundTrip) {
  auto c = Compile(MustXMLToProto(kTestSBEXML));
  ASSERT_NE(c, nullptr);
  auto codec = sbe::Codec::New({c->fd});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  auto msg = c->New("Order");
  Set(msg.get(), "order_id", uint64_t{1001});
  Set(msg.get(), "symbol", std::string("AAPL"));
  Set(msg.get(), "price", int64_t{19150});
  Set(msg.get(), "quantity", uint32_t{100});
  msg->GetReflection()->SetEnumValue(msg.get(), msg->GetDescriptor()->FindFieldByName("side"), 1);
  // active is uint32 [(sbe.encoding) = "uint8"] after the conversion.
  Set(msg.get(), "active", uint32_t{1});
  Set(msg.get(), "weight", 0.85);
  Set(msg.get(), "score", 3.14f);
  pb::Message* fill =
      msg->GetReflection()->AddMessage(msg.get(), msg->GetDescriptor()->FindFieldByName("fills"));
  Set(fill, "fill_price", int64_t{19155});
  Set(fill, "fill_qty", uint32_t{25});
  Set(fill, "fill_id", uint64_t{5001});
  auto data = codec->Marshal(*msg);
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  auto got = c->New("Order");
  ASSERT_TRUE(codec->Unmarshal(*data, got.get()).ok());
  EXPECT_EQ(got->SerializeAsString(), msg->SerializeAsString());
}

TEST(SbeXML, ProtoToXML) {
  auto c = Compile(kTestProto);
  ASSERT_NE(c, nullptr);
  auto xml = sbe::ProtoToXML({c->fd});
  ASSERT_TRUE(xml.ok()) << xml.status().ToString();
  for (const char* want : {"package=\"test.v1\"",
                           "id=\"1\"",
                           "<sbe:message name=\"Order\" id=\"1\">",
                           "<sbe:message name=\"Simple\" id=\"2\">",
                           "<enum name=\"Side\"",
                           "<composite name=\"Inner\">",
                           "<group name=\"fills\""}) {
    EXPECT_TRUE(Contains(*xml, want)) << want << "\n" << *xml;
  }
  auto schema = sbe::ParseXMLSchema(*xml);
  ASSERT_TRUE(schema.ok()) << schema.status().ToString();
  EXPECT_EQ(schema->package, "test.v1");
  EXPECT_EQ(schema->id, 1u);
  ASSERT_EQ(schema->messages.size(), 4u);
  const sbe::XMLMessage* order = nullptr;
  for (const auto& m : schema->messages) {
    if (m.name == "Order") order = &m;
  }
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->id, 1u);
  ASSERT_EQ(order->groups.size(), 1u);
  EXPECT_EQ(order->groups[0].name, "fills");
}

TEST(SbeXML, ProtoToXMLRoundTrip) {
  auto c = Compile(kTestProto);
  ASSERT_NE(c, nullptr);
  auto xml = sbe::ProtoToXML({c->fd});
  ASSERT_TRUE(xml.ok()) << xml.status().ToString();
  auto c2 = Compile(MustXMLToProto(*xml));
  ASSERT_NE(c2, nullptr);
  auto codec = sbe::Codec::New({c2->fd});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  auto msg = c2->New("Simple");
  Set(msg.get(), "id", uint32_t{42});
  Set(msg.get(), "value", int32_t{-100});
  auto data = codec->Marshal(*msg);
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  auto got = c2->New("Simple");
  ASSERT_TRUE(codec->Unmarshal(*data, got.get()).ok());
  EXPECT_EQ(got->SerializeAsString(), msg->SerializeAsString());
}

TEST(SbeXML, NameConversions) {
  const std::pair<const char*, const char*> cases[] = {{"orderId", "order_id"},
                                                       {"fillPrice", "fill_price"},
                                                       {"id", "id"},
                                                       {"x", "x"},
                                                       {"orderID", "order_id"}};
  for (const auto& [camel, snake] : cases) {
    const std::string xml = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<sbe:messageSchema xmlns:sbe="http://fixprotocol.io/2016/sbe" package="t" id="1" version="0" byteOrder="littleEndian">
    <types>
        <composite name="messageHeader"><type name="blockLength" primitiveType="uint16"/><type name="templateId" primitiveType="uint16"/><type name="schemaId" primitiveType="uint16"/><type name="version" primitiveType="uint16"/></composite>
    </types>
    <sbe:message name="M" id="1">
        <field name=")") + camel +
                            R"(" id="1" type="int32"/>
    </sbe:message>
</sbe:messageSchema>)";
    EXPECT_TRUE(Contains(MustXMLToProto(xml), std::string(snake) + " = 1")) << camel;
  }
}

TEST(SbeXML, EnumPrefixConversion) {
  auto c = Compile(kTestProto);
  ASSERT_NE(c, nullptr);
  auto xml = sbe::ProtoToXML({c->fd});
  ASSERT_TRUE(xml.ok()) << xml.status().ToString();
  EXPECT_TRUE(Contains(*xml, "<validValue name=\"Buy\">0</validValue>"));
  EXPECT_TRUE(Contains(*xml, "<validValue name=\"Sell\">1</validValue>"));
  const std::string proto = MustXMLToProto(*xml);
  EXPECT_TRUE(Contains(proto, "SIDE_BUY = 0;"));
  EXPECT_TRUE(Contains(proto, "SIDE_SELL = 1;"));
}

TEST(SbeXML, XMLWithoutNamespace) {
  std::string xml = ReplaceAll(kTestSBEXML, "sbe:message", "message");
  xml = ReplaceAll(xml, "sbe:messageSchema", "messageSchema");
  auto schema = sbe::ParseXMLSchema(xml);
  ASSERT_TRUE(schema.ok()) << schema.status().ToString();
  EXPECT_EQ(schema->package, "test.v1");
  EXPECT_EQ(schema->messages.size(), 4u);
}

// A char type whose characterEncoding is not UTF-8 compatible is imported as
// bytes (protowire#494). FIX SBE's default for char is US-ASCII, so a char with
// no encoding, or an ASCII or UTF-8 one, stays a string.
TEST(SbeXML, XMLToProtoCharacterEncoding) {
  const std::string proto = MustXMLToProto(R"(<?xml version="1.0" encoding="UTF-8"?>
<sbe:messageSchema xmlns:sbe="http://fixprotocol.io/2016/sbe"
                   package="enc.v1" id="2" version="0" byteOrder="littleEndian">
    <types>
        <type name="Plain" primitiveType="char" length="4"/>
        <type name="Ascii" primitiveType="char" length="4" characterEncoding="ASCII"/>
        <type name="UsAscii" primitiveType="char" length="4" characterEncoding="US-ASCII"/>
        <type name="Utf8" primitiveType="char" length="4" characterEncoding="utf_8"/>
        <type name="Latin1" primitiveType="char" length="4" characterEncoding="ISO-8859-1"/>
        <composite name="Pair">
            <type name="code" primitiveType="char" length="2" characterEncoding="windows-1252"/>
            <type name="tag" primitiveType="char" length="2"/>
        </composite>
    </types>
    <sbe:message name="Text" id="1">
        <field name="plain" id="1" type="Plain"/>
        <field name="ascii" id="2" type="Ascii"/>
        <field name="usAscii" id="3" type="UsAscii"/>
        <field name="utf8" id="4" type="Utf8"/>
        <field name="latin1" id="5" type="Latin1"/>
        <field name="pair" id="6" type="Pair"/>
    </sbe:message>
</sbe:messageSchema>)");
  for (const char* line :
       {"string plain = 1 [(sbe.length) = 4];",
        "string ascii = 2 [(sbe.length) = 4];",
        "string us_ascii = 3 [(sbe.length) = 4];",
        "string utf8 = 4 [(sbe.length) = 4];",
        "  // SBE characterEncoding \"ISO-8859-1\" is not UTF-8 compatible, so this field is "
        "bytes.\n  bytes latin1 = 5 [(sbe.length) = 4];",
        "  // SBE characterEncoding \"windows-1252\" is not UTF-8 compatible, so this field is "
        "bytes.\n  bytes code = 1 [(sbe.length) = 2];",
        "string tag = 2 [(sbe.length) = 2];"}) {
    EXPECT_TRUE(Contains(proto, line)) << line << "\n" << proto;
  }
  auto c = Compile(proto);
  ASSERT_NE(c, nullptr);
  auto codec = sbe::Codec::New({c->fd});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  // Latin-1 "café": E9 is not UTF-8, and the bytes field carries it.
  auto msg = c->New("Text");
  Set(msg.get(), "latin1", std::string("caf\xE9"));
  auto data = codec->Marshal(*msg);
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  auto got = c->New("Text");
  ASSERT_TRUE(codec->Unmarshal(*data, got.get()).ok());
  EXPECT_EQ(got->GetReflection()->GetString(*got, got->GetDescriptor()->FindFieldByName("latin1")),
            "caf\xE9");
}

// A uint8 or int8 array imports as bytes of the same length, so the block keeps
// its layout.
TEST(SbeXML, XMLToProtoOctetArray) {
  const std::string proto = MustXMLToProto(kFixedArraySchema);
  for (const char* line : {"bytes uuid = 2 [(sbe.length) = 16];",
                           "bytes signed = 3 [(sbe.length) = 3];",
                           "bytes raw = 1 [(sbe.length) = 6];",
                           "uint32 tail = 5;"}) {
    EXPECT_TRUE(Contains(proto, line)) << line << "\n" << proto;
  }
  auto c = Compile(proto);
  ASSERT_NE(c, nullptr);
  auto codec = sbe::Codec::New({c->fd});
  ASSERT_TRUE(codec.ok()) << codec.status().ToString();
  auto msg = c->New("Record");
  const std::string uuid("\0\1\2\3\4\5\6\7\x08\x09\x0a\x0b\x0c\x0d\x0e\xff", 16);
  Set(msg.get(), "uuid", uuid);
  Set(msg.get(), "tail", uint32_t{0xA1B2C3D4});
  auto data = codec->Marshal(*msg);
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  // Header(8) + name(4) + uuid(16) + signed(3) + key(6+4) + tail(4).
  ASSERT_EQ(data->size(), 8u + 4 + 16 + 3 + 10 + 4);
  EXPECT_EQ(std::string(data->begin() + 12, data->begin() + 28), uuid);
  EXPECT_EQ(std::vector<uint8_t>(data->end() - 4, data->end()),
            (std::vector<uint8_t>{0xD4, 0xC3, 0xB2, 0xA1}));
  auto got = c->New("Record");
  ASSERT_TRUE(codec->Unmarshal(*data, got.get()).ok());
  const pb::Reflection* r = got->GetReflection();
  EXPECT_EQ(r->GetString(*got, got->GetDescriptor()->FindFieldByName("uuid")), uuid);
  EXPECT_EQ(r->GetUInt32(*got, got->GetDescriptor()->FindFieldByName("tail")), 0xA1B2C3D4u);
}

// An array of a wider primitive has no proto field that holds it, so the
// import refuses it and names it.
TEST(SbeXML, XMLToProtoRefusesWideArray) {
  const std::pair<const char*, const char*> cases[] = {
      {R"(<type name="Quad" primitiveType="int32" length="4"/>)",
       R"(<field name="q" id="1" type="Quad"/>)"},
      {R"(<composite name="Box"><type name="sides" primitiveType="uint16" length="2"/></composite>)",
       R"(<field name="b" id="1" type="Box"/>)"},
  };
  for (const auto& [types, field] : cases) {
    const std::string schema = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<sbe:messageSchema xmlns:sbe="http://fixprotocol.io/2016/sbe" package="w" id="1" version="0" byteOrder="littleEndian">
    <types>)") + types + R"(</types>
    <sbe:message name="M" id="1">)" +
                               field + R"(</sbe:message>
</sbe:messageSchema>)";
    auto out = sbe::XMLToProto(schema);
    ASSERT_FALSE(out.ok()) << types;
    EXPECT_TRUE(Contains(out.status().message(), "which no proto field represents"))
        << out.status().ToString();
  }
  auto out = sbe::XMLToProto(ReplaceAll(kFixedArraySchema,
                                        R"(primitiveType="uint8" length="16")",
                                        R"(primitiveType="uint64" length="2")"));
  ASSERT_FALSE(out.ok());
  EXPECT_EQ(
      out.status().message(),
      R"(sbe: message "Record": field "uuid": type "Uuid" is an array of 2 uint64, which no proto field represents)");
}

std::vector<std::string> FieldShapes(const pb::Descriptor* md) {
  std::vector<std::string> out;
  for (int i = 0; i < md->field_count(); ++i) {
    const pb::FieldDescriptor* f = md->field(i);
    out.push_back(std::to_string(f->number()) + " " + std::string(f->name()) + " " +
                  std::string(f->type_name()) + " [" + f->options().ShortDebugString() + "]");
  }
  return out;
}

// XML → proto → XML → proto keeps every field's proto type, its SBE
// annotations, and the block layout.
TEST(SbeXML, XMLRoundTripFixedArrays) {
  auto c1 = Compile(MustXMLToProto(kFixedArraySchema));
  ASSERT_NE(c1, nullptr);
  auto xml2 = sbe::ProtoToXML({c1->fd});
  ASSERT_TRUE(xml2.ok()) << xml2.status().ToString();
  EXPECT_TRUE(Contains(*xml2, R"(<type name="bytes16" primitiveType="uint8" length="16"/>)"));
  EXPECT_TRUE(Contains(*xml2, R"(<field name="name" id="1" type="bytes4"/>)"));
  EXPECT_TRUE(Contains(*xml2, R"(<type name="raw" primitiveType="uint8" length="6"/>)"));
  auto c2 = Compile(MustXMLToProto(*xml2));
  ASSERT_NE(c2, nullptr);
  for (const char* name : {"Record", "Key"}) {
    EXPECT_EQ(FieldShapes(c1->fd->FindMessageTypeByName(name)),
              FieldShapes(c2->fd->FindMessageTypeByName(name)))
        << name;
  }
  const auto encode = [](const Compiled& c) {
    auto codec = sbe::Codec::New({c.fd});
    EXPECT_TRUE(codec.ok()) << codec.status().ToString();
    auto msg = c.New("Record");
    Set(msg.get(), "name", std::string("caf\xE9"));
    Set(msg.get(), "uuid", std::string("0123456789abcdef"));
    Set(msg.get(), "signed", std::string("\x80\x7f\x01"));
    pb::Message* key = msg->GetReflection()->MutableMessage(
        msg.get(), msg->GetDescriptor()->FindFieldByName("key"));
    Set(key, "raw", std::string("abcdef"));
    Set(key, "seq", uint32_t{7});
    Set(msg.get(), "tail", uint32_t{9});
    auto data = codec->Marshal(*msg);
    EXPECT_TRUE(data.ok()) << data.status().ToString();
    return data.ok() ? *data : std::vector<uint8_t>();
  };
  EXPECT_EQ(encode(*c1), encode(*c2));
}

// A bytes field of length 1 exports as a uint8 with length 1, which FIX SBE
// reads as a single uint8, so it imports back as uint32 with (sbe.encoding) =
// "uint8". Pinned, as in Go, so a change to that reading is a decision.
TEST(SbeXML, XMLRoundTripBytesLengthOne) {
  auto c = Compile(R"(syntax = "proto3";
package one.v1;
import "sbe/annotations.proto";
option (sbe.schema_id) = 1;
message M {
  option (sbe.template_id) = 1;
  bytes b = 1 [(sbe.length) = 1];
}
)");
  ASSERT_NE(c, nullptr);
  auto xml = sbe::ProtoToXML({c->fd});
  ASSERT_TRUE(xml.ok()) << xml.status().ToString();
  EXPECT_TRUE(Contains(*xml, R"(<type name="bytes1" primitiveType="uint8" length="1"/>)"));
  EXPECT_TRUE(Contains(MustXMLToProto(*xml), R"(uint32 b = 1 [(sbe.encoding) = "uint8"];)"));
}

// What encoding/xml does with a schema it cannot use, in Go's words.
TEST(SbeXML, ParseErrors) {
  EXPECT_EQ(sbe::ParseXMLSchema("").status().message(), "sbe: parse XML schema: EOF");
  EXPECT_EQ(sbe::ParseXMLSchema("<types/>").status().message(),
            "sbe: parse XML schema: expected element type <messageSchema> but have <types>");
  EXPECT_EQ(sbe::ParseXMLSchema(R"(<messageSchema id="x"/>)").status().message(),
            R"(sbe: parse XML schema: strconv.ParseUint: parsing "x": invalid syntax)");
  auto spaced = sbe::ParseXMLSchema(R"(<messageSchema id=" 7 " version=""/>)");
  ASSERT_TRUE(spaced.ok()) << spaced.status().ToString();
  EXPECT_EQ(spaced->id, 7u);
  EXPECT_FALSE(sbe::ParseXMLSchema("<messageSchema>").ok());
}

TEST(SbeXML, ProtoToXMLErrors) {
  EXPECT_EQ(sbe::ProtoToXML({}).status().message(), "sbe: no files provided");
  auto c = Compile("syntax = \"proto3\";\npackage none.v1;\nmessage M { int32 a = 1; }\n");
  ASSERT_NE(c, nullptr);
  auto out = sbe::ProtoToXML({c->fd});
  ASSERT_FALSE(out.ok());
  EXPECT_EQ(out.status().message(),
            "sbe: file " + std::string(c->fd->name()) + std::string(" missing (sbe.schema_id)"));
}

// A descriptor compiled against the pre-v1.12.0 annotations carries
// (sbe.schema_id) at 50100, which nothing declares any more, so it survives
// only as an unknown option field. protowire-go's text.
TEST(SbeXML, ProtoToXMLStaleDescriptor) {
  pb::FileDescriptorProto proto;
  proto.set_name("stale.proto");
  proto.set_package("stale.v1");
  proto.set_syntax("proto3");
  proto.add_message_type()->set_name("M");
  proto.mutable_options()
      ->GetReflection()
      ->MutableUnknownFields(proto.mutable_options())
      ->AddVarint(50100, 1);
  pb::DescriptorPool pool;
  const pb::FileDescriptor* fd = pool.BuildFile(proto);
  ASSERT_NE(fd, nullptr);
  auto out = sbe::ProtoToXML({fd});
  ASSERT_FALSE(out.ok());
  EXPECT_EQ(out.status().message(),
            "sbe: file stale.proto carries (sbe.schema_id) at retired option number 50100 "
            "(registered as 1319 since protowire v1.12.0): the descriptor predates the registered "
            "extension block and must be recompiled against the current sbe/annotations.proto");
}

}  // namespace

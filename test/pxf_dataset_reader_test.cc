// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Tests for DatasetReader (streaming @dataset consumption) and BindRow
// (per-row proto binding). PR 4 of the v0.72-v0.75 cpp catch-up.

#include "protowire/pxf.h"
#include "protowire/pxf/dataset_reader.h"

#include <gtest/gtest.h>
#include "protoc_compat.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

namespace {

namespace pb = google::protobuf;

using protowire::pxf::BindRow;
using protowire::pxf::DatasetReader;
using protowire::pxf::DatasetRow;

class SilentErrorCollector : public pb::compiler::MultiFileErrorCollector {
 public:
  PROTOWIRE_PROTOC_RECORD_ERROR(filename, line, column, msg) {
    last_ = std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column) +
            ": " + std::string(msg);
  }
  std::string last_;
};

class PxfTableReader : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_.MapPath("", TESTDATA_DIR);
    source_tree_.MapPath("", WKT_PROTO_DIR);
    importer_ = std::make_unique<pb::compiler::Importer>(&source_tree_, &errors_);
    file_ = importer_->Import("test.proto");
    ASSERT_NE(file_, nullptr) << errors_.last_;
    factory_ = std::make_unique<pb::DynamicMessageFactory>(importer_->pool());
    desc_ = importer_->pool()->FindMessageTypeByName("test.v1.AllTypes");
    ASSERT_NE(desc_, nullptr);
  }
  std::unique_ptr<pb::Message> NewAllTypes() {
    return std::unique_ptr<pb::Message>(factory_->GetPrototype(desc_)->New());
  }
  pb::compiler::DiskSourceTree source_tree_;
  SilentErrorCollector errors_;
  std::unique_ptr<pb::compiler::Importer> importer_;
  const pb::FileDescriptor* file_ = nullptr;
  const pb::Descriptor* desc_ = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory_;
};

// ---- DatasetReader::Create header parsing -----------------------------------

TEST_F(PxfTableReader, ReadsHeaderAndExposesTypeAndColumns) {
  std::istringstream in("@dataset trades.v1.Trade ( px, qty )\n( 100, 5 )\n( 101, 7 )\n");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok()) << tr.status().message();
  EXPECT_EQ((*tr)->Type(), "trades.v1.Trade");
  ASSERT_EQ((*tr)->Columns().size(), 2u);
  EXPECT_EQ((*tr)->Columns()[0], "px");
  EXPECT_EQ((*tr)->Columns()[1], "qty");
  EXPECT_TRUE((*tr)->Directives().empty());
}

TEST_F(PxfTableReader, NoTableReturnsError) {
  std::istringstream in("@type foo.Msg\nname = \"x\"\n");
  auto tr = DatasetReader::Create(&in);
  ASSERT_FALSE(tr.ok());
  EXPECT_NE(std::string(tr.status().message()).find("no @dataset directive"), std::string::npos);
}

TEST_F(PxfTableReader, EmptyInputReturnsError) {
  std::istringstream in("");
  auto tr = DatasetReader::Create(&in);
  ASSERT_FALSE(tr.ok());
}

TEST_F(PxfTableReader, NullStreamRejected) {
  auto tr = DatasetReader::Create(nullptr);
  ASSERT_FALSE(tr.ok());
}

TEST_F(PxfTableReader, LeadingDirectivesPreserved) {
  std::istringstream in(R"(@header pkg.Hdr { id = "h" }
@frob alpha
@dataset trades.v1.Trade ( px, qty )
( 1, 2 )
)");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok()) << tr.status().message();
  ASSERT_EQ((*tr)->Directives().size(), 2u);
  EXPECT_EQ((*tr)->Directives()[0].name, "header");
  EXPECT_EQ((*tr)->Directives()[1].name, "frob");
}

TEST_F(PxfTableReader, HeaderOversizeRejected) {
  // Generate >64 KiB of leading directive bytes before any @dataset —
  // should fail-fast with the budget message.
  std::string big;
  big.reserve(70 * 1024);
  big.append("@frob ");
  while (big.size() < 70 * 1024) big.append("x ");
  big.append("\n@dataset x.Row ( a )\n");
  std::istringstream in(big);
  auto tr = DatasetReader::Create(&in);
  ASSERT_FALSE(tr.ok());
  EXPECT_NE(std::string(tr.status().message()).find("header exceeds"), std::string::npos);
}

// ---- DatasetReader::Next row iteration --------------------------------------

TEST_F(PxfTableReader, IteratesAllRowsInOrder) {
  std::istringstream in("@dataset x.Row ( a, b )\n( 1, 2 )\n( 3, 4 )\n( 5, 6 )\n");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  int count = 0;
  for (;;) {
    DatasetRow row;
    auto s = (*tr)->Next(&row);
    ASSERT_TRUE(s.ok()) << s.message();
    if ((*tr)->Done()) break;
    EXPECT_EQ(row.cells.size(), 2u);
    ++count;
  }
  EXPECT_EQ(count, 3);
}

TEST_F(PxfTableReader, ZeroRowsReportsDoneImmediately) {
  std::istringstream in("@dataset x.Row ( a )\n");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  DatasetRow row;
  auto s = (*tr)->Next(&row);
  ASSERT_TRUE(s.ok());
  EXPECT_TRUE((*tr)->Done());
}

TEST_F(PxfTableReader, RowCellsParsedAsExpectedShapes) {
  std::istringstream in(R"(@dataset x.Row ( a, b, c, d )
( 42, "hello", true, null )
)");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  DatasetRow row;
  ASSERT_TRUE((*tr)->Next(&row).ok());
  ASSERT_FALSE((*tr)->Done());
  ASSERT_EQ(row.cells.size(), 4u);
  // a = 42 (IntVal)
  ASSERT_TRUE(row.cells[0].has_value());
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<protowire::pxf::IntVal>>(*row.cells[0]));
  // b = "hello" (StringVal)
  ASSERT_TRUE(row.cells[1].has_value());
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<protowire::pxf::StringVal>>(*row.cells[1]));
  EXPECT_EQ(std::get<std::unique_ptr<protowire::pxf::StringVal>>(*row.cells[1])->value, "hello");
  // c = true (BoolVal)
  ASSERT_TRUE(row.cells[2].has_value());
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<protowire::pxf::BoolVal>>(*row.cells[2]));
  // d = null (NullVal — present-but-null)
  ASSERT_TRUE(row.cells[3].has_value());
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<protowire::pxf::NullVal>>(*row.cells[3]));
}

TEST_F(PxfTableReader, ThreeStateCellsAbsentNullSet) {
  std::istringstream in("@dataset x.Row ( a, b, c )\n( 1, , null )\n");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  DatasetRow row;
  ASSERT_TRUE((*tr)->Next(&row).ok());
  ASSERT_EQ(row.cells.size(), 3u);
  EXPECT_TRUE(row.cells[0].has_value());   // present
  EXPECT_FALSE(row.cells[1].has_value());  // absent
  ASSERT_TRUE(row.cells[2].has_value());   // present-but-null
  EXPECT_TRUE(std::holds_alternative<std::unique_ptr<protowire::pxf::NullVal>>(*row.cells[2]));
}

TEST_F(PxfTableReader, ArityMismatchSurfacesAndBecomesSticky) {
  std::istringstream in("@dataset x.Row ( a, b )\n( 1, 2, 3 )\n( 4, 5 )\n");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  DatasetRow row;
  auto s = (*tr)->Next(&row);
  ASSERT_FALSE(s.ok());
  EXPECT_NE(std::string(s.message()).find("3 cells, expected 2"), std::string::npos);
  // Sticky: a second call returns the same error.
  auto s2 = (*tr)->Next(&row);
  EXPECT_FALSE(s2.ok());
}

TEST_F(PxfTableReader, MultiByteRowsAcrossPullBoundaries) {
  // Force the row scanner to pull bytes across many chunk boundaries
  // by using a row body that's much larger than the 4 KiB pull size.
  std::string body = "@dataset x.Row ( a )\n";
  const int row_count = 50;
  for (int i = 0; i < row_count; ++i) {
    body.append("(");
    body.append(std::string(200, 'x').insert(0, "\""));  // an x-string
    body.append("\")\n");
  }
  std::istringstream in(body);
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok()) << tr.status().message();
  int seen = 0;
  for (;;) {
    DatasetRow row;
    auto s = (*tr)->Next(&row);
    ASSERT_TRUE(s.ok()) << s.message();
    if ((*tr)->Done()) break;
    ++seen;
  }
  EXPECT_EQ(seen, row_count);
}

TEST_F(PxfTableReader, ParenthesesInsideStringNotMistakenForRowBoundary) {
  std::istringstream in("@dataset x.Row ( a )\n( \"hi ) there\" )\n( \"next\" )\n");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  DatasetRow row;
  ASSERT_TRUE((*tr)->Next(&row).ok());
  ASSERT_FALSE((*tr)->Done());
  EXPECT_EQ(std::get<std::unique_ptr<protowire::pxf::StringVal>>(*row.cells[0])->value,
            "hi ) there");
  ASSERT_TRUE((*tr)->Next(&row).ok());
  ASSERT_FALSE((*tr)->Done());
  EXPECT_EQ(std::get<std::unique_ptr<protowire::pxf::StringVal>>(*row.cells[0])->value, "next");
  ASSERT_TRUE((*tr)->Next(&row).ok());
  EXPECT_TRUE((*tr)->Done());
}

TEST_F(PxfTableReader, CommentsBetweenRowsIgnored) {
  std::istringstream in(R"(@dataset x.Row ( a )
# leading comment
( 1 )
// between rows
( 2 )
/* block
  comment */
( 3 )
)");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  int count = 0;
  for (;;) {
    DatasetRow row;
    ASSERT_TRUE((*tr)->Next(&row).ok());
    if ((*tr)->Done()) break;
    ++count;
  }
  EXPECT_EQ(count, 3);
}

// ---- DatasetReader::Tail chaining ------------------------------------------

TEST_F(PxfTableReader, TailAllowsChainingToSecondTable) {
  std::istringstream in(R"(@dataset a.Row ( x )
( 1 )
( 2 )
@dataset b.Row ( y )
( "p" )
( "q" )
)");
  auto tr1 = DatasetReader::Create(&in);
  ASSERT_TRUE(tr1.ok());
  EXPECT_EQ((*tr1)->Type(), "a.Row");
  // Drain to EOF.
  for (;;) {
    DatasetRow row;
    ASSERT_TRUE((*tr1)->Next(&row).ok());
    if ((*tr1)->Done()) break;
  }
  // Chain the second table.
  auto tail = (*tr1)->Tail();
  auto tr2 = DatasetReader::Create(tail.get());
  ASSERT_TRUE(tr2.ok()) << tr2.status().message();
  EXPECT_EQ((*tr2)->Type(), "b.Row");
  int n = 0;
  for (;;) {
    DatasetRow row;
    ASSERT_TRUE((*tr2)->Next(&row).ok());
    if ((*tr2)->Done()) break;
    ++n;
  }
  EXPECT_EQ(n, 2);
}

// ---- BindRow + Scan -------------------------------------------------------

TEST_F(PxfTableReader, BindRowSetsFieldsByColumnName) {
  std::istringstream in(R"(@dataset test.v1.AllTypes ( string_field, int32_field )
( "alpha", 42 )
)");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  DatasetRow row;
  ASSERT_TRUE((*tr)->Next(&row).ok());
  ASSERT_FALSE((*tr)->Done());

  auto msg = NewAllTypes();
  auto s = BindRow(msg.get(), (*tr)->Columns(), row);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(msg->GetReflection()->GetString(*msg, desc_->FindFieldByName("string_field")), "alpha");
  EXPECT_EQ(msg->GetReflection()->GetInt32(*msg, desc_->FindFieldByName("int32_field")), 42);
}

TEST_F(PxfTableReader, ScanIsEquivalentToNextPlusBindRow) {
  std::istringstream in(R"(@dataset test.v1.AllTypes ( string_field )
( "row1" )
( "row2" )
)");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  std::vector<std::string> seen;
  for (;;) {
    auto msg = NewAllTypes();
    auto s = (*tr)->Scan(msg.get());
    ASSERT_TRUE(s.ok()) << s.message();
    if ((*tr)->Done()) break;
    seen.push_back(msg->GetReflection()->GetString(*msg, desc_->FindFieldByName("string_field")));
  }
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0], "row1");
  EXPECT_EQ(seen[1], "row2");
}

TEST_F(PxfTableReader, BindRowAbsentCellLeavesFieldDefault) {
  // proto3 string default is "". An absent cell should not stamp a value.
  std::istringstream in(R"(@dataset test.v1.AllTypes ( string_field, int32_field )
( , 7 )
)");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  DatasetRow row;
  ASSERT_TRUE((*tr)->Next(&row).ok());

  auto msg = NewAllTypes();
  auto s = BindRow(msg.get(), (*tr)->Columns(), row);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(msg->GetReflection()->GetString(*msg, desc_->FindFieldByName("string_field")), "");
  EXPECT_EQ(msg->GetReflection()->GetInt32(*msg, desc_->FindFieldByName("int32_field")), 7);
}

TEST_F(PxfTableReader, BindRowMismatchColumnCountErrors) {
  DatasetRow row;
  row.cells.emplace_back(std::nullopt);
  row.cells.emplace_back(std::nullopt);
  auto msg = NewAllTypes();
  std::vector<std::string> columns = {"string_field"};  // 1 column vs 2 cells
  auto s = BindRow(msg.get(), columns, row);
  ASSERT_FALSE(s.ok());
  EXPECT_NE(std::string(s.message()).find("1 columns vs 2 cells"), std::string::npos);
}

TEST_F(PxfTableReader, BindRowUnknownColumnErrors) {
  // The synthetic body names a column the schema doesn't know — surfaces
  // as a per-field "field not found" from the underlying Unmarshal.
  std::istringstream in(R"(@dataset test.v1.AllTypes ( not_a_field )
( "x" )
)");
  auto tr = DatasetReader::Create(&in);
  ASSERT_TRUE(tr.ok());
  auto msg = NewAllTypes();
  auto s = (*tr)->Scan(msg.get());
  EXPECT_FALSE(s.ok());
}

// ---- Positions count from the start of the stream (#161) --------------------

// Each case reads rows until an error and writes one line per row as the Go
// probe in #161's PR does: a row's position and its first cell's span, or the
// error. The expected lines are protowire-go 903b4a7's output for the same
// inputs, under MaxBytesLiteralLength=191. The first `skip` rows are read
// and not written.
std::string ReadAll(const std::string& in, bool scan, int skip) {
  std::istringstream src(in);
  protowire::pxf::UnmarshalOptions opts;
  opts.max_bytes_literal_length = 191;
  auto tr = DatasetReader::Create(&src, opts);
  if (!tr.ok()) return "create: " + tr.status().ToString() + "\n";
  auto at = [](const protowire::pxf::Position& p) {
    return std::to_string(p.line) + ":" + std::to_string(p.column) + "@" + std::to_string(p.offset);
  };
  std::string out;
  for (int i = 0; i < skip; ++i) {
    DatasetRow row;
    if (protowire::Status st = (*tr)->Next(&row); !st.ok()) return "skip: " + st.ToString() + "\n";
  }
  for (int i = skip; i < skip + 10; ++i) {
    const std::string name = "row" + std::to_string(i) + ": ";
    if (scan) {
      pb::FieldDescriptorProto msg;
      protowire::Status st = (*tr)->Scan(&msg);
      if (!st.ok()) return out + name + st.ToString() + "\n";
      continue;
    }
    DatasetRow row;
    protowire::Status st = (*tr)->Next(&row);
    if (!st.ok()) return out + name + st.ToString() + "\n";
    if (row.cells.empty()) return out + name + "EOF\n";
    out += name + "pos=" + at(row.pos);
    if (row.cells[0].has_value()) {
      auto [first, end] = protowire::pxf::ValueSpan(*row.cells[0]);
      out += " cell0=" + at(first) + " end=" + at(end);
    }
    out += "\n";
  }
  return out;
}

TEST(DatasetReaderPositions, MatchProtowireGo) {
  const std::string big = "b\"" + std::string(256, 'A') + "\"";
  const std::string over = ": bytes literal decodes to more than MaxBytesLiteralLength=191 bytes\n";
  struct Case {
    const char* name;
    std::string in;
    bool scan;
    std::string want;
    int skip = 0;
  };
  const Case cases[] = {
      {"first row", "@dataset x.Row ( raw )\n( " + big + " )\n", false, "row0: 2:3" + over},
      {"later row",
       "@dataset x.Row ( raw )\n( 1 )\n( 2 )\n\n  ( " + big + " )\n",
       false,
       "row0: pos=2:1@23 cell0=2:3@25 end=2:4@26\n"
       "row1: pos=3:1@29 cell0=3:3@31 end=3:4@32\n"
       "row2: 5:5" +
           over},
      {"rows on the header's line",
       "@dataset x.Row ( raw ) ( 1 )   ( " + big + " )\n",
       false,
       "row0: pos=1:24@23 cell0=1:26@25 end=1:27@26\n"
       "row1: 1:34" +
           over},
      {"a row's second line",
       "@dataset x.Row ( a, b ) ( 1,\n    " + big + " )\n",
       false,
       "row0: 2:5" + over},
      {"arity",
       "@dataset x.Row ( a, b )\n( 1, 2 )\n   ( 1 )\n",
       false,
       "row0: pos=2:1@24 cell0=2:3@26 end=2:4@27\n"
       "row1: 3:4: @dataset row has 1 cells, expected 2 (column count)\n"},
      {"bad token",
       "@dataset x.Row ( a )\n( 1 )\n  ( 1 2 )\n",
       false,
       "row0: pos=2:1@21 cell0=2:3@23 end=2:4@24\n"
       "row1: 3:7: expected ',' or ')' in @dataset row, got integer\n"},
      // 18000 bytes of rows before the bad one: it arrives several reads in.
      {"many reads",
       "@dataset x.Row ( raw )\n" +
           [] {
             std::string rows;
             for (int i = 0; i < 3000; ++i) rows += "( 1 )\n";
             return rows;
           }() +
           "  ( 1, 2 )\n",
       false,
       "row3000: 3002:3: @dataset row has 2 cells, expected 1 (column count)\n",
       3000},
      {"scan, a column the type lacks",
       "@dataset google.protobuf.FieldDescriptorProto ( nope )\n\n  ( 1 )\n",
       true,
       "row0: 3:3: @dataset column \"nope\": google.protobuf.FieldDescriptorProto has no field "
       "\"nope\"\n"},
      {"scan",
       "@dataset google.protobuf.FieldDescriptorProto ( name, number )\n(\"a\", 1)\n  (\"b\", "
       "\"x\")\n",
       true,
       "row1: 3:3: column number: expected integer for field \"number\"\n"},
  };
  for (const Case& c : cases) {
    EXPECT_EQ(ReadAll(c.in, c.scan, c.skip), c.want) << c.name;
  }
}

// ---- A row ends at a `)`, a directive or the end of input (#164) ---------

std::string FromHex(const std::string& hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

// testdata/dataset-stream/go_sweep.tsv is protowire-go's verdict on streaming
// inputs: rows and headers cut short, junk where a row could begin, the cap on the bytes
// held while a row's end is sought, Scan's limits, and the 4096-byte read
// boundary at every offset in and around a row. Each line is name,
// max_message_size, mode, head, pad, tail and the result: the rows read, then
// EOF or the error. tools/dataset_stream_sweep wrote it; its header says how
// to regenerate it.
TEST(DatasetReaderEnds, MatchProtowireGo) {
  std::ifstream in(std::string(TESTDATA_DIR) + "/dataset-stream/go_sweep.tsv");
  ASSERT_TRUE(in) << "testdata/dataset-stream/go_sweep.tsv";
  int cases = 0;
  int errors = 0;
  for (std::string line; std::getline(in, line);) {
    std::vector<std::string> f;
    for (size_t at = 0;;) {
      const size_t tab = line.find('\t', at);
      f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
      if (tab == std::string::npos) break;
      at = tab + 1;
    }
    ASSERT_EQ(f.size(), 7u) << line;
    SCOPED_TRACE(f[0] + " max_message_size=" + f[1]);
    std::string input = FromHex(f[3]);
    if (const int pad = std::stoi(f[4]); pad >= 0) {
      input += "(\"" + std::string(pad, 'p') + "\")\n";
    }
    input += FromHex(f[5]);
    std::istringstream src(input);
    protowire::pxf::UnmarshalOptions opts;
    opts.max_message_size = std::stoi(f[1]);
    auto tr = DatasetReader::Create(&src, opts);
    std::string got;
    if (!tr.ok()) {
      got = "create: " + tr.status().ToString();
    } else {
      for (int rows = 0;; ++rows) {
        protowire::Status st;
        if (f[2] == "scan") {
          pb::FileOptions msg;
          st = (*tr)->Scan(&msg);
        } else {
          DatasetRow row;
          st = (*tr)->Next(&row);
        }
        if (!st.ok() || (*tr)->Done()) {
          got = "rows=" + std::to_string(rows) + " " + (st.ok() ? "EOF" : st.ToString());
          break;
        }
      }
    }
    EXPECT_EQ(got, f[6]);
    ++cases;
    if (f[6].rfind("rows=", 0) != 0 || f[6].find(' ') != f[6].size() - 4 ||
        f[6].substr(f[6].size() - 4) != " EOF") {
      ++errors;
    }
  }
  // The table reads whole: 590 cases, of which 230 end in an error.
  EXPECT_EQ(cases, 590);
  EXPECT_EQ(errors, 230);
}

}  // namespace

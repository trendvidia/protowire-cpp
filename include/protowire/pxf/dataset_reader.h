// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Streaming consumption for the `@dataset` directive (draft §3.4.4).
//
// `UnmarshalFull` materializes an entire `@dataset` directive — every row
// — into `Result::Datasets()`. That works for small datasets and breaks
// for the CSV-replacement workload `@dataset` was designed to serve.
// `DatasetReader` provides the streaming alternative: it pulls bytes from
// a `std::istream` on demand and yields one `DatasetRow` per `Next()`
// call, with working-set memory bounded by the size of the largest
// single row.
//
// Per draft §3.4.4: streaming consumers MUST enforce per-row arity and
// the v1 cell-grammar rule on each row as it is consumed (not deferred
// to end-of-input) and MUST yield rows in source order. Both invariants
// fall out of this implementation: the row scanner, which reads with
// the lexer `Parse()` uses, produces one (...) slice at a time, and the
// row parser the materializing path uses validates it.
//
// Convenience: `Scan(msg)` reads the next row and binds its cells to
// `msg`'s fields by column name; `BindRow` is exported for callers that
// iterate the materializing path's `Result::Datasets()[i].rows`.

#pragma once

#include <cstdint>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <google/protobuf/message.h>

#include "protowire/detail/status.h"
#include "protowire/pxf/ast.h"  // Directive, DatasetRow
#include "protowire/pxf/options.h"

namespace protowire::pxf {

struct BraceMatch;  // src/pxf/parser_internal.h

// Default cap on the @dataset header (leading directives plus the
// `@dataset TYPE ( cols )` declaration). Real headers are tiny — a few
// hundred bytes at most. The cap exists to fail-fast on misuse: a
// DatasetReader pointed at a multi-gigabyte document with no `@dataset`
// directive shouldn't OOM trying to find one.
constexpr int kDefaultHeaderMaxBytes = 64 * 1024;

// Streaming row reader for a single `@dataset` directive.
//
// A DatasetReader is positioned at the first row after `Create()`
// returns. Call `Next(&row)` in a loop until `Done()` returns true;
// the table's row sequence is exhausted at that point. Any parse or
// I/O error makes the reader sticky: subsequent `Next` / `Scan` calls
// return the same Status.
//
// For documents containing multiple `@dataset` directives, call
// `Create()` again on `tr->Tail()` to read the next table.
//
// A DatasetReader is NOT safe for concurrent use.
class DatasetReader {
 public:
  // Construct a DatasetReader and consume the leading directives and the
  // `@dataset TYPE ( cols )` header. `src` must outlive the reader.
  // Returns a non-OK Status if the input ends before any `@dataset`
  // directive is seen (the message contains "no @dataset directive in
  // stream") or on a parse / I/O error.
  static StatusOr<std::unique_ptr<DatasetReader>> Create(std::istream* src);

  // Create under options. The bytes held while a row's end is sought are
  // capped at opts.max_message_size, and the header and every row are
  // parsed under its nesting and bytes-literal limits. Scan binds each row
  // as BindRow does under the same options, except that max_message_size
  // stays at its default, since a row's body repeats the column names. So
  // the other limits, type_resolver (which a cell holding an Any block
  // needs), discard_unknown, skip_post_decode, skip_validate,
  // on_secret_field, validator and report all apply to a streamed row, as
  // they do in protowire-go (#166).
  static StatusOr<std::unique_ptr<DatasetReader>> Create(std::istream* src, UnmarshalOptions opts);

  // Row message type declared by the @dataset header (e.g. "trades.v1.Trade").
  const std::string& Type() const { return type_; }

  // Column field names declared by the @dataset header, in source order.
  const std::vector<std::string>& Columns() const { return columns_; }

  // Side-channel directives (`@<name>` / `@entry` / etc., NOT `@type`
  // or `@dataset`) that appeared before the `@dataset` header. Stable for
  // the lifetime of the reader.
  const std::vector<Directive>& Directives() const { return directives_; }

  // Reads the next row into `*out`. Returns OK on success; returns OK
  // and sets `done_` when the row sequence is exhausted (callers check
  // `Done()` to distinguish "got a row" from "EOF"): at the end of the
  // input, or before the next directive (read it via Tail). Anything else
  // that is not a well-formed row, a row cut short at the end of the input
  // included, is an error, and it is the error Parse reports for the same
  // input, at the same position. So is a read failure, and holding more
  // than the options' max_message_size bytes while seeking a row's end.
  //
  // After EOF or error, all subsequent calls return the same sticky
  // result.
  Status Next(DatasetRow* out);

  // Reads the next row and binds its cells to fields of `msg` by column
  // name. Equivalent to `Next` + `BindRow`. At EOF, returns OK and sets
  // `done_` — callers check `Done()`.
  Status Scan(google::protobuf::Message* msg);

  // True once the row sequence has been exhausted.
  bool Done() const { return finished_; }

  // Sticky error (or OK if none).
  const Status& StickyStatus() const { return err_; }

  // Returns a fresh istream-derived source that yields the bytes the
  // reader buffered but didn't consume, followed by the remaining
  // bytes from the underlying source. Use to chain a second
  // `Create()` for documents with multiple `@dataset` directives.
  //
  // MUST only be called after `Next` has reported `Done()`. Calling
  // earlier returns bytes the current reader still intends to consume,
  // which will desync the next reader.
  std::unique_ptr<std::istream> Tail();

  ~DatasetReader();
  DatasetReader(const DatasetReader&) = delete;
  DatasetReader& operator=(const DatasetReader&) = delete;

 private:
  friend class DatasetReaderTestPeer;  // reads scanned_
  DatasetReader() = default;

  // What ScanRow or ScanHeader found; see dataset_reader.cc.
  enum class RowScan : uint8_t { kNeedMore, kRow, kBad, kEnd, kNotRow };

  Status ReadHeader();
  Status Pull(size_t n);
  Status Fail(Status st);
  int MaxBytesLiteral() const;
  StatusOr<DatasetRow> ParseRow(int start, int end, int* end_offset = nullptr) const;
  std::optional<DatasetRow> TryWholeRow();
  Status RowError(int start) const;
  RowScan ScanRow(int* start, int* end);
  RowScan ScanHeader(int* end);
  bool Complete(const Token& tok, int tok_end) const;
  void Wait(int at);
  bool Resume();
  bool MayEndToken() const;

  std::istream* src_ = nullptr;
  Position PosAt(size_t i) const;
  void Consume(size_t n);

  std::string pending_;  // bytes pulled from src_ but not yet consumed
  Position base_;        // where pending_[0] is in the stream
  // The row scan resumes where the last read left it: scan_pos_ is the
  // first token not known to be complete, row_start_ the open row's `(`
  // or -1 between rows. Both index pending_.
  int scan_pos_ = 0;
  int row_start_ = -1;
  // While a token runs past the buffer's end, the scan does not lex it again
  // until bytes arrive that could end it (MayEndToken): wait_at_ is the
  // token's start, wait_from_ the buffer length already checked.
  bool waiting_ = false;
  int wait_at_ = 0;
  int wait_from_ = 0;
  // Where the header scan stands: past the @dataset token, how far into an
  // @proto directive (1 after @proto, 2 after its type name), and in its
  // body, whose braces are matched as bytes.
  bool hdr_in_dataset_ = false;
  int hdr_proto_ = 0;
  bool hdr_in_body_ = false;
  std::unique_ptr<BraceMatch> hdr_body_;
  // The bytes the header and row scans have lexed, for the tests that hold
  // them linear in the input.
  size_t scanned_ = 0;
  bool src_eof_ = false;
  bool finished_ = false;
  Status err_;
  std::string type_;
  std::vector<std::string> columns_;
  std::vector<Directive> directives_;
  UnmarshalOptions opts_;
  const google::protobuf::Descriptor* checked_type_ = nullptr;  // see Scan
};

// BindRow renders the row's cells as a synthetic PXF body and runs it
// through the standard `UnmarshalFull` pipeline against `msg`. The
// `columns` slice MUST have the same length as `row.cells`. A column may
// be a dotted path into a sub-message, and cells sharing a prefix land in
// one block.
//
// Cell-state semantics (mirrors draft §3.4.4):
//   - std::nullopt cell (empty cell) — field absent. (pxf.default) is
//     applied if declared on the field; (pxf.required) errors if
//     neither default nor value is present.
//   - NullVal — field present-but-null, on a field with presence; any
//     other field rejects it, as a body does.
//   - any other Value — field set, as `<column> = <value>` would set it
//     in a body, so a list or block cell works and `{}` is a present
//     empty message.
//
// A column that names no field of msg's type, or a path that goes through
// a repeated, map or scalar field or into a type written as a literal,
// fails every row bound into that type, its cells empty or not.
//
// msg's descriptor is checked against the bind-time schema rules
// (ValidateDescriptor), as Unmarshal checks it. That walks the import
// closure on every call. A caller binding many rows of one type should
// validate once and pass opts.skip_validate, as DatasetReader::Scan does.
//
// Exported so callers iterating `Result::Datasets()[i].rows` can reuse
// the same logic.
Status BindRow(google::protobuf::Message* msg,
               const std::vector<std::string>& columns,
               const DatasetRow& row);

// BindRow under options: every one of them applies to the row, as
// Unmarshal applies it to a body. type_resolver is what a cell holding an
// Any block that opens with a type entry needs. A decode error carries the
// row's position; a schema violation or a validator's error is returned as
// it is, as protowire-go returns them.
Status BindRow(google::protobuf::Message* msg,
               const std::vector<std::string>& columns,
               const DatasetRow& row,
               UnmarshalOptions opts);

}  // namespace protowire::pxf

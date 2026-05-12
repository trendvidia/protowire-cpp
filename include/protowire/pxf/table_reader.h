// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Streaming consumption for the `@table` directive (draft §3.4.4).
//
// `UnmarshalFull` materializes an entire `@table` directive — every row
// — into `Result::Tables()`. That works for small datasets and breaks
// for the CSV-replacement workload `@table` was designed to serve.
// `TableReader` provides the streaming alternative: it pulls bytes from
// a `std::istream` on demand and yields one `TableRow` per `Next()`
// call, with working-set memory bounded by the size of the largest
// single row.
//
// Per draft §3.4.4: streaming consumers MUST enforce per-row arity and
// the v1 cell-grammar rule on each row as it is consumed (not deferred
// to end-of-input) and MUST yield rows in source order. Both invariants
// fall out of this implementation: the byte-level row scanner produces
// one (...) slice at a time, and the same `Parse()` used by the
// materializing path validates it.
//
// Convenience: `Scan(msg)` reads the next row and binds its cells to
// `msg`'s fields by column name; `BindRow` is exported for callers that
// iterate the materializing path's `Result::Tables()[i].rows`.

#pragma once

#include <istream>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/message.h>

#include "protowire/detail/status.h"
#include "protowire/pxf/ast.h"  // Directive, TableRow

namespace protowire::pxf {

// Default cap on the @table header (leading directives plus the
// `@table TYPE ( cols )` declaration). Real headers are tiny — a few
// hundred bytes at most. The cap exists to fail-fast on misuse: a
// TableReader pointed at a multi-gigabyte document with no `@table`
// directive shouldn't OOM trying to find one.
constexpr int kDefaultHeaderMaxBytes = 64 * 1024;

// Streaming row reader for a single `@table` directive.
//
// A TableReader is positioned at the first row after `Create()`
// returns. Call `Next(&row)` in a loop until `Done()` returns true;
// the table's row sequence is exhausted at that point. Any parse or
// I/O error makes the reader sticky: subsequent `Next` / `Scan` calls
// return the same Status.
//
// For documents containing multiple `@table` directives, call
// `Create()` again on `tr->Tail()` to read the next table.
//
// A TableReader is NOT safe for concurrent use.
class TableReader {
 public:
  // Construct a TableReader and consume the leading directives and the
  // `@table TYPE ( cols )` header. `src` must outlive the reader.
  // Returns a non-OK Status if the input ends before any `@table`
  // directive is seen (the message contains "no @table directive in
  // stream") or on a parse / I/O error.
  static StatusOr<std::unique_ptr<TableReader>> Create(std::istream* src);

  // Row message type declared by the @table header (e.g. "trades.v1.Trade").
  const std::string& Type() const { return type_; }

  // Column field names declared by the @table header, in source order.
  const std::vector<std::string>& Columns() const { return columns_; }

  // Side-channel directives (`@<name>` / `@entry` / etc., NOT `@type`
  // or `@table`) that appeared before the `@table` header. Stable for
  // the lifetime of the reader.
  const std::vector<Directive>& Directives() const { return directives_; }

  // Reads the next row into `*out`. Returns OK on success; returns OK
  // and sets `done_` when the row sequence is exhausted (callers check
  // `Done()` to distinguish "got a row" from "EOF"). Returns a non-OK
  // Status on parse / I/O error.
  //
  // After EOF or error, all subsequent calls return the same sticky
  // result.
  Status Next(TableRow* out);

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
  // `Create()` for documents with multiple `@table` directives.
  //
  // MUST only be called after `Next` has reported `Done()`. Calling
  // earlier returns bytes the current reader still intends to consume,
  // which will desync the next reader.
  std::unique_ptr<std::istream> Tail();

 private:
  TableReader() = default;

  Status ReadHeader();
  Status Pull(size_t n);

  std::istream* src_ = nullptr;
  std::string pending_;  // bytes pulled from src_ but not yet consumed
  bool src_eof_ = false;
  bool finished_ = false;
  Status err_;
  std::string type_;
  std::vector<std::string> columns_;
  std::vector<Directive> directives_;
};

// BindRow renders the row's cells as a synthetic PXF body and runs it
// through the standard `Unmarshal` pipeline against `msg`. The
// `columns` slice MUST have the same length as `row.cells`.
//
// Cell-state semantics (mirrors draft §3.4.4):
//   - std::nullopt cell (empty cell) — field absent. (pxf.default) is
//     applied if declared on the field; (pxf.required) errors if
//     neither default nor value is present.
//   - *NullVal — field cleared, per draft §3.9 (clears optional /
//     wrapper / oneof; rejects on non-nullable scalars).
//   - any other Value — field set to that value.
//
// Exported so callers iterating `Result::Tables()[i].rows` can reuse
// the same logic.
Status BindRow(google::protobuf::Message* msg,
               const std::vector<std::string>& columns,
               const TableRow& row);

}  // namespace protowire::pxf

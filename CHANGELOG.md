# Changelog

All notable changes to `protowire-cpp` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The version number is kept aligned with the rest of the `protowire-*`
stack — releases bump in lockstep across language ports when the wire
format changes.

## [Unreleased]

### Added

- **`TableReader` streaming `@table` consumption + `Scan` / `BindRow`
  per-row binding** (draft §3.4.4). `UnmarshalFull` materializes
  every row of an `@table` directive into `Result::Tables()`; that
  works for small datasets and breaks for the CSV-replacement
  workload `@table` was designed for. New
  `<protowire/pxf/table_reader.h>` exposes:
  - `TableReader::Create(std::istream*)` — consumes any leading
    directives and the `@table TYPE ( cols )` header, returns a
    reader positioned at the first row. Header is capped at 64 KiB
    (`kDefaultHeaderMaxBytes`) to fail-fast when a non-`@table`
    document is handed in by mistake.
  - `Type()` / `Columns()` / `Directives()` accessors.
  - `Next(TableRow*)` pulls one row at a time from the underlying
    stream; working-set memory is bounded by the largest single row,
    not the full table. Per-row arity and v1 cell-grammar checks
    happen at consume time (not deferred to EOF), matching the
    spec's streaming-consumer requirements.
  - `Scan(Message*)` — convenience: `Next` + `BindRow`.
  - `Tail()` — returns the unconsumed buffer plus the remaining
    underlying source as a fresh `std::istream`, so callers can chain
    a second `Create()` for documents with multiple `@table`
    directives.
  - `BindRow(Message*, columns, row)` — exported helper for callers
    iterating `Result::Tables()[i].rows` from the materializing
    path. Strategy is format-and-reparse: render the row as a
    synthetic PXF body (`<col> = <val>` per non-`std::nullopt` cell)
    and run it through `Unmarshal`. This reuses every branch of the
    existing decoder — WKT timestamps / durations, wrapper
    nullability, enum-by-name resolution, `pxf.required` /
    `pxf.default`, oneof handling — instead of growing a parallel
    Value→FieldDescriptor switch. `SkipValidate` avoids re-running
    the reserved-name check per row.

- **`Result::Directives()` and `Result::Tables()` accessors.** The
  fast-path direct decoder now populates the document-root directive
  list and `@table` directive list on `Result` during
  `UnmarshalFull`, so consumers can read them after a decode call.
  - `Result::Directives()` returns the generic
    `@<name> *(prefix) [{ ... }]` blocks in source order, with raw
    body bytes preserved verbatim for downstream re-parsing
    (chameleon's `@header T { ... }` reader, etc.). A single prefix
    populates the back-compat `type` field; two or more leave it
    empty and consumers read `prefixes[]` directly.
  - `Result::Tables()` returns the `@table` directives with full
    column metadata and parsed cell values per row, faithful to the
    three-state cell grammar (absent / present-but-null /
    present-with-value).
  - `Unmarshal` (vs `UnmarshalFull`) still passes a null Result and
    walks directives without allocating any AST nodes — the fast
    path retains its zero-allocation contract on the hot path.


- **PXF schema reserved-name validator (`SchemaValidator`, draft §3.13).**
  Rejects protobuf schemas that declare a message field, oneof, or
  enum value whose name is case-sensitively equal to a PXF value
  keyword (`null` / `true` / `false`) — such a name lexes as the
  keyword and the declared element is unreachable from PXF surface
  syntax. New `<protowire/pxf/schema.h>` exposes `ValidateDescriptor`,
  `ValidateFile`, and the `Violation` struct (with `file`, `element`,
  `name`, `kind`); results are sorted by element FQN for stable
  output. `UnmarshalOptions` gains `skip_validate` for consumers that
  validate once at registry-load time and don't want the per-call
  recheck cost. `Unmarshal` and `UnmarshalFull` invoke the validator
  on the message's descriptor before decode runs; violations come
  back as a joined-string error message with one line per offender.


- **PXF parser-side `@<name>` / `@entry` / `@table` directive grammar**
  (draft §3.4.2 – §3.4.4). The AST `Document` now carries
  `directives` (generic `@<name> *(prefix) [{ ... }]` entries) and
  `tables` (`@table <type> ( cols ) row*` entries) alongside the
  existing `type_url` and `entries`. `Directive::body` preserves the
  raw bytes between `{` and `}`; `Directive::type` keeps the legacy
  single-prefix shape for v0.72.0-era consumers. `Document::body_offset`
  marks the byte right after the last directive (used by chameleon
  for hashing the schema-typed payload).
  Both the AST parser and the fast direct-decode path consume the new
  forms; runtime semantics (Result accessors, TableReader streaming,
  per-row Scan/BindRow) follow in subsequent PRs of the v0.72-v0.75
  catch-up sequence. The fast path discards directive contents for
  now and enforces the standalone constraint: a document containing
  any `@table` directive MUST NOT also carry `@type` or top-level
  field entries (draft §3.4.4).
  `Position` gains a `offset` field (byte offset into the lexer's
  input) so directive Body extraction can slice raw bytes; existing
  callers that read only line/column are unaffected.


- **`cmd/check_decode` HARDENING conformance binary.** The per-port
  binary the spec repo's `scripts/cross_security_check.sh` expects
  for every C++/Go/Java/etc. port. Runtime-compiles a `--proto` via
  libprotoc's `Importer`, decodes a `--input` against a `--schema`
  descriptor, and exits 0 on accept / 1 on clean reject / >1 on
  crash. Mirrors the Go reference at
  `protowire-go/scripts/check_decode/main.go`.

  Supported formats:
  - `pxf` — `protowire::pxf::Unmarshal` against a `DynamicMessage`
    built from the runtime-compiled descriptor.
  - `pb` — standard libprotobuf `Message::ParseFromArray` against a
    `DynamicMessage`. The C++ port's `protowire::pb` codec is
    struct-tag-driven and doesn't accept descriptor-bound inputs,
    so the descriptor-driven hardening tier exercises libprotobuf's
    parser primitives directly — which is what the adversarial
    corpus's depth / length / overflow probes actually hit on any
    libprotobuf-backed consumer.
  - `sbe`, `envelope` — not yet implemented; returns exit 2
    (configuration error) so the manifest can mark per-port skips.

  Build is gated on `libprotoc` being available — Ubuntu ships it
  as `libprotoc-dev`, separate from `libprotobuf-dev`. Consumers
  without `libprotoc` get a CMake `STATUS` message and the target
  is skipped; the rest of the build is unaffected.

  Surfaces real port-level HARDENING gaps. Smoke-running against
  the spec repo's adversarial corpus on `main` already shows:
  - `deep-nesting-200.pxf` accepted (cpp has no MaxNestingDepth cap)
  - `invalid-utf8-string.pxf` accepted (cpp has no proto3 UTF-8
    enforcement)

  Those are real HARDENING.md violations the cross-port harness now
  catches automatically. Tracked as a follow-up; this PR only adds
  the binary and wires the build.

## [0.70.0]

Initial public release. The version number aligns this port with the rest
of the `protowire-*` stack, which targets the 0.70.x series for the first
coordinated public release. The Python port (`protowire-python`) consumes
this library through the nanobind FFI and inherits its behaviour.

### Added

- **CMake `find_package` install** under `protowire::` namespace. Consumers
  can `find_package(protowire CONFIG REQUIRED)` after `cmake --install`
  and link against `protowire::pxf`, `protowire::sbe`,
  `protowire::envelope`, `protowire::pb`, `protowire::detail`,
  `protowire::protos`. Transitively pulls in `protobuf::libprotobuf`.
- **Comprehensive CI matrix**: Linux × {gcc-13, clang-18}, macOS ×
  AppleClang, Windows × MSVC (with vcpkg for protobuf), plus a
  dedicated AddressSanitizer + UBSan job that runs the full test suite
  under sanitizers on every PR. clang-format + clang-tidy gates,
  llvm-cov coverage uploaded to Codecov, weekly CodeQL SAST.
- **Governance scaffolding**: `LICENSE` (MIT), `CONTRIBUTING.md`,
  `SECURITY.md` (security@trendvidia.com), `GOVERNANCE.md`,
  `CODE_OF_CONDUCT.md`, `.github/CODEOWNERS`, issue + PR templates,
  Dependabot for GitHub Actions.
- **Style configs**: root `.clang-format` and `.clang-tidy` matching the
  in-tree style; CI's clang-format check job rejects unformatted code.

### Changed (breaking)

- **PXF parser stricter on key forms**, mirroring the upstream grammar
  tightening in
  [`trendvidia/protowire@8262bbb`](https://github.com/trendvidia/protowire/commit/8262bbb)
  (`docs/grammar.ebnf`, `docs/draft-trendvidia-protowire-00.txt`):
  - `=` (field assignment) and `{ … }` (submessage) now require an
    identifier key. Inputs like `123 = 234` or `child { 123 = 123 }`
    are now parse errors with
    `"field assignment with '=' requires an identifier key, got integer
    (\"123\"); use ':' for map entries"`.
  - `:` (map entry) is rejected at document top level — the document
    represents a proto message, never a `map<K,V>`. Use `=` for
    top-level field assignments. Map literals (`field = { 1: "x" }`)
    still work because `:` remains valid inside `{ … }` blocks.

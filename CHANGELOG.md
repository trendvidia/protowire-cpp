# Changelog

All notable changes to `protowire-cpp` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The version number is kept aligned with the rest of the `protowire-*`
stack — releases bump in lockstep across language ports when the wire
format changes.

## [Unreleased]

### Added

- **pxf: quoted entry names and keyed repeated fields** (#18; draft `-01`
  §3.13, protowire#116, reference protowire-go#50). The vendored
  `proto/pxf/annotations.proto` gains `(pxf.key) = 1316`, matching
  canonical. The grammar accepts a string at entry-name position with
  `=` and `{` everywhere (`Assignment::key_quoted`, `Block::name_quoted`
  retain the spelling for `FormatDocument`); the schema layer rejects it
  outside a keyed repeated field's block. A repeated message-typed field
  whose `(pxf.key)` names a singular string field of the element message
  decodes from the keyed block form — `children { greeting { … } }` or
  `children = { greeting = { … } }` — with the entry name as the key,
  entry order as list order, and duplicate names, the empty string, and
  a disagreeing explicit key assignment as errors; the anonymous list
  form still decodes, with an explicit empty key rejected. `Marshal`
  writes the keyed form whenever every key is present, non-empty and
  distinct (names bare when identifier-safe, quoted otherwise, the key
  field omitted from the entry body), and the anonymous form otherwise.
  `CanonicalizeKeyed(Document*, const Descriptor*)` (new
  `protowire/pxf/keyed.h`) is the schema-aware AST rewrite behind `fmt`:
  eligible anonymous bindings become keyed blocks, `name = { }` becomes
  `name { }`, identifier-safe quoted names are unquoted, redundant key
  assignments are dropped. `ValidateFile` reports a misplaced
  `(pxf.key)` as `ViolationKind::kKeyOption` with a `detail`;
  `KeyFieldName`, `KeyField` and `IsKeyed` are exported from
  `protowire/pxf/annotations.h`. The spec repo's `testdata/keyed/` is
  vendored and every fixture is pinned.

### Security

- **HARDENING.md § Mandatory limits are enforced, configurable per
  call, and the adversarial corpus passes on every row** (#26, #25;
  draft `-01` § Mandatory Limits, protowire#299, #301). Measured on
  `main` at 2a8c854 with the spec repo's `cross_security_check.sh`, ten
  of the port's rows failed: no PXF depth cap at all (200 and 1000
  levels accepted, 100 000 levels a stack overflow, 101 levels of blocks
  or lists accepted), a `\xFF\xFE` escape accepted into a proto3
  string, a 5000-digit literal accepted on a `pxf.BigInt`, and the
  three SBE rows reported as crashes. **Cause:** never wired, not a
  regression — no commit in this repository's history references
  `MaxNestingDepth`, `check_decode` (#8) exited 2 for `--format sbe`
  ("not implemented"), which the harness classifies as a crash, and its
  PB leg went through libprotobuf's parser rather than this port's
  codec. The gaps #1 recorded at M8 were closed without landing.
  - `pxf::UnmarshalOptions` gains `max_message_size`,
    `max_nesting_depth`, `max_numeric_literal_digits`,
    `max_bytes_literal_length` and `max_repeated_count` (0 = the default
    in the new `protowire/limits.h`); `Parse` takes a `ParseOptions`
    with the first, second and fourth. The input size is checked before
    the first token; every `{` or `[` is one descent from a root at 0,
    counted identically by `Parse` and `Unmarshal`, so exactly 100
    descents decode and 101 do not; a `b"…"` literal is refused from its
    length before it is decoded; a repeated or map field is refused
    before its element past the bound is allocated; a literal bound to
    `pxf.BigInt` / `Decimal` / `BigFloat` is refused past 4096 digits;
    a proto3 `string` field (scalar, repeated, map key) refuses invalid
    UTF-8 from `\xHH` / `\NNN` escapes or raw bytes, while `bytes` fields
    take them. The decoder now surfaces the lexer's own diagnostic for an
    ILLEGAL token instead of "expected string".
  - `pb::Unmarshal` takes a `pb::UnmarshalOptions` (`max_message_size`,
    `max_nesting_depth`, `max_numeric_literal_digits`,
    `max_repeated_count`). The depth counter is threaded through nested
    submessages, map entries and big-number messages rather than reset
    by the fresh span; `pxf.Decimal.scale` is bounded to ±4096 on the
    wire (protowire#279); and repeated numeric fields decode from the
    **packed** form every other encoder in the family writes — before,
    `0a 03 01 02 03` read as one element and corrupt tags.
  - `sbe::Codec::New` takes a `sbe::CodecOptions` (`max_message_size`,
    `max_repeated_count`). `Unmarshal` and `NewView` validate the header
    (template id, wire block ≥ template block) and every group header
    before any entry is allocated: wire entry block ≥ template's, no
    zero-length block with a non-zero count, count × block within the
    remaining input (as a division, so it cannot overflow), count within
    `MaxRepeatedCount`. `GroupView::Entry` past the count reads as an
    empty view instead of a span past the buffer. A `char[]` decoded into
    a proto3 string refuses invalid UTF-8.
  - `check_decode` gains `--limit NAME=VALUE`, decodes `--format sbe`
    through the codec, and `--format pb` through this port's codec with
    hand-mirrored `adversarial.proto` types, as the Go reference does.
    All 38 (port, corpus) pairs pass, including the twelve `limits` rows
    and the three `pb/decimal-*` rows the manifest skipped for this
    port.

### Fixed

- **pxf fmt: the quotes on a string map key spelled like a keyword or an
  integer are kept** (#27; draft `-01` § Entries and Keys, "Canonical
  spelling of map keys", protowire#306). `FormatDocument` wrote every
  identifier-shaped key bare, so `"true": "v"` on a `map<string, V>`
  became `true: "v"` — a bool key, which no longer binds on a string
  `K`. `MapEntry` gains `key_quoted`; a quoted key is unquoted only when
  it is identifier-safe and not `null` / `true` / `false`, and a bare key
  stays bare (so `404:` is no longer requoted on the way through fmt).
  The marshaller uses the same test (`IsIdentifierSafe`, exported from
  `protowire/pxf/format.h`), so `"123"`, `"true"` and `"null"` string
  keys are written quoted; it also sorts bool keys (false, true). The
  spec repo's `testdata/map-keys/` fixtures are vendored and pinned.
- **pxf: bool map keys bind in exactly the grammar's spellings**
  (absorbed into #27; protowire#284). The decoder bound any non-`true`
  key on a `map<bool, V>` to `false` — `t`, `yes`, `"TRUE"`, `"0"` all
  silently became a key the author did not write. A bool key is now
  `true` / `false` bare (the keyword spelling, newly accepted as a map
  key by the parser and decoder), `0` / `1` bare, or `"true"` /
  `"false"` quoted, and anything else is an error naming the key and
  field; on a string `K` the bare keyword is an error that says to
  write it quoted.

## [1.0.0] — 2026-05-13

First major-version cut. Implements the three one-time spec changes
from the protowire v1.0 freeze line (`STABILITY.md` in the spec
repo) in lockstep with `protowire`, `protowire-go`, `protowire-java`
(v1.0.1), `protowire-typescript`, and `protowire-kotlin`.
**Breaking** — there is no alias period; v1.0 is itself the major
bump.

### v1.0 spec changes

- **`@table` → `@dataset` rename** (draft §3.4.4). Public API
  follows: `Ast::TableDirective` → `Ast::DatasetDirective`,
  `Ast::TableRow` → `Ast::DatasetRow`, `TokenKind::kAtTable` →
  `TokenKind::kAtDataset`, `Result::Tables()` → `Result::Datasets()`,
  `Result::AddTable()` → `Result::AddDataset()`,
  `class TableReader` → `class DatasetReader`. Headers
  `protowire/pxf/table_reader.h` → `dataset_reader.h`; source
  `src/pxf/table_reader.cc` → `dataset_reader.cc`. Hard cutover.

- **`@proto` directive added** (draft §3.4.5). New `Ast::ProtoDirective`
  struct + `Ast::ProtoShape` enum (`kAnonymous`, `kNamed`, `kSource`,
  `kDescriptor`). Four body shapes lexically distinguished
  (anonymous `{ ... }`, named `<dotted-name> { ... }`,
  source `"""..."""`, descriptor `b"..."`). Exposed via
  `Document::protos` and `Result::Protos()`. Descriptor form is
  the MUST-support shape per spec; this port supports all four.

- **Reserved directive names** expanded from 5 to 13 (draft §3.4.6).
  `IsFutureReservedDirective(name)` exposed from
  `protowire/pxf/schema.h`. Parser + fast decoder reject `@table`,
  `@datasource`, `@view`, `@procedure`, `@function`,
  `@permissions` as spec-reserved.

`@dataset`'s row message type is now optional in the AST — binding
to an anonymous `@proto` per draft §3.4.4 Anonymous binding.

`Lexer::RepositionTo(int)` added so the parser can skip past an
`@proto` brace-body whose interior is protobuf source rather than
PXF.

### Build

- CMake `project(protowire VERSION ...)` bumped `0.75.0` → `1.0.0`.

### Tests

- New `test/pxf_proto_directive_test.cc` with 11 cases covering all
  four `@proto` body shapes, anonymous binding, multi-`@proto`,
  nested-brace bodies, reserved-name rejection, `@type` coexistence.
- `ctest`: 229 tests, 0 failures.

## [0.75.0] — 2026-05-12

First release after the v0.70.0 baseline that closes the v0.72–v0.75
gap with the rest of the `protowire-*` stack (Go, Java). All four PXF
v0.72-series features are now available in the C++ port, in lockstep
with what the Go and Java ports shipped over their v0.72 → v0.74 →
v0.75 cuts. The cpp port skips intermediate version numbers and lands
the bundled feature set directly on v0.75.0 to match the active wire
revision.

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

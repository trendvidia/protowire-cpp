# protowire4cpp

C++ port of [github.com/trendvidia/protowire](https://github.com/trendvidia/protowire) — a protobuf-backed serialization toolkit.

## Packages

| Package | Header | Notes |
|---------|--------|-------|
| `protowire::pb` | `protowire/pb.h` | Schema-free struct ↔ proto3 binary marshaling. Field numbers come from the `PROTOWIRE_FIELDS(Type, ...)` macro — the C++ analogue of Go's `protowire:"N"` struct tag. |
| `protowire::pxf` | `protowire/pxf.h` | PXF text ↔ `google::protobuf::Message`. Compiled-in or `DynamicMessage` works. Walks the AST produced by `Parse()` and writes through libprotobuf reflection. |
| `protowire::sbe` | `protowire/sbe.h` | FIX SBE binary codec, driven by SBE annotations on `.proto` schemas. `Marshal`, `Unmarshal`, and a zero-allocation `View`. |
| `protowire::envelope` | `protowire/envelope.h` | Standard API response envelope, wire-compatible with the Go module. |

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Command-line tool

The `protowire` CLI is shared across every port and lives in the spec repo at [github.com/trendvidia/protowire/cmd/protowire](https://github.com/trendvidia/protowire/tree/main/cmd/protowire). Install:

```sh
go install github.com/trendvidia/protowire/cmd/protowire@latest
```

C++ users use this library for in-process encode/decode and the shared CLI for command-line operations. There is no separate C++ CLI binary.

## Wire compatibility

Verified manually against the Go module:

- Go `pxf.Marshal` → file → C++ `pxf::Unmarshal` round-trips a representative AllTypes message.
- C++ `pxf::Marshal` → file → Go `pxf.Unmarshal` round-trips equally.

## Coverage status

PXF (`encoding/pxf`):

- ✅ Lexer — full feature parity with Go (single/triple strings, base64 bytes, RFC 3339 timestamps, Go-style durations, comments, dedent).
- ✅ AST + parser, comment-attaching.
- ✅ Schema-bound encoder + decoder via libprotobuf reflection.
- ✅ Scalars, enums, repeated, maps, nested messages.
- ✅ Well-known types: `Timestamp`, `Duration`, all wrapper types (sugar form).
- ✅ Field-presence tracking (`UnmarshalFull` returns a `Result` with `IsSet` / `IsNull` / `IsAbsent`).
- ⏳ `google.protobuf.Any` sugar (block syntax with `@type =`).
- ⏳ `pxf.BigInt` / `pxf.Decimal` / `pxf.BigFloat` sugar inside PXF text (the bytes-only types are wired into `pb` already).
- ⏳ `_null` `FieldMask` discovery and emission across binary round-trips.
- ⏳ `(pxf.required)` / `(pxf.default)` annotation enforcement in `UnmarshalFull`.
- ⏳ AST-preserving `FormatDocument` (use the schema-bound encoder for round-tripping today).
- ⏳ Fused single-pass lexer+decoder optimization (the AST path is the only one wired up).

SBE (`encoding/sbe`):

- ✅ Codec construction from `FileDescriptor`s, schema/version/template-id discovery via SBE annotations.
- ✅ `Marshal` / `Unmarshal` for proto messages, including composites and repeating groups.
- ✅ Type-narrowing via `(sbe.encoding)` overrides (e.g. `uint32 → uint8`).
- ✅ Zero-allocation `View` / `GroupView`.
- ⏳ XML schema parsing (file stubs are present in `src/sbe/xml*.cc`). The `sbe2proto` / `proto2sbe` CLI subcommands are provided by the shared CLI in the spec repo, not by this library.

`pb` (`encoding/pb`):

- ✅ Wire format for all proto3 scalar types, repeated, embedded messages.
- ✅ `BigInt`, `Decimal`, `BigFloat` byte-backed types matching `pxf.BigInt`/`Decimal`/`BigFloat` schemas.
- ✅ Unknown-field skipping on decode.
- ⏳ Optional GMP/Boost.Multiprecision adapter header (interface lives in `protowire/pb_big.h`; library glue not yet shipped).

`envelope`:

- ✅ Full parity for `Envelope`, `AppError`, `FieldError`, builders, and queries.
- ⏳ `metadata` map serialization on the wire (in-memory works; PB map encoding requires more macro plumbing).

## Repository layout

```
protowire-cpp/
├── CMakeLists.txt
├── proto/                              # vendored .proto files
├── include/protowire/                  # public headers
├── src/{pb,pxf,sbe,envelope,detail}/   # implementations
├── cmd/{bench_pxf,bench_sbe,dump_envelope}/  # cross-port test harnesses
├── third_party/CLI11.hpp               # vendored single-header (used by harnesses)
├── testdata/                           # test.proto + example.pxf (from Go module)
└── test/                               # GoogleTest suites
```

## Notes for macOS users

Homebrew's `protobuf` package is current; older installs at `/usr/local/include/google/protobuf/` will silently win the include-path race. The CMake build detects this and prepends `/opt/homebrew/include` to the compile search path; if you have similar issues elsewhere, removing the stale install is the cleanest fix.

## Limitations & open gaps

The C++ port targets `protobuf` (`libprotobuf` / `libprotobuf-lite`) and `abseil`; a few items fall out of that or are deferred:

- **C++17 minimum**, with C++20-friendly internals where it doesn't break the public ABI. Toolchains pinning to C++14 are not supported.
- **No header-only single-include build.** The library is structured for static linking via CMake. A header-only mode is a frequent ask but requires inline'ing the protobuf descriptor wiring and is open work.
- **Exceptions are used internally.** Embedded targets that compile with `-fno-exceptions` are not supported today; converting the public surface to `std::expected`-style error returns is open work.
- **macOS Homebrew include-path quirk** documented above is a known footgun. The CMake build handles it; if you wire this library into a non-CMake build, mirror the include-order tweak.
- **The CLI lives in [trendvidia/protowire/cmd/protowire](https://github.com/trendvidia/protowire/tree/main/cmd/protowire), not here.** This repo ships only the cross-port test harnesses (`cmd/bench_pxf`, `cmd/bench_sbe`, `cmd/dump_envelope`).
- **SBE schema XML import is one-way at runtime.** `proto2sbe` is shipped via the harness; full bidirectional XML / `.proto` interop happens in the shared CLI.

## Contributing & governance

This repository is part of the `protowire-*` family and is governed by [**Steward**](https://github.com/trendvidia/steward) — the meritocratic, AI-driven governance engine that runs all of the ports. Voting weight is per-directory expertise, the constitution is public in [`governance.pxf`](https://github.com/trendvidia/steward/blob/main/governance.pxf), and Steward routes draft / first-time PRs through a [private mentorship pipeline](https://github.com/trendvidia/steward#-private-mentorship-mode) so initial contributions get private feedback rather than public-review friction.

If any of the items above sound interesting, pull requests are welcome. New contributors start at zero trust and accumulate influence by shipping merged PRs in the directories they actually work on — the [escrow pipeline](https://github.com/trendvidia/steward#%EF%B8%8F-the-escrow-pipeline-zero-trust-onboarding) auto-routes large first-time PRs through 2–3 sandbox issues before unlocking them for community review.

See the [Steward README](https://github.com/trendvidia/steward) for a longer walkthrough of vector reputation, escrow, and the immune system.

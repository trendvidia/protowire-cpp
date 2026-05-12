# Changelog

All notable changes to `protowire-cpp` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The version number is kept aligned with the rest of the `protowire-*`
stack — releases bump in lockstep across language ports when the wire
format changes.

## [Unreleased]

### Added

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

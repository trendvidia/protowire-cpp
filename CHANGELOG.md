# Changelog

All notable changes to `protowire-cpp` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The version number is kept aligned with the rest of the `protowire-*`
stack — releases bump in lockstep across language ports when the wire
format changes.

## [Unreleased]

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

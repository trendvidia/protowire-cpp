# protowire-cpp

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/trendvidia/protowire-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/trendvidia/protowire-cpp/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/trendvidia/protowire-cpp/branch/main/graph/badge.svg)](https://codecov.io/gh/trendvidia/protowire-cpp)

C++ port of [protowire](https://protowire.org) — a protobuf-backed wire-format
toolkit. C++20, MIT, CMake. Verified for byte-equivalence against the
canonical Go reference and seven other sibling ports.

CI exercises **Linux × {gcc-13, clang-18}**, **macOS × AppleClang**, and
**Windows × MSVC**, plus a dedicated **AddressSanitizer + UBSan** job
that runs the full test suite under sanitizers on every PR. The Python
port (`protowire-python`) consumes this library through nanobind FFI.

## Packages

| Package | Header | Notes |
|---------|--------|-------|
| `protowire::pb` | `protowire/pb.h` | Schema-free struct ↔ proto3 binary marshaling. Field numbers come from the `PROTOWIRE_FIELDS(Type, ...)` macro — the C++ analogue of Go's `protowire:"N"` struct tag. |
| `protowire::pxf` | `protowire/pxf.h` | PXF text ↔ `google::protobuf::Message`. Compiled-in or `DynamicMessage` works. Walks the AST produced by `Parse()` and writes through libprotobuf reflection. |
| `protowire::sbe` | `protowire/sbe.h` | FIX SBE binary codec, driven by SBE annotations on `.proto` schemas. `Marshal`, `Unmarshal`, and a zero-allocation `View`. |
| `protowire::envelope` | `protowire/envelope.h` | Standard API response envelope, wire-compatible with the Go module. |

## Use it

### CMake `find_package`

After `cmake --install build --prefix <dir>`, consumers add three lines:

```cmake
find_package(protowire CONFIG REQUIRED)

target_link_libraries(my_app PRIVATE
  protowire::pxf
  protowire::sbe
  protowire::envelope)
```

The package transitively pulls in `protobuf::libprotobuf`; you don't
need to call `find_package(Protobuf)` yourself.

### CMake FetchContent

For projects that prefer a source-level dependency:

```cmake
include(FetchContent)
FetchContent_Declare(protowire
  GIT_REPOSITORY https://github.com/trendvidia/protowire-cpp.git
  GIT_TAG        v0.70.0)
FetchContent_MakeAvailable(protowire)

target_link_libraries(my_app PRIVATE protowire::pxf)
```

Pass `-DPROTOWIRE_INSTALL=OFF` if you want the bundled FetchContent
copy not to leak install rules into your parent build.

### System packages

`apt`, `brew`, vcpkg, and Conan ports are not yet in their respective
registries; submission is on the roadmap. Until then, build from source
or use the FetchContent path above.

## Build from source

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Required: CMake ≥ 3.20, a C++20 compiler, protobuf headers + libs.

- Linux: `apt-get install protobuf-compiler libprotobuf-dev`
- macOS: `brew install protobuf`
- Windows: `vcpkg install protobuf` and pass `-DCMAKE_TOOLCHAIN_FILE=<vcpkg root>/scripts/buildsystems/vcpkg.cmake`

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
├── LICENSE                                  # MIT
├── README.md
├── CHANGELOG.md
├── CONTRIBUTING.md, SECURITY.md,
│   GOVERNANCE.md, CODE_OF_CONDUCT.md
├── CMakeLists.txt                           # build + install + find_package
├── cmake/
│   └── protowireConfig.cmake.in             # consumed by find_package
├── proto/                                   # vendored .proto annotation files
├── include/protowire/                       # public headers
├── src/{pb,pxf,sbe,envelope,detail}/        # implementations
├── cmd/{bench_pxf,bench_sbe,dump_envelope}/ # cross-port test harnesses
├── third_party/CLI11.hpp                    # vendored single-header (harnesses)
├── testdata/                                # test.proto + example.pxf
├── test/                                    # GoogleTest suites
├── .clang-format, .clang-tidy               # style + lint enforced by CI
├── .editorconfig
└── .github/                                 # CI: build matrix + sanitizers + CodeQL
```

## Notes for macOS users

Homebrew's `protobuf` package is current; older installs at `/usr/local/include/google/protobuf/` will silently win the include-path race. The CMake build detects this and prepends `/opt/homebrew/include` to the compile search path; if you have similar issues elsewhere, removing the stale install is the cleanest fix.

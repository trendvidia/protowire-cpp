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

Options:

- `-DPROTOWIRE_BUILD_CLI=ON` — build the `protowire` CLI (off by default).
- `-DPROTOWIRE_WITH_REGISTRY=ON` — link the gRPC client for the remote `protoregistry` service. Requires `gRPC` C++ to be findable via `find_package(gRPC CONFIG)`.

## CLI

Standalone (compile schemas locally):

```sh
protowire encode   -p schema.proto -m pkg.Type input.pxf > out.pb
protowire decode   -p schema.proto -m pkg.Type input.pb  > out.pxf
protowire validate -p schema.proto -m pkg.Type input.pxf
protowire fmt      -p schema.proto -m pkg.Type input.pxf
```

Registry mode (talk to the remote `protoregistry` gRPC service):

```sh
protowire encode -s host:port -n NS --schema NAME -m pkg.Type input.pxf
```

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
- ⏳ XML schema parsing and the `sbe2proto` / `proto2sbe` converters (file stubs are present in `src/sbe/xml*.cc`).

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
protowire4cpp/
├── CMakeLists.txt
├── proto/                              # vendored .proto files
├── include/protowire/                  # public headers
├── src/{pb,pxf,sbe,envelope,detail}/   # implementations
├── cmd/protowire/main.cc               # CLI
├── third_party/CLI11.hpp               # vendored single-header
├── testdata/                           # test.proto + example.pxf (from Go module)
└── test/                               # GoogleTest suites
```

## Notes for macOS users

Homebrew's `protobuf` package is current; older installs at `/usr/local/include/google/protobuf/` will silently win the include-path race. The CMake build detects this and prepends `/opt/homebrew/include` to the compile search path; if you have similar issues elsewhere, removing the stale install is the cleanest fix.

# Contributing to protowire-cpp

Welcome — this is the C++ port of [protowire](https://protowire.org), a
language-neutral wire-format toolkit. It tracks the canonical
specification in
[`trendvidia/protowire`](https://github.com/trendvidia/protowire) and is
one of nine sibling ports (Go, C++, Rust, Java, TypeScript, Python, C#,
Swift, Dart). The Python port (`protowire-python`) consumes this library
through nanobind FFI — anything that affects this port's runtime
behaviour also affects Python.

> **Steward integration is rolling out.** The governance described in
> [GOVERNANCE.md](GOVERNANCE.md) is the steady-state model. While Steward
> is being finalised, pull requests are reviewed by human maintainers in
> the conventional way — open a PR, expect review, iterate.

## Where bugs go

| Symptom | File against |
|---|---|
| C++ port-only crash, wrong API ergonomics, performance regression in this port only | `trendvidia/protowire-cpp` |
| Python crash that does NOT reproduce in C++ directly | `trendvidia/protowire-python` |
| The same input produces different output here vs another port | upstream [`trendvidia/protowire`](https://github.com/trendvidia/protowire) (cross-port wire-equivalence regression) |
| Spec / grammar / proto annotation question | upstream [`trendvidia/protowire`](https://github.com/trendvidia/protowire) |
| Decoder crash / hang / OOM on adversarial input | **email security@trendvidia.com**, do not file public issue (see [SECURITY.md](SECURITY.md)) |

## Build matrix

C++20, CMake ≥ 3.20. Tested in CI on:

- Linux × {gcc-13, clang-18}
- macOS × AppleClang (Xcode latest)
- Windows × MSVC (latest VS Build Tools)

Plus an ASan + UBSan job on Linux/clang.

## Local development

```sh
# Configure + build (Release-with-debug-info by default)
cmake -S . -B build
cmake --build build -j

# Run tests
ctest --test-dir build --output-on-failure

# Or run a single suite directly
./build/test/protowire_pxf_test --gtest_filter='ParseEntry.*'
```

### Sanitizer build (recommended when changing the parser)

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
```

The CI's ASan job runs the full test suite + the upstream adversarial
corpus from [`testdata/adversarial/`](https://github.com/trendvidia/protowire/tree/main/testdata/adversarial)
under sanitizers; failures there are blockers, not warnings.

### protobuf

Linux: `apt-get install protobuf-compiler libprotobuf-dev`
macOS: `brew install protobuf`
Windows: `vcpkg install protobuf` (CI uses this; local: same command after `bootstrap-vcpkg.bat`)

CMake auto-detects via `find_package(Protobuf REQUIRED)`; no version
pinning required, but ≥ 3.21 is the floor we test against.

## Sending changes

1. Open a draft PR early.
2. **For changes that touch the parser/encoder behaviour**: comment with
   which fixtures from `test/pxf/testdata/` you exercised. Cross-port
   wire-equivalence means a wrong move here can break six other ports'
   contracts.
3. **For changes that touch the wire format itself** — annotation field
   numbers in `proto/`, the PXF grammar, the SBE schema-id semantics —
   open the upstream PR in
   [`trendvidia/protowire`](https://github.com/trendvidia/protowire)
   first. This port shouldn't lead spec changes; it implements them.
4. **Anything that adds a new public symbol** must be reflected in the
   exported CMake target (`protowire_*`) and surface through the
   installed config package, not just live as an internal helper.

## Code style

- C++20 is the floor (we use `concepts`, `ranges`, `<bit>`, `consteval`).
- `clang-format` is enforced in CI; your editor should be configured to
  format on save against the root `.clang-format` (4 spaces, 100-col).
- `clang-tidy` runs in CI with the `bugprone-*`, `performance-*`,
  `modernize-use-*` rule sets. Suppress with `// NOLINT(rule-name)` and
  a one-line comment explaining why.
- Public headers use `org::protowire::` namespace nesting that mirrors
  the install path (`#include <protowire/pxf/parser.h>`).
- Match the existing zero-allocation patterns in `:sbe` — the `View` API
  is the "zero allocation" reference point.

## What we don't accept

- Changes that break wire-equivalence with another sibling port.
- Compiler-specific intrinsics without a portable fallback (`__builtin_*`,
  `__attribute__(...)` outside `third_party/`).
- New top-level dependencies without a one-line justification in the PR
  description. We currently depend only on protobuf + GTest.
- Static analysis suppressions on a whole file or whole function. Keep
  them line-scoped.

## Releases

This port releases in lockstep with the rest of the `protowire-*` stack.
The version line is `0.70.x` for the first coordinated public release;
ports that share a `0.70.x` minor implement the same wire contract.

Cutting a release:

1. Bump `project(... VERSION X.Y.Z ...)` in `CMakeLists.txt`.
2. Add a `## [X.Y.Z]` section to `CHANGELOG.md`.
3. Tag `vX.Y.Z` on `main`.
4. The `.github/workflows/release.yml` workflow will produce build
   artifacts and post a GitHub Release. (Conan / vcpkg distribution is
   intentionally deferred to a later milestone.)

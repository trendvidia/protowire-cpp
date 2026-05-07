<!--
For changes that touch wire-format behaviour: please open the upstream
PR in trendvidia/protowire FIRST. This port implements the spec; it
shouldn't lead spec changes. See CONTRIBUTING.md.

For changes touching the SBE View / GroupView zero-allocation
primitives, or any pointer arithmetic / static_cast / reinterpret_cast
in parser hot paths: include an ASan/UBSan-clean justification.
-->

## Summary

What this PR changes, in 1–3 sentences.

## Why

Link to the issue or upstream spec change that motivated this.

## Scope

- [ ] Wire-impacting source (`include/protowire/`, `src/pxf/`, `src/sbe/`, `src/pb/`, `src/envelope/`, `proto/`)
- [ ] Test fixtures / benches (`test/`, `cmd/`)
- [ ] Build / CI / repo plumbing (`CMakeLists.txt`, `cmake/`, `.github/`)
- [ ] `third_party/` vendored library (license review attached)
- [ ] Documentation only

## Test plan

- [ ] Local build clean: `cmake --build build -j && ctest --test-dir build --output-on-failure`
- [ ] If parser/encoder change: ASan + UBSan build also clean
- [ ] If wire-impacting: cross-port harness re-run locally via
      [`scripts/cross_*.sh`](https://github.com/trendvidia/protowire/tree/main/scripts) in the spec repo
- [ ] If protocol-touching: matching upstream spec PR linked above
- [ ] If new public symbol: surfaces through the installed CMake config
      package (`find_package(Protowire CONFIG REQUIRED)`)

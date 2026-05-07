# Governance

`protowire-cpp` is governed under the same constitution as the rest of
the `protowire-*` stack. The machine-readable source of truth lives in
the upstream spec repo at
[`governance.pxf`](https://github.com/trendvidia/protowire/blob/main/governance.pxf);
the human-readable preamble is at
[`GOVERNANCE.md`](https://github.com/trendvidia/protowire/blob/main/GOVERNANCE.md).

This file is a short pointer-doc. If anything below disagrees with the
upstream constitution, the upstream wins.

## Domain ownership

This repo's only domain vector is
[`protowire-cpp`](https://github.com/trendvidia/protowire/blob/main/governance.pxf)
under the upstream `port-libraries` umbrella. Approval requirements:

| Path | Reviewer authority |
|---|---|
| `include/protowire/`, `src/pb/`, `src/pxf/`, `src/sbe/`, `src/envelope/` | port maintainers (`@trendvidia/maintainers`); native-code memory-safety scrutiny |
| `proto/` | upstream spec maintainers — these mirror `trendvidia/protowire/proto/` and may not diverge |
| `cmd/`, `test/` | port maintainers |
| `third_party/` | maintainers only — adding or updating a vendored library needs a license review and a justification in the PR |
| `.github/`, `CMakeLists.txt`, `.clang-tidy`, `.clang-format` | port maintainers |

## What's enforced today vs (roadmap)

The Steward agent that enforces the constitution programmatically is
**rolling out**. Until it is live:

- Pull requests are reviewed by human maintainers.
- The `0.70.x` release line implements the wire contract documented in
  [`docs/grammar.ebnf`](https://github.com/trendvidia/protowire/blob/main/docs/grammar.ebnf)
  + [`docs/HARDENING.md`](https://github.com/trendvidia/protowire/blob/main/docs/HARDENING.md);
  the ASan + UBSan job is the local enforcement of the hardening
  invariants.
- Reputation-weighted voting, automatic escrow for risky changes, and
  the `manifesto.blocked_module_globs` restriction are all `(roadmap)`
  per the upstream `governance.pxf`.

## Stable surfaces

Everything in these public namespaces is part of this port's SemVer
contract:

- `org::protowire::pb::*`
- `org::protowire::pxf::*`
- `org::protowire::sbe::*`
- `org::protowire::envelope::*`

Headers under `include/protowire/internal/` (or any namespace ending in
`::detail`) are not stable.

The wire contract — what bytes a given proto message produces — is
governed by the **upstream** spec, not this port. Bumping the wire
contract requires a coordinated PR landing in every sibling port; see
[`STABILITY.md`](https://github.com/trendvidia/protowire/blob/main/STABILITY.md)
upstream.

## Native-code particulars

C++ touches a class of bugs the JVM and managed-runtime ports do not
(memory unsafety, ABI breaks, undefined behaviour). The constitution
treats those as a higher-severity tier:

- Any change to the SBE `View` / `GroupView` zero-allocation primitives
  needs explicit maintainer approval.
- Any change to `static_cast`, pointer arithmetic, or
  `reinterpret_cast` in parser/encoder hot paths needs an
  ASan/UBSan-clean justification in the PR.
- New uses of compiler intrinsics (`__builtin_*`, `__attribute__(...)`)
  outside `third_party/` are blocked unless the PR documents the MSVC
  fallback path.

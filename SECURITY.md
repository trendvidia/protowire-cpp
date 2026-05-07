# Security Policy

## Reporting a vulnerability

Email **security@trendvidia.com** with a description, reproduction steps,
and the affected version(s) or commit(s). PGP key on request.

Please do **not** file public GitHub issues for vulnerabilities, and do
**not** post details in pull request comments.

You can expect:

- An acknowledgement within **3 business days**.
- A triage decision (accepted / not-a-vulnerability / needs-more-info)
  within **10 business days**.
- A coordinated fix on the timeline below.

## Scope

This policy covers `protowire-cpp` — the C++ port of the `protowire`
stack. The Python port (`protowire-python`) wraps this library through
nanobind FFI, so vulnerabilities here also apply there. Cross-port
issues are also accepted here and routed to the upstream project; you
can equivalently file at
[`trendvidia/protowire`](https://github.com/trendvidia/protowire) per
its [`SECURITY.md`](https://github.com/trendvidia/protowire/blob/main/SECURITY.md).

In scope:

- Decoder crashes, hangs, infinite loops, unbounded memory, or OOMs
  triggered by adversarial PXF / PB / SBE / envelope input.
- Wire-format divergences from other ports for the same input that
  could be exploited (e.g. authorization bypass via parser
  disagreement).
- Schema-validation bypasses that let invalid messages reach
  application code.
- **Native-code memory-safety issues**: buffer overflows, use-after-free,
  type confusion, integer overflows in length/size math, ABI mismatches
  in the View / GroupView SBE primitives. These are the highest-priority
  class of report we receive — please flag even theoretical paths.

Out of scope:

- Denial-of-service via legitimately large inputs that respect the
  limits in the upstream
  [`docs/HARDENING.md`](https://github.com/trendvidia/protowire/blob/main/docs/HARDENING.md).
- Issues in `protobuf` itself — file those upstream at
  [`protocolbuffers/protobuf`](https://github.com/protocolbuffers/protobuf)
  and CC us.

## Hardening floor

Every release is built and tested in CI with **AddressSanitizer +
UndefinedBehaviorSanitizer** against the full upstream adversarial
corpus ([`testdata/adversarial/`](https://github.com/trendvidia/protowire/tree/main/testdata/adversarial)).
A sanitizer fail blocks release. So the bar for "is this a memory-safety
bug" is "ASan/UBSan didn't catch it in CI" — every accepted report
implies a corpus gap that we'll commit alongside the fix.

## Coordinated disclosure

For vulnerabilities affecting **more than one port**, a **30-day
embargo** applies from the date we acknowledge your report (per the
upstream project's policy), extendable by mutual agreement when a fix
needs more time.

Single-port issues follow this port's own disclosure timeline,
typically 7–14 days, but always at least long enough for a fix to be
released.

## Hall of fame

Reporters who follow coordinated disclosure are credited in
`SECURITY-ADVISORY-*.md` advisories on the upstream repo and (with
permission) in the release notes. We do not currently run a paid
bug-bounty program.

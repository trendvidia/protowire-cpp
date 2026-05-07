---
name: Feature request
about: Propose a C++-port-only API addition or ergonomics improvement
title: "feat: "
labels: enhancement
---

<!--
Wire-format / spec / annotation proposals belong upstream at
trendvidia/protowire — they affect every port. This template is for
C++-PORT-ONLY changes (better ergonomics, new convenience overloads,
performance improvements that don't affect the wire output, support
for a new compiler / OS).
-->

## Problem

What's awkward to express today, or what's missing?

## Proposal

What you'd like to add. If it's a new public API, sketch the signature
and the typical call-site. If it's a perf change, ideally include a
microbench number from `cmd/bench-pxf` or `cmd/bench-sbe`.

## Alternatives considered

What else you tried, and why it isn't enough.

## Out of scope (optional)

Things this proposal is **not** trying to do, to keep review focused.

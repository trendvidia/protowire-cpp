---
name: Bug report
about: Report a defect — wrong output, crash, parse error on valid input, etc.
title: "bug: "
labels: bug
---

<!--
Cross-port issues (the same input behaves differently on multiple ports)
belong upstream at trendvidia/protowire, not here. See CONTRIBUTING.md.

Security issues (decoder crash/hang/OOM on adversarial input, ASan
findings, UBSan findings, ABI mismatches) go to security@trendvidia.com
instead. See SECURITY.md.
-->

## What happened

A clear description of the bug.

## How to reproduce

Smallest possible PXF / PB / SBE / envelope input + C++ snippet that
triggers it.

```cpp
#include <protowire/pxf/parser.h>

auto doc = protowire::pxf::Parse("@type my.Type\nname = \"x\"\n");
// throws here?
```

## What you expected

What you thought should happen.

## Versions

- `protowire-cpp` version (tag or commit):
- Compiler + version (`gcc --version`, `clang --version`, `cl.exe`):
- OS / arch:
- Protobuf version (`protoc --version`):
- CMake version (`cmake --version`):

## Sanitizer findings (if any)

If you can reproduce under ASan / UBSan / TSan, paste the report here.
A sanitizer hit is the highest-priority class of bug.

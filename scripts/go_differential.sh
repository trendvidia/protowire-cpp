#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 TrendVidia, LLC.
#
# Runs protowire-go and this port over the spec's fixtures and this repo's
# testdata/differential, and fails when they differ. protowire-go is the
# reference, so a difference is a bug here unless it is listed in
# scripts/go_differential.accepted.
#
#   SPEC_DIR=../protowire GO_DIR=../protowire-go PXF_DIFF=build/bin/pxf_diff \
#     scripts/go_differential.sh
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
spec=$(cd "${SPEC_DIR:-$here/../protowire}" && pwd)
gosrc=$(cd "${GO_DIR:-$here/../protowire-go}" && pwd)
pxf_diff=${PXF_DIFF:-$here/build/bin/pxf_diff}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

manifest="$work/manifest"
: > "$manifest"
add_dir() { # root dir [extra protos]
  local root=$1 dir=$2 extra=${3:-}
  local protos
  protos=$(cd "$root/$dir" && ls | awk '/\.proto$/' | paste -sd, -)
  protos=${protos:+$protos}${extra:+${protos:+,}$extra}
  (cd "$root" && find "$dir" -name '*.pxf' | LC_ALL=C sort) |
    awk -v r="$root" -v p="$protos" '{ print r "\t" p "\t" $0 }' >> "$manifest"
}
for dir in annotations any bigfloat blocks duplicates duration keyed map-keys \
           nulls strings; do
  add_dir "$spec/testdata" "$dir"
done
add_dir "$spec/testdata" dataset test.proto
add_dir "$spec/testdata" repeated test.proto
add_dir "$spec/testdata" proto-imports
add_dir "$here/testdata" differential

cp -R "$here/tools/godiff" "$work/godiff"
chmod -R u+w "$work/godiff"
(
  cd "$work/godiff"
  export GOWORK=off GOFLAGS=-mod=mod
  go mod edit -replace "github.com/trendvidia/protowire-go=$gosrc"
  go mod tidy >/dev/null 2>&1
  go build -o "$work/godiff.bin" .
)

"$work/godiff.bin" "$manifest" "$spec/proto" > "$work/go.out"
"$pxf_diff" "$manifest" "$spec/proto" > "$work/cpp.out"

"$work/godiff.bin" --quote-runes > "$work/go.quote"
"$pxf_diff" --quote-runes > "$work/cpp.quote"
quoted=$(wc -l < "$work/go.quote" | tr -d ' ')
if ! diff "$work/go.quote" "$work/cpp.quote" > "$work/quote.diff"; then
  echo "strconv.Quote differs; if Go moved Unicode, regenerate src/detail/go_isprint.inc:" >&2
  head -20 "$work/quote.diff" >&2
  exit 1
fi
echo "$quoted strings quoted alike"

docs=$(wc -l < "$manifest" | tr -d ' ')
for f in go cpp; do
  ran=$(LC_ALL=C awk '/^=== /{n++} END{print n+0}' "$work/$f.out")
  if [ "$ran" != "$docs" ]; then
    echo "$f ran $ran of $docs documents" >&2
    exit 2
  fi
done
echo "$docs documents, $(wc -l < "$work/go.out" | tr -d ' ') lines of Go output"

[ -n "${DIFF_OUT:-}" ] && cp "$work/go.out" "$DIFF_OUT.go" && cp "$work/cpp.out" "$DIFF_OUT.cpp"
if diff -u "$work/go.out" "$work/cpp.out" > "$work/diff"; then
  echo "no differences"
  exit 0
fi
cat "$work/diff"
exit 1

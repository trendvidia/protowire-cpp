#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 TrendVidia, LLC.
#
# Runs protowire-go and this port over the spec's fixtures and this repo's
# testdata/differential, and fails when they differ. protowire-go is the
# reference, so a difference is a bug here.
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
add_dir() { # root dir [extra protos] [type of a document with no @type]
  local root=$1 dir=$2 extra=${3:-} type=${4:--}
  local protos
  protos=${ONLY_PROTOS:-$(cd "$root/$dir" && ls | awk '/\.proto$/' | paste -sd, -)}
  protos=${protos:+$protos}${extra:+${protos:+,}$extra}
  (cd "$root" && find "$dir" -path "$dir/${SKIP_DIR:-}" -prune -o -name '*.pxf' -print |
    LC_ALL=C sort) |
    awk -v r="$root" -v d="$dir" -v p="$protos" -v t="$type" \
      '{ print r "\t" d "\t" p "\t" t "\t" $0 }' >> "$manifest"
}
for dir in any blocks duplicates duration keyed map-keys \
           nulls strings; do
  add_dir "$spec/testdata" "$dir"
done
add_dir "$spec/testdata" bigfloat "" bf.v1.H
# settings_annotated.proto needs the @required sugar's compiler plugin.
ONLY_PROTOS=settings.proto add_dir "$spec/testdata" annotations "" settings.v1.Settings
add_dir "$spec/testdata" dataset test.proto
add_dir "$spec/testdata" repeated test.proto
add_dir "$spec/testdata" proto-imports
SKIP_DIR=wkt-shape add_dir "$here/testdata" differential
# Each of these schemas needs its own run: most redefine well-known types, and
# google-path-own has a file at a google/protobuf path.
for dir in "$here"/testdata/differential/wkt-shape/*/; do
  add_dir "$here/testdata" "differential/wkt-shape/$(basename "$dir")"
done

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

# A table of literals on every kind of field, through ApplyDefault.
"$work/godiff.bin" --defaults "$here/testdata" differential "$spec/proto" > "$work/go.defaults"
"$pxf_diff" --defaults "$here/testdata" differential "$spec/proto" > "$work/cpp.defaults"
if ! cmp -s "$work/go.defaults" "$work/cpp.defaults"; then
  echo "ApplyDefault differs:" >&2
  diff "$work/go.defaults" "$work/cpp.defaults" | head -"${DEFAULTS_DIFF_LINES:-40}" >&2 || true
  exit 1
fi
applied=$(LC_ALL=C awk '/: ok /{n++} END{print n+0}' "$work/go.defaults")
refused=$(LC_ALL=C awk '/^bytes: /{n++} END{print n+0}' "$work/go.defaults")
echo "$((applied + refused)) defaults applied alike: $applied accepted, $refused refused"

# Every prefix and every one-byte deletion of every document, the states an
# editor buffer goes through, through Parse and ParseTolerant.
"$work/godiff.bin" --tolerant "$manifest" > "$work/go.tolerant"
"$pxf_diff" --tolerant "$manifest" > "$work/cpp.tolerant"
if ! cmp -s "$work/go.tolerant" "$work/cpp.tolerant"; then
  echo "ParseTolerant differs:" >&2
  diff "$work/go.tolerant" "$work/cpp.tolerant" | head -"${TOLERANT_DIFF_LINES:-40}" >&2 || true
  [ -n "${DIFF_OUT:-}" ] && cp "$work/go.tolerant" "$DIFF_OUT.go.tolerant" &&
    cp "$work/cpp.tolerant" "$DIFF_OUT.cpp.tolerant"
  exit 1
fi
states=$(LC_ALL=C awk '/^=== /{n++} END{print n+0}' "$work/go.tolerant")
broken=$(LC_ALL=C awk '/^error /{n++} END{print n+0}' "$work/go.tolerant")
bad=$(LC_ALL=C awk '/[0-9]+ bad$/{n++} END{print n+0}' "$work/go.tolerant")
echo "$states editor states parsed alike: $broken tolerant errors, $bad placeholders"

# The same Rewriter edits through both ports: every call's error text and the
# final bytes. A run with no ops is a failure, so a broken driver cannot pass.
rewrite_dir=$here/testdata/differential/rewrite
"$work/godiff.bin" --rewrite "$rewrite_dir" > "$work/go.rewrite"
"$pxf_diff" --rewrite "$rewrite_dir" > "$work/cpp.rewrite"
if ! cmp -s "$work/go.rewrite" "$work/cpp.rewrite"; then
  echo "Rewriter differs:" >&2
  diff "$work/go.rewrite" "$work/cpp.rewrite" | head -"${REWRITE_DIFF_LINES:-40}" >&2 || true
  [ -n "${DIFF_OUT:-}" ] && cp "$work/go.rewrite" "$DIFF_OUT.go.rewrite" &&
    cp "$work/cpp.rewrite" "$DIFF_OUT.cpp.rewrite"
  exit 1
fi
ops=$(LC_ALL=C awk '/^op [0-9]+ /{n++} END{print n+0}' "$work/go.rewrite")
op_errors=$(LC_ALL=C awk '/^op [0-9]+ .*: err "/{n++} END{print n+0}' "$work/go.rewrite")
scripted=$(LC_ALL=C awk '/^=== .*\.ops:/{s=0} /^=== sweep /{s=1} /^op [0-9]+ /{if(!s)n++} END{print n+0}' \
  "$work/go.rewrite")
if [ "$ops" -lt 60 ]; then
  echo "Rewriter ran only $ops operations" >&2
  exit 2
fi
echo "$ops rewriter operations alike ($scripted scripted), $op_errors of them returned errors"

# The pb and sbe stream codecs over every truncation and many corruptions of a
# stream: the bytes they write, each Decode's result and error text.
"$work/godiff.bin" --streams "$here/testdata" differential/stream "$spec/proto" > "$work/go.streams"
"$pxf_diff" --streams "$here/testdata" differential/stream "$spec/proto" > "$work/cpp.streams"
if ! cmp -s "$work/go.streams" "$work/cpp.streams"; then
  echo "Stream codecs differ:" >&2
  diff "$work/go.streams" "$work/cpp.streams" | head -"${STREAM_DIFF_LINES:-40}" >&2 || true
  [ -n "${DIFF_OUT:-}" ] && cp "$work/go.streams" "$DIFF_OUT.go.streams" &&
    cp "$work/cpp.streams" "$DIFF_OUT.cpp.streams"
  exit 1
fi
variants=$(LC_ALL=C awk '/^--- /{n++} END{print n+0}' "$work/go.streams")
decoded=$(LC_ALL=C awk '/^ok /{n++} END{print n+0}' "$work/go.streams")
framing=$(LC_ALL=C awk '/stream framing corrupt/{n++} END{print n+0}' "$work/go.streams")
if [ "$variants" -lt 900 ] || [ "$decoded" -lt 100 ] || [ "$framing" -lt 100 ]; then
  echo "Streams ran only $variants variants, $decoded decodes, $framing framing errors" >&2
  exit 2
fi
echo "$variants damaged streams decoded alike: $decoded messages, $framing framing errors"

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

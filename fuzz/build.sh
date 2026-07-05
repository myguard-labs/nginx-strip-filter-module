#!/usr/bin/env bash
# Build the six strip_core libFuzzer targets.
# Usage: bash fuzz/build.sh [fuzz-dir]
set -euo pipefail

FUZZ_DIR="${1:-$PWD/fuzz}"
ROOT="$(dirname "$FUZZ_DIR")"

CC="${CC:-clang}"
CFLAGS="-O1 -g -fsanitize=fuzzer,address,undefined \
  -fno-sanitize-recover=undefined \
  -fno-omit-frame-pointer \
  -I$ROOT"

declare -A KINDS=(
    [html]=0
    [css]=1
    [js]=2
    [json]=3
    [svg]=4
    [xml]=5
)

for name in html css js json svg xml; do
    kind="${KINDS[$name]}"
    echo "building fuzz_strip_${name} (FUZZ_KIND=${kind})..."
    mkdir -p "$FUZZ_DIR/corpus_${name}"
    # shellcheck disable=SC2086
    "$CC" $CFLAGS \
        -DFUZZ_KIND="${kind}" \
        "$FUZZ_DIR/fuzz_strip.c" \
        "$ROOT/strip_core.c" \
        -o "$FUZZ_DIR/fuzz_strip_${name}"
done
echo "done"

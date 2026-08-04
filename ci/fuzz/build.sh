#!/usr/bin/env bash
# Build the six strip_core libFuzzer targets.
# Usage: bash ci/fuzz/build.sh [fuzz-dir]
set -euo pipefail

FUZZ_DIR="${1:-$PWD/ci/fuzz}"
# ci/fuzz -> ci -> repo root; the C now lives under src/.
ROOT="$(cd "$FUZZ_DIR/../.." && pwd)"
SRC="$ROOT/src"

CC="${CC:-clang}"
CFLAGS="-O1 -g -fsanitize=fuzzer,address,undefined \
  -fno-sanitize-recover=undefined \
  -fno-omit-frame-pointer \
  -I$SRC"

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
        "$SRC/strip_core.c" \
        -o "$FUZZ_DIR/fuzz_strip_${name}"
done
echo "done"

#!/usr/bin/env bash
# Copyright (C) 2026 Thijs Eilander
# SPDX-License-Identifier: BSD-2-Clause
#
# ci/tests/unit/run.sh -- build and run the strip-core unit tests.
#
#   ci/tests/unit/run.sh            # build with warnings-as-errors, then run
#   ci/tests/unit/run.sh clean      # remove the built binary and objects
#   COVERAGE=1 ci/tests/unit/run.sh # also instrument, so ci/tools/coverage.sh
#                                   # can gcov src/ afterwards
#
# Env:
#   CC   compiler (default cc). Takes a full driver line, so CC="gcc -m32"
#        runs the suite as a 32-bit binary.
#
# Exit: 0 all checks passed, 1 a check failed or the build failed.
#
# NO NGINX BUILD TREE IS NEEDED, and that is a property of this module rather
# than a shortcut. src/strip_core.c takes (kind, const unsigned char *, size_t,
# unsigned char *) and returns a byte count: no ngx_http_request_t, no request
# pool, no logger, no upstream nginx source linked in. The reference skeleton's
# copy of this script builds nginx's real src/core/ngx_string.c because its scan
# core calls ngx_unescape_uri(); ours calls nothing. Adding a build dependency
# to mirror the reference more closely would buy no honesty here -- the test
# already links the REAL decision TU, which is the property that matters.
#
# It therefore runs in well under a second with no nginx, no network and no
# root, and stays usable under a cross toolchain.
#
# SEEN RED -- the suite is gated by 21 per-group control mutations named in
# comments in test_scan.c; each was applied to src/strip_core.c and the named
# group observed failing. One more was applied while moving this suite under
# ci/ (2026-08-04), to prove the layer still links shipped code after the move:
#
#   * strip_json() made a verbatim copy -- `memcpy(dst, src, len); return len;`
#     at the top of strip_core.c:72
#       -> 6 checks FAIL. Restored; suite back to 144/144.
#
# A check that has never failed is not known to be a check. Re-run the
# mutations in test_scan.c after touching the core.
#
# Extend: add a CASE() to test_scan.c and one line to its main(). A new source
# file in src/ -> add it to the link line below.

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
BIN="$DIR/test_scan"
BIN2="$DIR/flush_copy_bound"

if [ "${1:-}" = "clean" ]; then
    # *.o/*.gcda/*.gcno too: this script produces them and leaving them behind
    # meant `clean` did not give you a clean tree -- switching $CC (say to
    # `gcc -m32`) then relinked stale objects from the previous toolchain.
    rm -f "$BIN" "$BIN2" "$DIR"/*.o "$DIR"/*.gcda "$DIR"/*.gcno
    echo "unit test binary removed"
    exit 0
fi

CC="${CC:-cc}"

# The full warning wall on our own code. A run that only passes with these
# loosened is hiding the exact class of defect they exist for.
CFLAGS=(-std=c11 -g -O1 -Wall -Wextra -Wshadow -Wstrict-prototypes -Werror)

if [ "${COVERAGE:-0}" = 1 ]; then
    CFLAGS+=(--coverage)
    LINK_EXTRA=(--coverage)
else
    LINK_EXTRA=()
fi

echo "==> Building $BIN with ${CC}"
# shellcheck disable=SC2086  # $CC may legitimately carry flags (e.g. "gcc -m32")
$CC "${CFLAGS[@]}" -I"$ROOT/src" -c "$DIR/test_scan.c" -o "$DIR/test_scan.o"
# shellcheck disable=SC2086
$CC "${CFLAGS[@]}" -I"$ROOT/src" -c "$ROOT/src/strip_core.c" \
    -o "$DIR/strip_core.o"
# shellcheck disable=SC2086
$CC "${LINK_EXTRA[@]}" -o "$BIN" "$DIR/test_scan.o" "$DIR/strip_core.o"

# flush_copy_bound: an ngx-independent extraction of the ngx_http_strip_flush()
# copy-loop arithmetic (see the file header for why it cannot link the real
# nginx module). Standalone, no strip_core dependency.
echo "==> Building $BIN2 with ${CC}"
# shellcheck disable=SC2086
$CC "${CFLAGS[@]}" "${LINK_EXTRA[@]}" -o "$BIN2" "$DIR/flush_copy_bound.c"

echo "==> Running"
"$BIN"

echo "==> Running $BIN2"
"$BIN2"

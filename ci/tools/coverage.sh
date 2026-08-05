#!/usr/bin/env bash
# Copyright (C) 2026 Thijs Eilander
# SPDX-License-Identifier: BSD-2-Clause
#
# ci/tools/coverage.sh -- line + branch coverage of the MODULE's own sources.
#
#   ci/tools/coverage.sh [version]
#
# Steps:
#   1. ci-build.sh nginx <version> coverage  -> a gcov-instrumented, STATICALLY
#      linked server binary (--add-module, not --add-dynamic-module -- see
#      ci-build.sh's coverage branch for why), in its own .build/nginx-<ver>
#      -coverage tree. .gcno files land under objs/addon/src/.
#   2. ci/tests/unit/run.sh with COVERAGE=1 -> the scan core's boundary cases,
#      instrumented and linked separately from the nginx build above.
#   3. prove ci/t/ against that server -> the request-path cases. nginx flushes
#      .gcda on a graceful exit, which is how the worker's arcs reach disk;
#      Test::Nginx stops the server between blocks, so this needs no special
#      handling, but a test that KILLs nginx contributes nothing.
#   4. gcovr over the module's own src/ only.
#
# Env:
#   COVERAGE_FAIL_UNDER   unset by default, and that is a decision, not an
#                         omission: repo policy is meaningful tests over a
#                         percentage. This card (Publish coverage as a report)
#                         adds no pass/fail gate -- if a future card wants a
#                         floor, it earns its own SEEN RED probe first.
#   COVERAGE_OUT          output directory (default .build/coverage).
#
# Exit: 0 on success, non-zero if a build/test step failed.
#
# WHY THE FILTER IS `src/` AND NOTHING ELSE: nginx's core objects are compiled
# by the same instrumented configure run, so an unfiltered gcovr reports ~1% of
# a 200k-line upstream tree and the module's own numbers vanish into it. The
# module's sources are the coverage target; upstream nginx is not ours to test.
#
# Extend: a new test layer belongs between steps 2 and 3, before gcovr runs --
# adding it after the report is generated silently contributes nothing.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# This repo's version pin lives directly in .github/versions.env (KEY=value
# lines), loaded into $GITHUB_ENV by .github/scripts/load-versions.sh inside
# CI. That script hard-requires $GITHUB_ENV and refuses to run locally by
# design (see its own header) -- this script needs the same pins outside a
# workflow step, so it reads NGINX_VERSION directly off the file instead of
# sourcing a loader that does not exist in this repo (the reference skeleton's
# coverage.sh sources a versions-env.sh helper; this repo never adopted that
# helper -- see .github/scripts/load-versions.sh instead).
VERSIONS_FILE="$ROOT/.github/versions.env"
if [ -z "${1:-}" ]; then
    [ -f "$VERSIONS_FILE" ] || { echo "ERROR: $VERSIONS_FILE not found" >&2; exit 1; }
    PINNED_VERSION="$(grep -m1 '^NGINX_VERSION=' "$VERSIONS_FILE" | cut -d= -f2-)"
    [ -n "$PINNED_VERSION" ] || { echo "ERROR: NGINX_VERSION not set in $VERSIONS_FILE" >&2; exit 1; }
    VERSION="$PINNED_VERSION"
else
    VERSION="$1"
fi
OUT="${COVERAGE_OUT:-$ROOT/.build/coverage}"

command -v gcovr >/dev/null 2>&1 || {
    echo "ERROR: gcovr not found. Install it: pipx install gcovr" >&2
    exit 2
}

echo "==> Building nginx $VERSION with gcov instrumentation (separate tree)"
bash ci/tools/ci-build.sh nginx "$VERSION" coverage

BUILD="$ROOT/.build/nginx-${VERSION}-coverage"
OBJDIR="$BUILD/objs/addon/src"

# The instrumented objects must actually exist before anything is run against
# them. Without this the suite runs, gcovr finds no .gcno, and the report reads
# "0.0%" -- which looks like a coverage finding rather than a broken build.
if ! compgen -G "$OBJDIR/*.gcno" >/dev/null; then
    echo "FAIL: no .gcno under $OBJDIR -- the build was NOT instrumented." >&2
    echo "      A coverage report from this tree would read 0% and mean nothing." >&2
    echo "      Suspect a cached non-coverage build tree restored into this mode." >&2
    exit 1
fi

# Stale arcs from a previous run would be merged into this one's counts.
find "$OBJDIR" -name '*.gcda' -delete

echo "==> Unit tests (instrumented)"
COVERAGE=1 bash ci/tests/unit/run.sh

echo "==> Test::Nginx suite against the instrumented (statically linked) server"
# No TEST_NGINX_LOAD_MODULES: coverage mode links the module straight into the
# server (--add-module, see ci-build.sh) instead of building a .so, so there is
# nothing to load_module -- matching asan.yml's identical static-link setup.
TEST_NGINX_BINARY="$BUILD/objs/nginx" \
TEST_NGINX_TIMEOUT="${TEST_NGINX_TIMEOUT:-20}" \
TEST_NGINX_PORT="${TEST_BASE_PORT:-18880}" \
TEST_NGINX_SERVROOT="$ROOT/ci/t/servroot" \
    prove ci/t/

echo "==> Report"
mkdir -p "$OUT"
GCOVR_ARGS=(
    --root "$ROOT"
    # Only the module's own sources. See the header for why an unfiltered run
    # is worse than useless here.
    --filter "$ROOT/src/"
    --branches
    --print-summary
    --html-details "$OUT/index.html"
    --txt "$OUT/summary.txt"
)
if [ -n "${COVERAGE_FAIL_UNDER:-}" ]; then
    GCOVR_ARGS+=(--fail-under-line "$COVERAGE_FAIL_UNDER")
fi

# Both object dirs: the nginx-linked module objects and the unit-test objects,
# which cover the same src/ TUs from a different driver. Merging them is the
# point -- a line reached only by the unit harness is still reached.
# --object-directory, not --gcov-object-directory: the latter was only added in
# gcovr 7.0 as an alias for this one, and an unknown option is a hard argparse
# failure, not a warning. The fork arm of this job runs on ubuntu-latest, whose
# gcovr is older than the self-hosted runner's -- so the newer spelling would
# fail exactly on the runner nobody watches. Both spellings work on 7.x.
gcovr "${GCOVR_ARGS[@]}" \
    --object-directory "$OBJDIR" \
    "$OBJDIR" "$ROOT/ci/tests/unit"

echo
echo "HTML report: $OUT/index.html"
echo "Summary:     $OUT/summary.txt"

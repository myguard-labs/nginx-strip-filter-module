#!/usr/bin/env bash
# Copyright (C) 2026 Thijs Eilander
# SPDX-License-Identifier: BSD-2-Clause
#
# Refresh every upstream pin in this repo. Called by .github/workflows/bump.yml
# on a schedule; also runnable locally to preview a bump before it lands.
#
#   ci/tools/bump-versions.sh [--dry-run]
#
# Two things move here:
#   - .github/versions.env    -- nginx mainline/stable and Angie versions plus
#                                 sha256, rewritten by compute-versions.sh
#   - GitHub Action sha pins  -- ci/tools/bump-actions.sh (sha + the tag comment
#                                 beside it, as one unit)
#
# Adapted from nginx-skeleton-module's bump-versions.sh. Deliberately dropped
# from the reference:
#   - pinned-linter bumping (bump-tools.sh) -- this repo's scanners
#     use one centrally pinned Python tool (Semgrep) plus distribution tools.
#     Semgrep moves through reviewed dependency PRs; there is no multi-tool
#     manifest for bump-tools.sh to maintain here.
#   - vendored nginx-tests submodule update -- this repo has no such submodule
#     (ci/t/ is native Test::Nginx, not a vendored copy).
#
# --dry-run reports what WOULD change without writing anything: versions.env is
# regenerated into a scratch copy and diffed.
#
# Exit status is 0 whether or not anything changed; the caller decides what to
# do with a dirty tree. Prints CHANGED=0|1 as its last line.
#
# GH_TOKEN is honoured (passed through to compute-versions.sh as GITHUB_TOKEN):
# the runners share an egress IP, so unauthenticated api.github.com calls are
# routinely rate-limited to 403s.

set -euo pipefail

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

cd "$(dirname "$0")/../.."

# compute-versions.sh reads GITHUB_TOKEN; bump.yml historically set GH_TOKEN.
export GITHUB_TOKEN="${GITHUB_TOKEN:-${GH_TOKEN:-}}"

CHANGED=0
VERSIONS_FILE=".github/versions.env"

# --- version + sha256 pin ---------------------------------------------------
if [ "$DRY_RUN" = 0 ]; then
    # Tolerate a missing file: compute-versions.sh creates it from scratch, so
    # bootstrapping (or regenerating after a delete) should report "changed"
    # rather than dying here under set -e.
    before="$(cat "$VERSIONS_FILE" 2>/dev/null || true)"
    bash .github/scripts/compute-versions.sh
    if [ "$before" != "$(cat "$VERSIONS_FILE")" ]; then
        echo "--- versions.env changed ---"
        git --no-pager diff -- "$VERSIONS_FILE" || true
        CHANGED=1
    else
        echo "versions.env already up to date"
    fi
else
    # Regenerate into a scratch copy so the working tree is untouched.
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT
    cp -a .github "$scratch/.github"
    ( cd "$scratch" && bash .github/scripts/compute-versions.sh >/dev/null )
    if diff -u "$VERSIONS_FILE" "$scratch/$VERSIONS_FILE"; then
        echo "(dry-run: versions.env already up to date)"
    else
        echo "(dry-run: versions.env would change as shown above)"
        CHANGED=1
    fi
fi

# --- GitHub Action pins ------------------------------------------------------
# Actions are already sha-pinned; what was missing was anything that MOVES the
# pin. An unmaintained sha is not "stable", it is a frozen copy of an action
# that stopped receiving its own security fixes. Major-line only -- see
# bump-actions.sh for why crossing a major unattended is not wanted.
if [ "$DRY_RUN" = 0 ]; then
    bash ci/tools/bump-actions.sh
else
    bash ci/tools/bump-actions.sh --dry-run
fi
if ! git diff --quiet -- .github/ 2>/dev/null; then
    CHANGED=1
fi

echo "CHANGED=$CHANGED"

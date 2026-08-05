#!/usr/bin/env bash
# sync-sha: bcc78e36aae3fc9ee3e4001c8b17d9485c155d703f2db982578279e4fef5cab7
# compute-versions.sh -- resolve the latest nginx mainline version + its
# sha256 and rewrite .github/versions.env in place. Used by bump.yml (weekly).
# Prints a short summary of what it resolved to stdout, which bump.yml quotes
# into the PR body.
#
# This module builds and tests against nginx mainline only -- no stable
# matrix, no angie flavor (unlike nginx-skeleton-module, which this script is
# adapted from: its mainline/stable/angie split is dropped here because no
# workflow in this repo builds those cells; see the versions.env header).
#
# Run locally to preview a bump:  bash .github/scripts/compute-versions.sh
# (it rewrites versions.env; `git diff` to review, `git checkout` to discard).
#
# Requires: curl, sha256sum.
set -euo pipefail

VERSIONS_FILE=".github/versions.env"
FV=".github/scripts/fetch-verify.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# sha256 of a URL (download to scratch, hash). Fails the job on download error.
sha_of_url() {
  local url="$1" out="$tmp/dl.$RANDOM" sha
  # In "-" mode fetch-verify.sh sends progress text to stderr, so stdout is
  # exactly one "SHA  OUTFILE" line. Read the first field directly rather than
  # picking the last line out of mixed output -- an extra stdout line there
  # would otherwise be captured as a digest with no error.
  read -r sha _ < <(bash "$FV" "$url" - "$out")
  # Validate the shape rather than trusting position: a non-digest silently
  # written into versions.env would pin every future build to a value that
  # can never match.
  if ! printf '%s' "$sha" | grep -qE '^[0-9a-f]{64}$'; then
    echo "::error::expected a sha256 from $FV for $url, got: ${sha:-<empty>}" >&2
    return 1
  fi
  printf '%s' "$sha"
}

echo "resolving nginx mainline from nginx.org..."
dl_html="$(curl -fsSL --retry 3 --retry-delay 2 --connect-timeout 30 --max-time 300 \
  https://nginx.org/en/download.html)"
# nginx numbers mainline with an odd minor. Take the highest rather than
# trusting page order.
NGX_MAINLINE="$(printf '%s' "$dl_html" | grep -oE 'nginx-1\.[0-9]+\.[0-9]+' \
  | awk -F. '$2%2==1' | sort -uV | tail -1 | sed 's/nginx-//')"
[ -n "$NGX_MAINLINE" ] || { echo "::error::failed to resolve nginx mainline version" >&2; exit 1; }

echo "hashing archive..."
NGX_MAINLINE_SHA="$(sha_of_url "https://nginx.org/download/nginx-${NGX_MAINLINE}.tar.gz")"

cat > "$VERSIONS_FILE" <<EOF
# Central version + sha256 pin for all CI workflows.
#
# SINGLE SOURCE OF TRUTH. Every workflow loads this file into \$GITHUB_ENV as its
# first step (via .github/scripts/load-versions.sh); the weekly bump.yml job
# rewrites it and opens a PR. The tarball is pinned by version string (release
# archives are immutable) AND verified against the sha256 recorded here, so a
# compromised or changed upstream archive fails the build instead of being
# compiled.
#
# Version and digest live on adjacent lines on purpose: they are bumped by one
# writer (compute-versions.sh) in one file, so a version can no longer move
# while its digest stays behind.
#
# Regenerate with .github/scripts/compute-versions.sh (bump.yml runs it weekly).
# Keep KEY=value, no spaces, no quotes -- this file is both \`source\`d by
# ci/tools/ci-build.sh and \`cat\`d into \$GITHUB_ENV.
#
# This module builds and tests against ONE nginx line only (no stable-vs-
# mainline matrix, no angie flavor) -- every existing workflow already hardcoded
# the same single NGINX_VERSION, so there is nothing else to pin here. See
# ci/tools/ci-build.sh's \`angie\` case for a build-time flavor path that exists
# in the script but is not exercised by any workflow; adding an angie/stable
# matrix cell is out of scope for this card (no target workflow builds one).

# nginx version used by every job (build-test, asan, valgrind, codeql,
# fuzzing, security-scanners, ci-deep).
NGINX_VERSION=${NGX_MAINLINE}
NGINX_VERSION_SHA256=${NGX_MAINLINE_SHA}
EOF

echo "----- resolved -----"
echo "nginx mainline: ${NGX_MAINLINE}"

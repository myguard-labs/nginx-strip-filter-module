#!/usr/bin/env bash
# sync-sha: e7748c46d3bb609fb94a4a0326b51f484085856e10c94ff87ff28b98e4cd52b7
# compute-versions.sh -- resolve the latest upstream versions + their sha256
# and rewrite .github/versions.env in place. Used by bump.yml (weekly).
#
# Resolves nginx mainline and stable from nginx.org, plus Angie's latest
# release from GitHub. The Angie digest comes from download.angie.software,
# matching the archive ci-deep actually builds rather than GitHub's different
# tag archive.
#
# Run locally to preview a bump: bash .github/scripts/compute-versions.sh
# (it rewrites versions.env; `git diff` to review, `git restore` to discard).
#
# Requires: curl, jq, sha256sum. GITHUB_TOKEN is honoured for API rate limits.
set -euo pipefail

VERSIONS_FILE=".github/versions.env"
FV=".github/scripts/fetch-verify.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

api() {
    local url="$1"
    local -a opts=(-fsSL --retry 3 --retry-delay 2 --connect-timeout 30 --max-time 300)
    if [ -n "${GITHUB_TOKEN:-}" ]; then
        curl "${opts[@]}" -H "Authorization: Bearer $GITHUB_TOKEN" "$url"
    else
        curl "${opts[@]}" "$url"
    fi
}

sha_of_url() {
    local url="$1" out="$tmp/dl.$RANDOM" sha
    read -r sha _ < <(bash "$FV" "$url" - "$out")
    if ! printf '%s' "$sha" | grep -qE '^[0-9a-f]{64}$'; then
        echo "::error::expected a sha256 from $FV for $url, got: ${sha:-<empty>}" >&2
        return 1
    fi
    printf '%s' "$sha"
}

echo "resolving nginx versions from nginx.org..."
dl_html="$(curl -fsSL --retry 3 --retry-delay 2 --connect-timeout 30 --max-time 300 \
    https://nginx.org/en/download.html)"
NGX_MAINLINE="$(printf '%s' "$dl_html" | grep -oE 'nginx-1\.[0-9]+\.[0-9]+' |
    awk -F. '$2%2==1' | sort -uV | tail -1 | sed 's/nginx-//')"
NGX_STABLE="$(printf '%s' "$dl_html" | grep -oE 'nginx-1\.[0-9]+\.[0-9]+' |
    awk -F. '$2%2==0' | sort -uV | tail -1 | sed 's/nginx-//')"
if [ -z "$NGX_MAINLINE" ] || [ -z "$NGX_STABLE" ]; then
    echo "::error::failed to resolve nginx versions" >&2
    exit 1
fi

echo "resolving Angie latest release..."
ANGIE_TAG="$(api 'https://api.github.com/repos/webserver-llc/angie/releases/latest' | jq -r '.tag_name')"
if [ -z "$ANGIE_TAG" ] || [ "$ANGIE_TAG" = "null" ]; then
    echo "::error::failed to resolve Angie" >&2
    exit 1
fi
ANGIE="${ANGIE_TAG#Angie-}"
if ! printf '%s' "$ANGIE" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "::error::unexpected Angie tag format: $ANGIE_TAG" >&2
    exit 1
fi

echo "hashing archives..."
NGX_MAINLINE_SHA="$(sha_of_url "https://nginx.org/download/nginx-${NGX_MAINLINE}.tar.gz")"
NGX_STABLE_SHA="$(sha_of_url "https://nginx.org/download/nginx-${NGX_STABLE}.tar.gz")"
ANGIE_SHA="$(sha_of_url "https://download.angie.software/files/angie-${ANGIE}.tar.gz")"

cat >"$VERSIONS_FILE" <<EOF
# Central version + sha256 pins for all CI workflows.
#
# SINGLE SOURCE OF TRUTH. Every workflow that consumes these pins loads this
# file into \$GITHUB_ENV (via .github/scripts/load-versions.sh); bump.yml
# rewrites it and opens a PR. Tarballs are pinned by version string (release
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

# nginx mainline (odd minor) -- the default build everywhere; also the
# ci-deep "mainline" matrix cell.
NGINX_MAINLINE=${NGX_MAINLINE}
NGINX_MAINLINE_SHA256=${NGX_MAINLINE_SHA}

# nginx stable (even minor) -- ci-deep "stable" matrix cell only.
NGINX_STABLE=${NGX_STABLE}
NGINX_STABLE_SHA256=${NGX_STABLE_SHA}

# nginx version used by every single-version job (build-test, asan, valgrind,
# codeql, security-scanners, ci-deep memcheck, and Windows). Tracks mainline.
NGINX_VERSION=${NGX_MAINLINE}
NGINX_VERSION_SHA256=${NGX_MAINLINE_SHA}

# Angie (webserver-llc) -- ci-deep "angie" matrix cell. Pinned to the tarball
# from download.angie.software, NOT the GitHub tag archive: different bytes,
# so this digest is not interchangeable with a github.com/webserver-llc one.
# ANGIE_VERSION is the bare version; the upstream release tag is "Angie-<ver>".
ANGIE_VERSION=${ANGIE}
ANGIE_SHA256=${ANGIE_SHA}

# Scanner version shared by local and hosted CI. This is intentionally updated
# by dependency PRs, not inferred from PyPI during an nginx/Angie bump.
SEMGREP_VERSION=1.173.0
EOF

echo "----- resolved -----"
echo "nginx mainline: ${NGX_MAINLINE}"
echo "nginx stable:   ${NGX_STABLE}"
echo "angie:          ${ANGIE}"

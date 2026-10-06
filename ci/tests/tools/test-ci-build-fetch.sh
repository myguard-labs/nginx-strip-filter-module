#!/usr/bin/env bash
# Regression tests for ci/tools/ci-build.sh's source fetch: every version that
# .github/versions.env pins (nginx current/mainline/stable, Angie) is
# sha256-verified, an unpinned one-off build still works, a caller env that
# drifted from the pin is refused, and both download paths retry transient
# TLS/connection errors. curl, make and the tarball's ./configure are stubs, so
# no network or compiler is used.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mod="$work/module"
mkdir -p "$mod/ci/tools" "$mod/.github/scripts" "$work/bin" "$work/mirror"
cp "$ROOT/ci/tools/ci-build.sh" "$mod/ci/tools/"
cp "$ROOT/.github/scripts/fetch-verify.sh" "$mod/.github/scripts/"

# One fake source tarball per flavor/version. ./configure is a no-op.
make_tarball() {
    local name="$1" src="$work/src/$1"
    mkdir -p "$src"
    printf '#!/bin/sh\nexit 0\n' >"$src/configure"
    chmod +x "$src/configure"
    tar -czf "$work/mirror/$name.tar.gz" -C "$work/src" "$name"
    sha256sum "$work/mirror/$name.tar.gz" | cut -d' ' -f1
}
sha_cur="$(make_tarball nginx-1.31.3)"
sha_stable="$(make_tarball nginx-1.30.4)"
sha_angie="$(make_tarball angie-1.12.1)"
make_tarball nginx-1.29.0 >/dev/null

cat >"$mod/.github/versions.env" <<EOF
NGINX_MAINLINE=1.31.3
NGINX_MAINLINE_SHA256=$sha_cur
NGINX_STABLE=1.30.4
NGINX_STABLE_SHA256=$sha_stable
NGINX_VERSION=1.31.3
NGINX_VERSION_SHA256=$sha_cur
ANGIE_VERSION=1.12.1
ANGIE_SHA256=$sha_angie
EOF

# curl stub: serve the mirror by URL basename and log argv for flag checks.
cat >"$work/bin/curl" <<EOF
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "\$*" >>"$work/curl.log"
out="" url=""
while [ \$# -gt 0 ]; do
    case "\$1" in
        -o) out="\$2"; shift 2 ;;
        -*) shift ;;
        *) url="\$1"; shift ;;
    esac
done
cp "$work/mirror/\${url##*/}" "\$out"
EOF
printf '#!/bin/sh\nexit 0\n' >"$work/bin/make"
chmod +x "$work/bin/curl" "$work/bin/make"

run() {
    (cd "$mod" && env -u NGINX_VERSION -u ANGIE_VERSION \
        PATH="$work/bin:$PATH" NO_CACHE=1 BUILD_ROOT="$work/build" "$@" \
        bash ci/tools/ci-build.sh "${args[@]}" 2>&1)
}
fresh() { rm -rf "${work:?}/build" "${work:?}/curl.log"; }

# Pinned versions are verified, including the two that used to be unverified.
for case_ in "nginx 1.31.3" "nginx 1.30.4" "angie 1.12.1"; do
    fresh
    read -r -a args <<<"$case_"
    out="$(run)"
    printf '%s\n' "$out" | grep -q "^sha256 verified: .*/${args[0]}-${args[1]}.tar.gz$" \
        || { echo "FAIL: $case_ not verified"; printf '%s\n' "$out"; exit 1; }
    grep -q -- '--retry-all-errors' "$work/curl.log"
done

# A tampered pinned tarball is rejected, not built.
fresh
cp "$work/mirror/angie-1.12.1.tar.gz" "$work/angie.orig"
printf 'tampered' >>"$work/mirror/angie-1.12.1.tar.gz"
args=(angie 1.12.1)
if out="$(run)"; then
    echo "FAIL: tampered angie tarball was accepted"; printf '%s\n' "$out"; exit 1
fi
printf '%s\n' "$out" | grep -q 'sha256 MISMATCH'
cp "$work/angie.orig" "$work/mirror/angie-1.12.1.tar.gz"

# An unpinned one-off version builds unverified, with retries.
fresh
args=(nginx 1.29.0)
out="$(run)"
if printf '%s\n' "$out" | grep -q 'sha256 verified'; then
    echo "FAIL: unpinned version claimed verification"; exit 1
fi
printf '%s\n' "$out" | grep -q '^binary=.*/nginx-1.29.0/objs/nginx$'
grep -q -- '--retry-all-errors' "$work/curl.log"

# A caller env that drifted from versions.env is refused, for both flavors.
fresh
args=(nginx 1.29.0)
if out="$(run NGINX_VERSION=1.29.0)"; then
    echo "FAIL: drifted NGINX_VERSION accepted"; exit 1
fi
printf '%s\n' "$out" | grep -q "caller's NGINX_VERSION (1.29.0) does not match .github/versions.env's pin (1.31.3)"
fresh
args=(angie 1.11.0)
if out="$(run ANGIE_VERSION=1.11.0)"; then
    echo "FAIL: drifted ANGIE_VERSION accepted"; exit 1
fi
printf '%s\n' "$out" | grep -q "caller's ANGIE_VERSION (1.11.0) does not match .github/versions.env's pin (1.12.1)"
[ ! -e "$work/curl.log" ]

# A pinned version whose digest line is missing is refused, not built unverified.
fresh
sed -i '/^ANGIE_SHA256=/d' "$mod/.github/versions.env"
args=(angie 1.12.1)
if out="$(run)"; then
    echo "FAIL: pinned angie without a digest was accepted"; exit 1
fi
printf '%s\n' "$out" | grep -q 'ANGIE_SHA256 is missing from .github/versions.env'

echo "test-ci-build-fetch: pass"

#!/usr/bin/env bash

set -euo pipefail

FLAVOR="${1:-nginx}"
VERSION="${2:-1.31.1}"
MODE="${3:-debug}"
ROOT="${BUILD_ROOT:-$PWD/.build}"
MODULE_DIR="$PWD"

# coverage gets its own tree, never shared with debug/asan/module: gcov
# instruments objects in place, and a tree also used by another mode would
# either fail to configure (stale objs/Makefile from a different --with-cc-opt)
# or worse, silently mix instrumented and non-instrumented objects into one
# report. Every other mode keeps the existing unsuffixed "$FLAVOR-$VERSION"
# name -- ci-deep.yml and build-test.yml already key their tarball cache and
# objs/ lookups off that exact string, and changing it for modes nothing here
# asked to change would be its own regression.
if [ "$MODE" = "coverage" ]; then
    DIR_SUFFIX="-coverage"
else
    DIR_SUFFIX=""
fi

case "$FLAVOR" in
    nginx)
        URL="https://nginx.org/download/nginx-${VERSION}.tar.gz"
        DIR="nginx-${VERSION}${DIR_SUFFIX}"
        BINARY="nginx"
        ;;
    angie)
        URL="https://download.angie.software/files/angie-${VERSION}.tar.gz"
        DIR="angie-${VERSION}${DIR_SUFFIX}"
        BINARY="angie"
        ;;
    *)
        echo "unsupported flavor: $FLAVOR" >&2
        exit 2
        ;;
esac

mkdir -p "$ROOT"
# .github/versions.env is the single source of truth for the nginx tarball's
# sha256. Only nginx carries a pin there today (this repo builds one flavor,
# one version -- see the versions.env header); an angie build, or a nginx
# VERSION that simply isn't the pinned one (a deliberate one-off/local build),
# falls back to an unverified download exactly as before.
#
# The one case that must NOT fall back quietly: a caller running under a
# workflow (NGINX_VERSION env set) whose VERSION matches that env, but the
# env itself has drifted from versions.env's pinned version -- e.g. a
# workflow still hardcoding an older "1.31.1" after versions.env was bumped
# to "1.31.3". Silently treating that as "not the pinned version, skip the
# check" would turn a bump into a permanent unverified build for every
# workflow that forgot to move its own copy. Fail loud instead: the pin
# exists so a version move never ships unverified.
SHA256="-"
if [ "$FLAVOR" = "nginx" ] && [ -f "$MODULE_DIR/.github/versions.env" ]; then
    pinned_version="$(grep -m1 '^NGINX_VERSION=' "$MODULE_DIR/.github/versions.env" | cut -d= -f2-)"
    pinned_sha="$(grep -m1 '^NGINX_VERSION_SHA256=' "$MODULE_DIR/.github/versions.env" | cut -d= -f2-)"
    if [ "$VERSION" = "$pinned_version" ]; then
        SHA256="$pinned_sha"
    elif [ -n "${NGINX_VERSION:-}" ] && [ "$VERSION" = "$NGINX_VERSION" ]; then
        echo "::error::caller's NGINX_VERSION ($NGINX_VERSION) does not match .github/versions.env's pin ($pinned_version) -- refusing to build nginx unverified. Update the caller's NGINX_VERSION to match versions.env." >&2
        exit 1
    fi
    # else: VERSION differs from both the pin and any caller env -- a
    # deliberate one-off build (e.g. ci-deep's ad-hoc versions), unverified
    # exactly as before this change.
fi
if [ "$SHA256" != "-" ] && [ -f "$MODULE_DIR/.github/scripts/fetch-verify.sh" ]; then
    bash "$MODULE_DIR/.github/scripts/fetch-verify.sh" "$URL" "$SHA256" "$ROOT/${DIR}.tar.gz"
elif [ ! -f "$ROOT/${DIR}.tar.gz" ]; then
    curl -fsSL "$URL" -o "$ROOT/${DIR}.tar.gz"
fi
if [ ! -d "$ROOT/$DIR" ]; then
    # The archive's own top-level dir is unsuffixed ("nginx-<ver>/"), so a
    # suffixed DIR (coverage's separate tree) needs an explicit rename after
    # extracting -- tar has no "extract as a different top-level name" flag,
    # and extracting straight into "$ROOT" would land in the plain-mode dir
    # instead of this one, defeating the whole point of a separate tree.
    UNSUFFIXED_DIR="${FLAVOR}-${VERSION}"
    if [ "$DIR" != "$UNSUFFIXED_DIR" ]; then
        rm -rf "$ROOT/${UNSUFFIXED_DIR:?}.extract-tmp"
        mkdir -p "$ROOT/${UNSUFFIXED_DIR}.extract-tmp"
        tar -xzf "$ROOT/${DIR}.tar.gz" -C "$ROOT/${UNSUFFIXED_DIR}.extract-tmp"
        mv "$ROOT/${UNSUFFIXED_DIR}.extract-tmp/${UNSUFFIXED_DIR}" "$ROOT/$DIR"
        rm -rf "$ROOT/${UNSUFFIXED_DIR}.extract-tmp"
    else
        tar -xzf "$ROOT/${DIR}.tar.gz" -C "$ROOT"
    fi
fi

CC_OPT="-DNGX_DEBUG_PALLOC=1 -g3 -O0 -fno-omit-frame-pointer -funwind-tables"
LD_OPT=""
ADD_MODULE="--add-dynamic-module=$MODULE_DIR"
if [ "$MODE" = "asan" ]; then
    SAN="-fsanitize=address,undefined -fno-sanitize=nonnull-attribute -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g3 -O1"
    if "${CC:-cc}" --version 2>/dev/null | grep -qi clang; then
        SAN="-fsanitize=address,undefined -fno-sanitize=function,nonnull-attribute,pointer-overflow -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g3 -O1"
    fi
    CC_OPT="$SAN"
    LD_OPT="$SAN"
    ADD_MODULE="--add-module=$MODULE_DIR"
elif [ "$MODE" = "coverage" ]; then
    # --add-module (static), not --add-dynamic-module: a .so loaded via
    # load_module never runs its exit handler in a way that reliably flushes
    # gcov counters across every nginx build/version combination, and this
    # tree's only purpose is the coverage number -- linking it straight into
    # the server removes that variable instead of debugging it per version.
    CC_OPT="--coverage -g -O0"
    LD_OPT="--coverage"
    ADD_MODULE="--add-module=$MODULE_DIR"
fi

WITH_CC=""
if [ -n "${CC:-}" ]; then
    WITH_CC="--with-cc=$CC"
fi

cd "$ROOT/$DIR"
# shellcheck disable=SC2086
./configure \
    --with-compat \
    --with-debug \
    --with-threads \
    --with-http_realip_module \
    $WITH_CC \
    --with-cc-opt="$CC_OPT" \
    --with-ld-opt="$LD_OPT" \
    "$ADD_MODULE"

if [ "$MODE" != "asan" ] && [ "$MODE" != "coverage" ]; then
    make -j"$(nproc)" modules
fi

if [ "$MODE" != "module" ]; then
    make -j"$(nproc)"
    printf 'binary=%s\n' "$ROOT/$DIR/objs/$BINARY"
fi

if [ "$MODE" != "asan" ] && [ "$MODE" != "coverage" ]; then
    printf 'module=%s\n' "$ROOT/$DIR/objs/ngx_http_strip_filter_module.so"
fi

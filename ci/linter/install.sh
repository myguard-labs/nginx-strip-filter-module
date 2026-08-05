#!/usr/bin/env bash
# install.sh -- single installer for every linter run-all.sh drives.
#
# ONE installer, ONE entry point (run-all.sh) is the card's actual requirement
# (card 32, "Port the linter and installer"). This repo never grew a
# ci/linter/ tree of its own and the referenced skeleton has none either --
# there is nothing to import. This wraps the linters the repo's OWN CI
# workflows already gate on (.github/workflows/security-scanners.yml,
# build-test.yml), so local and CI stay the same tools at the same versions.
#
# Installs via apt (system tools) and pipx (semgrep, isolated the same way
# security-scanners.yml does it -- PEP 668 blocks a bare pip3 install into
# the system Python). Never touches global shell profiles; never curl|sh.
#
# Idempotent: safe to re-run. Exits 0 on success, non-zero if apt/pipx fail.
set -euo pipefail

need_apt=()
command -v flawfinder >/dev/null 2>&1 || need_apt+=(flawfinder)
command -v clang-tidy >/dev/null 2>&1 || need_apt+=(clang-tidy)
command -v shellcheck >/dev/null 2>&1 || need_apt+=(shellcheck)
command -v cppcheck >/dev/null 2>&1 || need_apt+=(cppcheck)

if [ "${#need_apt[@]}" -gt 0 ]; then
  echo "install.sh: installing via apt: ${need_apt[*]}" >&2
  sudo apt-get update
  sudo apt-get install -y "${need_apt[@]}"
fi

if ! command -v semgrep >/dev/null 2>&1; then
  echo "install.sh: installing semgrep via pipx" >&2
  if command -v pipx >/dev/null 2>&1; then
    pipx install --quiet semgrep
  else
    pip3 install --quiet --user --break-system-packages semgrep
  fi
fi

# actionlint has no apt package; same version-pinned, checksum-verified
# fetch build-test.yml's "Lint workflows" step uses, so local and CI run the
# identical binary.
if ! command -v actionlint >/dev/null 2>&1; then
  echo "install.sh: installing actionlint" >&2
  version=1.7.7
  base="https://github.com/rhysd/actionlint/releases/download/v${version}"
  tarball="actionlint_${version}_linux_amd64.tar.gz"
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  curl -fsSL -o "$tmp/actionlint.tgz" "${base}/${tarball}"
  curl -fsSL -o "$tmp/actionlint.checksums" "${base}/actionlint_${version}_checksums.txt"
  ( cd "$tmp" && grep " ${tarball}\$" actionlint.checksums \
      | sed 's#'"${tarball}"'#actionlint.tgz#' | sha256sum -c - )
  tar -xzf "$tmp/actionlint.tgz" -C "$tmp" actionlint
  install -m 0755 "$tmp/actionlint" "${HOME}/.local/bin/actionlint"
  echo "install.sh: installed actionlint to ${HOME}/.local/bin -- ensure that dir is on PATH" >&2
fi

echo "install.sh: done"

#!/usr/bin/env bash
# run-all.sh -- single entry point for every linter this repo gates on.
#
# Card 32 ("Port the linter and installer"): one installer (install.sh) + one
# run-all.sh, wired into CI on a hosted runner. No ci/linter/ tree exists in
# this repo or in the referenced skeleton to port verbatim, so this wraps the
# linters .github/workflows/security-scanners.yml and build-test.yml already
# gate PRs on, at the SAME thresholds -- mirrored, not invented:
#
#   flawfinder      fails at level >=4        (security-scanners.yml "flawfinder")
#   semgrep         fails at severity WARNING (security-scanners.yml "semgrep")
#   the SC-lint     fails at severity warning (mirrors .pre-commit-config.yaml floor)
#   actionlint      default severity          (build-test.yml "Lint workflows")
#
# Exit codes (the card's literal DONE criterion):
#   0 -- clean run, no findings
#   1 -- at least one real lint finding at the gated severity
#   2 -- a required tool is missing, OR (see clang-tidy below) a required
#        prerequisite tree is missing. Never silently skips -- a missing tool
#        must never read as "passed".
#
# clang-tidy is a DELIBERATE, DOCUMENTED relaxation: it needs a configured
# nginx source tree (ngx_auto_config.h etc.), which only exists after a real
# build (see security-scanners.yml's "Configure nginx src tree" step). This
# entry point does not perform that build -- doing so here would silently
# duplicate build-test.yml's own build and double the local runtime for a
# check that is already correctly gated in CI. Instead: if $NGINX_SRC_TREE is
# set (a caller who already built one, e.g. CI after the security-scanners
# build step) clang-tidy runs and gates for real; if it is unset, clang-tidy
# is SKIPPED but that skip is exit 2, never exit 0 -- so a bare local run
# truthfully reports "prerequisite missing", not "passed". This mirrors
# .pre-commit-config.yaml's identical clang-tidy relaxation and reasoning.
#
# clang-format is NOT included: no .clang-format exists in this repo (same
# relaxation .pre-commit-config.yaml documents) -- there is no agreed style to
# enforce, and adding one now would reformat unrelated code in this diff.
#
# cppcheck is NOT included here even though build-test.yml gates on it: that
# gate already runs as part of the "Validation" job entry point
# (ci/tools/ci-build.sh's siblings), is C-source-specific with its own
# suppress list tuned there, and duplicating it here would be a second source
# of truth for the same thresholds. run-all.sh covers the linters that do NOT
# already have a single existing repo entry point of their own.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

missing=()
command -v flawfinder >/dev/null 2>&1 || missing+=(flawfinder)
command -v semgrep >/dev/null 2>&1 || missing+=(semgrep)
command -v shellcheck >/dev/null 2>&1 || missing+=(shellcheck)
command -v actionlint >/dev/null 2>&1 || missing+=(actionlint)
command -v ruff >/dev/null 2>&1 || missing+=(ruff)

if [ "${#missing[@]}" -gt 0 ]; then
  echo "run-all.sh: missing required tool(s): ${missing[*]}" >&2
  echo "run-all.sh: run ci/linter/install.sh first" >&2
  exit 2
fi

status=0

echo "== flawfinder (gate on >=4) =="
if ! flawfinder --minlevel=1 src/*.c 2>&1 | tee /tmp/run-all-flawfinder.log; then
  status=1
fi
if flawfinder --minlevel=4 --quiet src/*.c 2>/dev/null | grep -Eq 'Hits = [1-9]'; then
  echo "run-all.sh: flawfinder high-severity (>=4) finding" >&2
  status=1
fi

echo "== semgrep (gate on >=WARNING) =="
# semgrep scan exit codes: 0 clean, 1 findings (what --error asks for), 2
# fatal/tool error, 3 invalid target. Only 1 is a real lint finding -- a 2
# means semgrep itself failed (bad config, missing ruleset fetch, etc) and
# must not silently fold into the same "status=1" bucket as a real finding.
semgrep_rc=0
semgrep scan --config p/c --config p/security-audit \
  --severity=WARNING --severity=ERROR --error \
  src/*.c || semgrep_rc=$?
if [ "$semgrep_rc" -eq 1 ]; then
  echo "run-all.sh: semgrep finding at >=WARNING" >&2
  status=1
elif [ "$semgrep_rc" -ne 0 ]; then
  echo "run-all.sh: semgrep tool error (exit $semgrep_rc), not a lint finding" >&2
  exit 2
fi

echo "== shellcheck (gate on >=warning) =="
# git ls-files, not find: find only ever saw ci/ and .github/, so a shell
# script at the repo root (.githooks/pre-commit) or with no .sh/.bash
# extension was never read -- the gate reported clean for files it never
# checked. Union of *.sh/*.bash (extension-based) with a shebang grep over
# every OTHER tracked file (extensionless scripts) catches both shapes.
mapfile -t sh_files < <(
  {
    git ls-files -z '*.sh' '*.bash' | tr '\0' '\n'
    git grep -lIz -E '^#!.*\b(ba)?sh\b' -- ':!*.sh' ':!*.bash' \
      | tr '\0' '\n'
  } | sort -u
)
if [ "${#sh_files[@]}" -gt 0 ]; then
  if ! shellcheck -S warning "${sh_files[@]}"; then
    echo "run-all.sh: shellcheck finding at >=warning" >&2
    status=1
  fi
else
  echo "run-all.sh: no shell scripts found -- unexpected, treating as a gate failure" >&2
  status=1
fi

echo "== actionlint (workflow syntax) =="
if ! env SHELLCHECK_OPTS=-Swarning actionlint -ignore 'label ".+" is unknown' .github/workflows/*.yml; then
  echo "run-all.sh: actionlint finding" >&2
  status=1
fi

echo "== ci-ports (runtime jobs declare TEST_BASE_PORT) =="
if ! bash ci/linter/lint-ci-ports.sh; then
  echo "run-all.sh: ci-ports finding" >&2
  status=1
fi

echo "== docs-drift (README '## CI' table <-> .github/workflows/) =="
if ! bash ci/linter/lint-docs-drift.sh; then
  echo "run-all.sh: docs-drift finding" >&2
  status=1
fi

echo "== flush-bound (ngx_http_strip_flush() copy loop stays source-bounded) =="
if ! python3 ci/tools/check-flush-bound.py; then
  echo "run-all.sh: flush-bound finding" >&2
  status=1
fi

echo "== ruff (Python lint, gate on default rule set) =="
mapfile -t py_files < <(git ls-files -z '*.py' | tr '\0' '\n' | sort)
if [ "${#py_files[@]}" -gt 0 ]; then
  if ! ruff check "${py_files[@]}"; then
    echo "run-all.sh: ruff finding" >&2
    status=1
  fi
else
  echo "run-all.sh: no Python files found -- unexpected, treating as a gate failure" >&2
  status=1
fi

echo "== clang-tidy (cert-*, security.*) =="
prereq_missing=0
if [ -z "${NGINX_SRC_TREE:-}" ]; then
  echo "run-all.sh: NGINX_SRC_TREE not set -- clang-tidy needs a configured nginx" >&2
  echo "run-all.sh: source tree (ngx_auto_config.h etc), which only exists after" >&2
  echo "run-all.sh: a real build. This is a documented target-specific relaxation" >&2
  echo "run-all.sh: (see .pre-commit-config.yaml and the header of this script)." >&2
  echo "run-all.sh: skipping is exit 2, never exit 0 -- run CI, or export" >&2
  echo "run-all.sh: NGINX_SRC_TREE to a configured tree to run this check locally." >&2
  prereq_missing=1
elif ! command -v clang-tidy >/dev/null 2>&1; then
  echo "run-all.sh: NGINX_SRC_TREE is set but clang-tidy is not installed" >&2
  prereq_missing=1
else
  N="$NGINX_SRC_TREE"
  includes=(
    "-I$N/src/core" "-I$N/src/event" "-I$N/src/event/modules"
    "-I$N/src/event/quic" "-I$N/src/os/unix" "-I$N/objs"
    "-I$N/src/http" "-I$N/src/http/modules" "-I$N/src/http/v2"
    "-I$(pwd)" "-I/usr/include"
  )
  for f in src/*.c; do
    if ! clang-tidy "$f" \
      --checks='-*,cert-*,clang-analyzer-security.*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling' \
      --warnings-as-errors='*' -- "${includes[@]}"; then
      echo "run-all.sh: clang-tidy finding in $f" >&2
      status=1
    fi
  done
fi

if [ "$status" -eq 0 ]; then
  if [ "$prereq_missing" -eq 1 ]; then
    echo "run-all.sh: no findings, but clang-tidy prerequisite missing" >&2
    exit 2
  fi
  echo "run-all.sh: clean"
else
  echo "run-all.sh: findings present" >&2
fi
exit "$status"

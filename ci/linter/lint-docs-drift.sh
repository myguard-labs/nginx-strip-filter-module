#!/usr/bin/env bash
# Copyright (C) 2026 Thijs Eilander
# SPDX-License-Identifier: BSD-2-Clause
#
# ci/linter/lint-docs-drift.sh -- README.md's `## CI` table and
# .github/workflows/ must describe the same pipeline, in both directions:
#
#   * a workflow added with no README row is a gate nobody knows exists, so
#     nobody notices when a later refactor deletes it;
#   * a README row naming a workflow that does not exist is a badge that
#     404s and a documented guarantee this repo does not actually have.
#
# Exit codes (matches the run-all.sh contract):
#   0 -- every workflow has a row, every row names a workflow
#   1 -- drift found in either direction
#   2 -- prerequisite missing (python3/PyYAML), preventing verification
#
# EXCLUDED from the "every row" direction: none. Every workflow file gets a
# row, including bump.yml and ci.yml -- both are real, user-facing CI surface
# (bump.yml auto-commits version bumps; ci.yml is the orchestrator every PR
# actually runs) and the README's own table already documents their triggers
# ("weekly + dispatch", "PR (the only pull_request entry point)"). If a future
# workflow is added that should NOT be documented (e.g. a purely internal
# reusable workflow never meant to be user-facing), add its filename to
# EXCLUDE_FROM_TABLE below and say why in a comment next to it -- do not
# silently special-case it in the Python logic.
#
# Usage: ci/linter/lint-docs-drift.sh

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

if ! command -v python3 >/dev/null 2>&1; then
  echo "lint-docs-drift: python3 not found; cannot verify" >&2
  exit 2
fi

python3 - "$@" <<'PYEOF'
import pathlib
import re
import sys

ROOT = pathlib.Path(".")
WORKFLOWS_DIR = ROOT / ".github" / "workflows"
README = ROOT / "README.md"

# See the header comment above for why this is empty by default.
EXCLUDE_FROM_TABLE: set[str] = set()

if not WORKFLOWS_DIR.is_dir():
    print("lint-docs-drift: no .github/workflows/ directory", file=sys.stderr)
    sys.exit(2)

if not README.is_file():
    print("lint-docs-drift: no README.md", file=sys.stderr)
    sys.exit(2)

names = {p.name for p in WORKFLOWS_DIR.glob("*.yml")} | {
    p.name for p in WORKFLOWS_DIR.glob("*.yaml")
}
if not names:
    print("lint-docs-drift: no workflow files found -- unexpected", file=sys.stderr)
    sys.exit(2)

text = README.read_text(encoding="utf-8")

m = re.search(r"^## CI\s*$(.*?)(^## |\Z)", text, re.MULTILINE | re.DOTALL)
if not m:
    print("lint-docs-drift: README.md has no '## CI' section", file=sys.stderr)
    sys.exit(2)
ci_section = m.group(1)

# Table rows: | `name.yml` | ... -- the first backtick-quoted cell of each
# table row. Deliberately only matches inside the CI section, so an
# incidental "build-test.yml" mentioned in prose elsewhere in the README does
# not create a false row.
table_names = set(re.findall(r"^\|\s*`([\w.-]+\.ya?ml)`", ci_section, re.MULTILINE))

errors = []

undocumented = sorted((names - EXCLUDE_FROM_TABLE) - table_names)
for name in undocumented:
    errors.append(
        f"{name} exists under .github/workflows/ but has no row in README.md's "
        "'## CI' table -- an undocumented gate"
    )

dangling = sorted(table_names - names)
for name in dangling:
    errors.append(
        f"README.md's '## CI' table names {name}, which does not exist under "
        ".github/workflows/ -- a dead row or a stale badge"
    )

if errors:
    print("lint-docs-drift: FAIL", file=sys.stderr)
    for e in errors:
        print(f"  - {e}", file=sys.stderr)
    sys.exit(1)

print(f"lint-docs-drift: clean -- {len(names)} workflow(s), all documented")
sys.exit(0)
PYEOF

#!/usr/bin/env python3
"""check-lane-map.py -- fail loudly when ci.yml's documented lane map drifts
from its actual `needs:` graph.

THE PROBLEM. The header comment in .github/workflows/ci.yml describes lane
membership in prose ("Lane B: build-test -> fuzzing -> asan"). Nothing enforces
that prose stays true when a `needs:` edge is added, removed or retargeted --
a future edit can silently desync the two, and a human skimming the comment
has no way to tell.

WHAT THIS ASSERTS. It parses the REAL `needs:` edges out of the `jobs:` map
(the thing that actually SETS ordering -- not a description of it) and compares
that edge set against the lane map hardcoded below, which mirrors the header
comment. A mismatch in either direction (an edge the code has that the map
doesn't document, or a documented edge the code no longer has) is a hard
failure. This is deliberately NOT a grep for a job name or a substring that
every line contains -- a check that reads what the graph SAYS about itself
rather than what actually orders execution proves nothing.

NO PyYAML DEPENDENCY, ON PURPOSE. PyYAML is only guaranteed present in the
security-scanners CI environment (see ci/linter/lint-ci-ports.sh); this job
runs standalone on the generic self-hosted label set and must not depend on
optional interpreter state. Parsing is done with a small state machine over
top-level `jobs:` children -- sufficient because ci.yml's job list is flat
(no reusable-workflow `needs:` nesting inside `with:` blocks etc.) and this
script only ever has to understand ITS OWN file's shape, not general YAML.

Usage:
    ci/tools/check-lane-map.py <path-to-ci.yml>

Exit 0 if the needs: graph matches the documented lane map, 1 otherwise.
"""

import re
import sys

# The documented lane map, mirroring the header comment in ci.yml. Update both
# together -- that's the entire point of this check.
#
# Each entry: job name -> sorted list of its documented `needs:` job names
# (empty list = unchained / no needs:).
DOCUMENTED_NEEDS = {
    "changes": [],
    "build-test": [],
    "fuzzing": ["build-test"],
    "asan": ["fuzzing"],
    "valgrind": [],
    "security-scanners": ["build-test"],
    "codeql": [],
}

# Matches a top-level job key under `jobs:`, e.g. "  build-test:" (exactly
# 2-space indent, no further nesting -- that is ci.yml's job-list shape).
JOB_RE = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")

# Matches a `needs:` line nested one level under a job (4-space indent),
# either scalar form (`needs: build-test`) or the start of a list form
# (`needs:` alone, followed by `- item` lines).
NEEDS_SCALAR_RE = re.compile(r"^    needs:\s*(\S.*)$")
NEEDS_BLOCK_RE = re.compile(r"^    needs:\s*$")
LIST_ITEM_RE = re.compile(r"^      -\s*(\S.*)$")


def parse_needs(path):
    """Return {job_name: sorted [needs...]} by scanning ci.yml's jobs: block."""
    with open(path, "r") as f:
        lines = f.readlines()

    in_jobs = False
    current_job = None
    result = {}
    collecting_list = False

    for raw in lines:
        line = raw.rstrip("\n")

        if line == "jobs:":
            in_jobs = True
            continue
        if not in_jobs:
            continue

        job_match = JOB_RE.match(line)
        if job_match:
            current_job = job_match.group(1)
            result[current_job] = []
            collecting_list = False
            continue

        if current_job is None:
            continue

        if collecting_list:
            item_match = LIST_ITEM_RE.match(line)
            if item_match:
                result[current_job].append(item_match.group(1).strip().strip("\"'"))
                continue
            else:
                collecting_list = False
                # fall through: this line might be something else for this job

        scalar_match = NEEDS_SCALAR_RE.match(line)
        if scalar_match:
            val = scalar_match.group(1).strip()
            if val.startswith("[") and val.endswith("]"):
                items = [x.strip().strip("\"'") for x in val[1:-1].split(",") if x.strip()]
                result[current_job].extend(items)
            else:
                result[current_job].append(val.strip("\"'"))
            continue

        if NEEDS_BLOCK_RE.match(line):
            collecting_list = True
            continue

    return {job: sorted(needs) for job, needs in result.items()}


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <path-to-ci.yml>", file=sys.stderr)
        return 1

    path = sys.argv[1]
    actual_needs = parse_needs(path)

    errors = []

    for job, documented in DOCUMENTED_NEEDS.items():
        if job not in actual_needs:
            errors.append(f"documented lane member '{job}' is missing from ci.yml jobs:")
            continue
        actual = actual_needs[job]
        documented_sorted = sorted(documented)
        if actual != documented_sorted:
            errors.append(
                f"job '{job}': documented needs={documented_sorted} but actual needs={actual}"
            )

    for job in actual_needs:
        if job not in DOCUMENTED_NEEDS:
            errors.append(f"job '{job}' exists in ci.yml but is not in the documented lane map")

    if errors:
        print("LANE MAP DRIFT DETECTED:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(f"lane map OK: {len(DOCUMENTED_NEEDS)} jobs match their documented needs: edges")
    return 0


if __name__ == "__main__":
    sys.exit(main())

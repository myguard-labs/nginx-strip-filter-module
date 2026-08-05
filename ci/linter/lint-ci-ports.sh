#!/usr/bin/env bash
# Copyright (C) 2026 Thijs Eilander
# SPDX-License-Identifier: BSD-2-Clause
#
# ci/linter/lint-ci-ports.sh -- enforce that all runtime-bearing CI jobs declare
# TEST_BASE_PORT to avoid port collisions on the shared self-hosted runner.
#
# Exit codes (matches the run-all.sh contract):
#   0 -- all runtime jobs declare TEST_BASE_PORT
#   1 -- at least one runtime job missing TEST_BASE_PORT or other configuration error
#   2 -- prerequisite missing (e.g., PyYAML not available), preventing verification
#
# This checker enforces the guarantee that ci/tools/max-port.sh documents in its
# header: every job that runs the runtime suite (prove -v ci/t/ and/or
# ci/tools/test_runtime.py) must declare TEST_BASE_PORT before it starts, so
# ci/tools/max-port.sh can verify the band is safe. A missing declaration allows
# the job to silently use the default 18880, which can collide with another job
# on builder02's persistent pool.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Check for PyYAML. It is available in the security-scanners CI environment
# (installed alongside semgrep), and we prefer it over regex: it correctly
# parses the full YAML grammar without false positives on comments, strings, etc.
if ! command -v python3 >/dev/null 2>&1; then
  echo "lint-ci-ports.sh: python3 not found; cannot verify workflow YAML" >&2
  exit 2
fi

if ! python3 -c "import yaml" 2>/dev/null; then
  echo "lint-ci-ports.sh: PyYAML not available; cannot parse workflow YAML" >&2
  exit 2
fi

# Scan .github/workflows/*.yml for jobs that run the runtime suite.
# A job is runtime-bearing if ANY of its steps runs:
#   - prove -v ci/t/
#   - ci/tools/test_runtime.py
# A runtime job MUST declare TEST_BASE_PORT in its env block (or inherit from
# the workflow-level env, though that is not the pattern here).

python3 << 'PYEOF'
import sys
import yaml
import glob
import os

def find_runtime_jobs():
    """Scan all workflow YAMLs for jobs that run the runtime suite."""
    runtime_jobs = []

    for yml_file in sorted(glob.glob('.github/workflows/*.yml')):
        try:
            with open(yml_file, 'r') as f:
                doc = yaml.safe_load(f)
        except Exception as e:
            print(f"lint-ci-ports.sh: failed to parse {yml_file}: {e}", file=sys.stderr)
            sys.exit(2)

        if not doc or 'jobs' not in doc:
            continue

        for job_name, job_data in doc['jobs'].items():
            if not isinstance(job_data, dict):
                continue

            # Check if any step runs prove or test_runtime
            has_runtime_step = False
            if 'steps' in job_data:
                for step in job_data.get('steps', []):
                    if isinstance(step, dict):
                        run_cmd = step.get('run', '')
                        if 'prove' in run_cmd or 'test_runtime' in run_cmd:
                            has_runtime_step = True
                            break

            if has_runtime_step:
                # Check if job declares TEST_BASE_PORT statically
                env = job_data.get('env', {})
                has_port_static = isinstance(env, dict) and 'TEST_BASE_PORT' in env
                port_value = env.get('TEST_BASE_PORT', 'NOT SET') if isinstance(env, dict) else 'NOT SET'

                # Check if job calls max-port.sh (which requires TEST_BASE_PORT to be set)
                has_port_dynamic = False
                if 'steps' in job_data:
                    for step in job_data.get('steps', []):
                        if isinstance(step, dict):
                            run_cmd = step.get('run', '')
                            if 'max-port.sh' in run_cmd:
                                has_port_dynamic = True
                                if port_value == 'NOT SET':
                                    port_value = 'set dynamically'
                                break

                has_port = has_port_static or has_port_dynamic
                has_matrix = 'strategy' in job_data and 'matrix' in job_data.get('strategy', {})

                runtime_jobs.append({
                    'file': yml_file,
                    'job': job_name,
                    'has_port': has_port,
                    'port_value': port_value,
                    'has_matrix': has_matrix,
                })

    return runtime_jobs

runtime_jobs = find_runtime_jobs()

if len(runtime_jobs) < 5:
    print(f"lint-ci-ports.sh: selector found {len(runtime_jobs)} runtime jobs, expected >=5", file=sys.stderr)
    print(f"lint-ci-ports.sh: scanner may be broken; treating as failure", file=sys.stderr)
    sys.exit(2)

# Report findings
missing_port = [j for j in runtime_jobs if not j['has_port']]
has_port = [j for j in runtime_jobs if j['has_port']]

print(f"lint-ci-ports.sh: found {len(runtime_jobs)} runtime jobs ({len(has_port)} declared, {len(missing_port)} missing)")

for j in has_port:
    print(f"  ✓ {j['file']}: {j['job']} — TEST_BASE_PORT={j['port_value']}")

if missing_port:
    for j in missing_port:
        matrix_note = " [MATRIX]" if j['has_matrix'] else ""
        print(f"  ✗ {j['file']}: {j['job']}{matrix_note} — missing TEST_BASE_PORT", file=sys.stderr)
    sys.exit(1)

sys.exit(0)
PYEOF

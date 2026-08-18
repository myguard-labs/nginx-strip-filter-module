# ci/linter — local lint gate

Mirrors the cheap half of remote CI so a push does not burn a round-trip on a
finding a local tool could have named in two seconds. `run-all.sh` is the
single entry point — every script here is standalone and can also be run
directly. `.githooks/pre-commit` runs `pre-commit` (see
[CONTRIBUTING.md](../../CONTRIBUTING.md) for how to enable it), which is a
separate, overlapping gate — see "Two overlapping local gates" below.

## Layout

| File | Covers | Gate |
|---|---|---|
| `install.sh` | — | installs every tool `run-all.sh` needs: apt (flawfinder, clang-tidy, shellcheck, cppcheck) → pipx/pip3 (version-pinned semgrep, `ruff==0.16.1`) → pinned checksummed binary (actionlint) |
| `run-all.sh` | `src/*.c`, `*.sh`/`.githooks/*`, `.github/workflows/*.yml`, `*.py` | runs every checker below, reports once |
| `lint-ci-ports.sh` | `.github/workflows/*.yml` | every runtime-bearing job (runs `prove -v ci/t/` and/or `ci/tools/test_runtime.py`) declares `TEST_BASE_PORT` before its first runtime step |
| `lint-docs-drift.sh` | `.github/workflows/*.yml`, `README.md` | every workflow under `.github/workflows/` has a row in README's `## CI` table, and every table row names a workflow that exists — both directions |

`run-all.sh` itself also gates, without a separate wrapper script:

- **flawfinder** (`src/*.c`, blocks at `--minlevel=4`)
- **semgrep** (`src/*.c`, `p/c` + `p/security-audit`, blocks at `>=WARNING`; exit-code 1 vs a tool error is distinguished so a semgrep crash never silently reports as "clean")
- **shellcheck** (`-S warning`; file set is `git ls-files '*.sh' '*.bash'` UNION a shebang grep over every other tracked file, so an extensionless script like `.githooks/pre-commit` is not invisible to it)
- **actionlint** (`.github/workflows/*.yml`, syntax + expression checks)
- **ruff** (`git ls-files '*.py'`, pinned `ruff==0.16.1` so local and CI find the same findings)
- **clang-tidy** (`cert-*`, `clang-analyzer-security.*`, `src/*.c`) — **CI-only in practice**: it needs `ngx_auto_config.h`, which only exists inside a configured nginx source tree. Set `NGINX_SRC_TREE` to one to run it locally; without it the script exits `2` (prerequisite missing), never a silent skip.

## Install

```bash
ci/linter/install.sh
```

Idempotent — installs only what is missing. Uses apt for system packages and
pipx (falling back to `pip3 --break-system-packages`) for `semgrep` and
`ruff==0.16.1`; fetches `actionlint` as a version-pinned, checksum-verified
binary to `~/.local/bin` (make sure that is on `PATH`).

`ruff` is pinned because an unpinned upgrade changes findings under you and
local stops matching CI's `security-scanners.yml`/`run-all.sh` combination.
Bump it in both places together.

## Enable the pre-commit hook

See [CONTRIBUTING.md](../../CONTRIBUTING.md#local-checks-before-you-push) for
how to wire `.githooks/pre-commit` via `core.hooksPath`. That hook runs the
`pre-commit` framework (`.pre-commit-config.yaml`), which overlaps with but is
not identical to `run-all.sh` — see below.

## Use it

```bash
ci/linter/run-all.sh                 # every check this script drives
NGINX_SRC_TREE=/path/to/configured-nginx ci/linter/run-all.sh   # also runs clang-tidy
```

Exit codes: `0` clean, `1` findings present, `2` a required tool (or
prerequisite, e.g. `NGINX_SRC_TREE` for clang-tidy) is missing.

## Two overlapping local gates

This repo has **two** local lint mechanisms that both exist and both work,
covering overlapping but not identical ground:

- **`ci/linter/run-all.sh`** (this directory) — flawfinder, semgrep,
  shellcheck, actionlint, `lint-ci-ports.sh`, ruff, clang-tidy (CI-only). Runs
  over the whole tracked tree, not just staged files.
- **`.pre-commit-config.yaml`** via `.githooks/pre-commit` — whitespace/EOF
  fixers, `detect-private-key`, `gitleaks`, plus its own flawfinder, semgrep,
  cppcheck and ruff hooks at thresholds mirroring `run-all.sh`'s. Runs on
  staged files only, on every `git commit`.

They are not wired together — CI's `security-scanners.yml` and `build-test.yml`
run the tools `run-all.sh` also runs (that duplication is deliberate: local
and CI must find the same findings), and neither invokes `pre-commit` or
`run-all.sh` directly. Running both locally on the same commit means running
flawfinder/semgrep/ruff twice; that is the accepted cost of two independent
gates rather than one that could silently drift.

There is no `lint.yml` workflow and no `LINT_ONLY` selector in this repo —
`ci/linter/` was ported from the `nginx-skeleton-module` reference layout but
not (yet) wired as its own CI job; `security-scanners.yml` and `build-test.yml`
cover the same tools directly. See the reference skeleton's own
`ci/linter/README.md` for the fuller checker set (`lint-nginx.sh`,
`lint-perl.sh`, `lint-yaml.sh`, `lint-ci-runners.sh`, `lint-spelling.sh`,
`workflow_policy.py`) this module has not adopted.

## Extending

- New shell/C/Python file type already covered by an existing checker: no
  change needed, `run-all.sh`'s file-selection globs pick it up automatically.
- New workflow: add a row to README's `## CI` table in the same commit, or
  `lint-docs-drift.sh` fails the build. See its header comment for the
  excluded-workflow policy.
- New checker: add a block to `run-all.sh` following the existing pattern
  (`status=1` on a real finding, `exit 2` on a missing tool/prerequisite,
  never a silent pass), and add its install step to `install.sh`.

# CI Performance and Timing Data

This document records measured job durations from representative CI runs on the nginx-strip-filter-module.

## Timing Run 2026-08-05

**Run ID:** 31008062208 (branch: grind-lint-speed-budget, status: completed successfully)
**Run Created:** 2026-08-05T12:59:07Z

This run captures timing for all 10 jobs in the current workflow suite. Columns are:

- **Job Name**: Display name from the workflow
- **Runner (runs-on)**: GitHub Actions runner specification from the workflow file
- **Runner Type**: Classification (self-hosted vs GitHub-hosted)
- **Queue Time (s)**: Time from run creation to job start
- **Execution Time (s)**: Time from job start to job completion
- **Total Time (s)**: Queue + Execution
- **Started At**: UTC timestamp of job start
- **Completed At**: UTC timestamp of job completion

### Self-Hosted Runner Jobs

| Job Name | Runner (runs-on) | Runner Type | Queue Time (s) | Execution Time (s) | Total Time (s) | Started At | Completed At |
|----------|------------------|-------------|---|---|---|---|---|
| Build (nginx 1.31.1 / gcc) | ["self-hosted","builder02","lxc"] | self-hosted | 4 | 30 | 34 | 12:59:11Z | 12:59:41Z |
| A/UBSan (ci/t/ suite) | ["self-hosted","builder02","lxc"] | self-hosted | 3 | 65 | 68 | 12:59:10Z | 13:00:15Z |
| Valgrind Memcheck lite | ["self-hosted","builder02","lxc"] | self-hosted | 3 | 769 | 772 | 12:59:10Z | 13:11:59Z |
| Build&Test / Validation | ["self-hosted","builder02","lxc"] | self-hosted | 3 | 24 | 27 | 12:59:10Z | 12:59:34Z |
| Linter (ci/linter/run-all.sh) | ["self-hosted","builder02","lxc"] | self-hosted | 4 | 35 | 39 | 12:59:11Z | 12:59:46Z |
| Security scanners / Security scanners | ["self-hosted","builder02","lxc"] | self-hosted | 3 | 76 | 79 | 12:59:10Z | 13:00:26Z |
| Core unit tests | ["self-hosted","builder02","lxc"] | self-hosted | 4 | 16 | 20 | 12:59:11Z | 12:59:27Z |
| Fuzz regression (20s/target) | ["self-hosted","builder02","lxc"] | self-hosted | 4 | 149 | 153 | 12:59:11Z | 13:01:40Z |
| Test::Nginx | ["self-hosted","builder02","lxc"] | self-hosted | 36 | 45 | 81 | 12:59:43Z | 13:00:28Z |

### GitHub-Hosted Runner Jobs

| Job Name | Runner (runs-on) | Runner Type | Queue Time (s) | Execution Time (s) | Total Time (s) | Started At | Completed At |
|----------|------------------|-------------|---|---|---|---|---|
| CodeQL / Analyze C | ubuntu-latest | GitHub-hosted | 10 | 72 | 82 | 12:59:17Z | 13:00:29Z |

## Analysis

### Queue Time vs. Execution Time

The queue time is the interval from run creation (12:59:07Z) to job start. Execution time is from job start to completion.

- **Fastest**: Validation job (3s queue, 24s execution)
- **Slowest**: Valgrind Memcheck lite (3s queue, 769s execution = 12:49 min)
- **Test::Nginx notable**: 36s queue time (awaits Build artifact), then 45s execution
- **GitHub-hosted (CodeQL)**: 10s queue time, 72s execution

### Self-Hosted Runner Slot Usage

All 9 self-hosted jobs target `["self-hosted","builder02","lxc"]` — the same
runner label set. `builder02` is one physical host, but a host is not a slot:
concurrency is bounded by the number of registered *runner processes* carrying
that label.

Measured, not inferred (`gh api orgs/myguard-labs/actions/runners`, 2026-08-05):

| Label group | Registered runners | Matching `builder02` + `lxc` |
|---|---|---|
| `builder02-runner-01..04` | 4 | 4 |
| `builder02-docker-01..02` | 2 | 0 (docker label set, not `lxc`) |
| `builder03-*` | 7 | 0 (different host label) |

**Real self-hosted runner slot count: 4** (`builder02-runner-01..04`).

Note the repo-scoped endpoint (`repos/.../actions/runners`) returns
`total_count: 0` — these runners are registered at the **org** level, so a
repo-scoped query reads as "no self-hosted runners" and must not be used as
evidence.

This is corroborated by the run itself: 8 of the 9 self-hosted jobs started
within 12:59:10–12:59:11Z, which is impossible on a single slot. The 4-slot
ceiling is what cards 37–38 must size lanes against.

### GitHub-Hosted Runner Slots

The CodeQL job runs on `ubuntu-latest` without private selectors — no self-hosted slots consumed. GitHub manages hosted runner allocation.

### Sequential vs. Parallel

Within a single run:
- Build artifacts are required by Test::Nginx, introducing a 36s queue delay for that job
- Other jobs depend only on checkout and start nearly simultaneously (3-4s queue for job setup overhead)
- Valgrind runs longest (769s) but does not block other jobs due to concurrency group isolation

## Workflow Coverage

This timing represents the **main CI suite** triggered on every PR/push via `ci.yml`. The workflows laned here:

- `.github/workflows/build-test.yml` — Build, Validation, Core unit tests, Test::Nginx
- `.github/workflows/asan.yml` — A/UBSan suite
- `.github/workflows/valgrind.yml` — Valgrind Memcheck lite
- `.github/workflows/security-scanners.yml` — Security scanners, Linter
- `.github/workflows/codeql.yml` — CodeQL Analyze C
- `.github/workflows/fuzzing.yml` — Fuzz regression

Deep/extended checks (monthly cron, full Memcheck+Helgrind, longer fuzz runs) live in `ci-deep.yml` and are not captured here.

## Notes

- Valgrind completedAt was fetched fresh from the API (previously stale at `0001-01-01`); 769s execution confirmed.
- No synthetic timing estimation was used; all figures are from the GitHub Actions API.
- Queue time is job overhead (runner acquisition, setup steps) + any workflow-enforced serialization (e.g., Test::Nginx awaiting Build artifacts).
- This is a representative run, not averaged across multiple runs.

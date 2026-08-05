# ngx_http_strip_filter_module

[![Build&Test](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/build-test.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/build-test.yml)
[![Security Scanners](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/security-scanners.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/security-scanners.yml)
[![Fuzzing](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/fuzzing.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/fuzzing.yml)
[![A/UBSan](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/asan.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/asan.yml)
[![CodeQL](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/codeql.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/codeql.yml)
[![Valgrind](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/valgrind.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/valgrind.yml)
[![CI Deep](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/ci-deep.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/ci-deep.yml)

A dynamic nginx response-body minifier. Strips newlines, redundant whitespace
and comments from HTML, CSS, JavaScript and JSON responses — context-aware, so
significant bytes are never removed.

> **Upgrade note:** output is now **more conservative by default.** Four
> transforms proven to be able to corrupt valid input under some inputs are
> now opt-in, gated behind `strip_aggressive on;` (default stays `off`):
> CSS zero-unit stripping (`0px` → `0`, unsafe inside e.g. a media-query
> feature test), JS whitespace elision between identical operator characters
> (`+ +`/`- -`), and SVG/XML/HTML text-node whitespace collapsing (unsafe
> without `xml:space`/`white-space:pre` awareness, which this byte-level
> filter does not have). If you relied on the previous (pre-upgrade) output,
> add `strip_aggressive on;` to restore it — every other transform is
> unaffected and unchanged. See `## Directives` below and `CHANGES`.

**See also:** [nginx-strip-filter-module: CSS and JavaScript Minification](https://deb.myguard.nl/nginx-strip-filter-module-css-javascript-minification/) — full write-up, benchmarks and config examples on deb.myguard.nl.

## Features

| Content type | What is stripped |
|---|---|
| `text/html` | `<!-- -->` comments, inter-tag whitespace/newline runs, boolean attrs (`disabled="disabled"` → `disabled`), safe attribute-value unquoting (`class="btn"` → `class=btn`), text-node whitespace collapse (**`strip_aggressive` only**) |
| `text/css` | `/* */` comments, redundant whitespace, trailing `;` before `}`, leading zeros (`0.5` → `.5`), 6→3-digit hex colors (`#ffaabb` → `#fab`), zero units (`0px` → `0`, **`strip_aggressive` only**) |
| `application/javascript`, `text/javascript` | `//` and `/* */` comments, safe newline collapse (whitespace between identical operator characters, e.g. `+ +`, kept unless **`strip_aggressive`**) |
| `application/json` | all structural whitespace |
| `image/svg+xml` | XML comments, inter-tag whitespace (CDATA preserved), text-node whitespace collapse (**`strip_aggressive` only**) |
| `text/xml`, `application/xml`, `*+xml` | XML comments, inter-tag whitespace (CDATA preserved), text-node whitespace collapse (**`strip_aggressive` only**) — RSS/Atom/sitemap |

**Smart, not brute:** regions that must survive verbatim are passed through
untouched:

- HTML `<pre>`, `<textarea>`, `<script>`, `<style>` element bodies
- CSS and JSON string literals
- JS string (`'…'`, `"…"`), template (`` `…` ``), and regex (`/…/`) literals
- JS newlines where Automatic Semicolon Insertion would fire

Runs **before** the compression filters so gzip/brotli/zstd compress
already-minified bytes. Output is always `<=` input length; no extra heap
allocations beyond a single per-request pooled buffer.

## Directives

All directives are valid in `http`, `server` and `location` blocks.

| Directive | Default | Description |
|---|---|---|
| `strip` | `off` | Enable HTML minification |
| `strip_css` | `off` | Enable CSS minification |
| `strip_js` | `off` | Enable JavaScript minification |
| `strip_json` | `off` | Enable JSON minification |
| `strip_svg` | `off` | Enable SVG (`image/svg+xml`) minification |
| `strip_xml` | `off` | Enable XML minification (`text/xml`, `application/xml`, any `+xml` subtype — RSS/Atom/sitemap) |
| `strip_aggressive` | `off` | Enable four transforms proven to be able to corrupt valid input under some inputs: CSS zero-unit stripping, JS identical-operator whitespace elision, and SVG/XML/HTML text-node whitespace collapse. Off (default) = conservative, byte-preserving for these cases. Restores pre-upgrade output — see the note above `## Features`. |
| `strip_min_size` | `0` | Skip bodies smaller than this (bytes) |
| `strip_max_size` | `10m` | Skip bodies larger than this (buffered whole) |
| `strip_types` | `text/html` | Extra MIME types treated as HTML |

## Quick start

```nginx
load_module modules/ngx_http_strip_filter_module.so;

http {
    server {
        strip      on;       # HTML
        strip_css  on;
        strip_js   on;
        strip_json on;
    }
}
```

### Per-location selective strip

```nginx
location /api/ {
    strip_json on;
}

location /static/ {
    strip     on;
    strip_css on;
    strip_js  on;
}
```

## Building

```bash
# dynamic module against an existing nginx source tree
./configure --with-compat --add-dynamic-module=/path/to/nginx-strip-filter-module
make modules
# result: objs/ngx_http_strip_filter_module.so
```

Or use `ci/tools/ci-build.sh` which downloads and builds nginx automatically:

```bash
bash ci/tools/ci-build.sh nginx 1.31.1
```

## Testing

Two suites, deliberately separate.

**Core unit tests** — `strip_core.c` has no nginx dependency, so its state
machines are driven directly. No server, no Perl, no network; the whole suite
runs in well under a second and emits TAP.

```bash
ci/tests/unit/run.sh
```

**Request-path tests** — `ci/t/basic.t` drives the filter through a real nginx via
`Test::Nginx::Socket`, which is the right instrument for directive handling,
content types and buffering.

```bash
TEST_NGINX_BINARY=/path/to/nginx \
TEST_NGINX_LOAD_MODULES=/path/to/ngx_http_strip_filter_module.so \
prove -v ci/t/
```

### Coverage

The goal is 100% coverage of `strip_core.c`; the current figure is **99.79%**,
with the single uncovered line carrying a `COVERAGE:` comment explaining why it
is unreachable. That is the standard: every uncovered line either gets a real
test or an honest note.

```bash
work=$(mktemp -d)
gcc -O0 -g --coverage -std=c11 -Isrc -o "$work/t" ci/tests/unit/test_scan.c src/strip_core.c
"$work/t" >/dev/null
gcov -o "$work" "$work/t-strip_core.gcda"
grep -n '#####' strip_core.c.gcov     # lines still needing a test or a note
```

There is **no coverage-percent gate in CI, by design.** The fastest way to move
a coverage number is a test that executes lines without asserting anything. The
gate that matters is the control mutation: every test group in
`ci/tests/unit/test_scan.c` names, in a comment, a one-line change to `src/strip_core.c`
that makes that group fail. A test whose control does not red it covers the line
and proves nothing. Two cases in this suite were caught being vacuous exactly
that way, and both comments record what the control taught us.

The method is documented in full at
[nginx-test-harness/docs/COVERAGE.md](https://github.com/myguard-labs/nginx-test-harness/blob/main/docs/COVERAGE.md)
and [COVERAGE-HOWTO.md](https://github.com/myguard-labs/nginx-test-harness/blob/main/docs/COVERAGE-HOWTO.md).

## Layout

```
.
├── config                    # nginx module manifest (no ngx_module_order — see ABI gotcha in memory)
├── src/                       # the module: nginx glue + the nginx-independent core
│   ├── ngx_http_strip_filter_module.c
│   ├── strip_core.c           # (u_char*, size_t) in, verdict out — no nginx types
│   └── strip_core.h
├── ci/
│   ├── tests/unit/            # standalone unit suite, drives strip_core.c directly
│   ├── t/                     # Test::Nginx::Socket request-path suite
│   ├── fuzz/                  # libFuzzer targets, one per content type, + seed corpora
│   ├── linter/                # local lint gate — see ci/linter/README.md
│   └── tools/                 # ci-build.sh, coverage.sh, sync-stamp.sh, bump scripts, ...
├── .github/workflows/         # see `## CI` below
├── .githooks/pre-commit       # tracked git hook — see CONTRIBUTING.md
└── CI_PERFORMANCE.md          # lane map + measured wall-clock, kept current every CI change
```

## CI

Only `ci.yml` has a `pull_request` trigger. The PR-time workflows below are
`workflow_call` members it lanes, so a PR asks for one run, not many.
`ci/linter/lint-docs-drift.sh` gates that this table and `.github/workflows/`
never drift apart — see [ci/linter/README.md](ci/linter/README.md).

| Workflow | Trigger | Gates |
|---|---|---|
| `ci.yml` | PR (the only `pull_request` entry point) | no gates of its own — lanes and dispatches every PR-time member below |
| `build-test.yml` | PR (via `ci.yml`) | build, Test::Nginx, ASan+UBSan, `unit-core` job (gcc+clang unit run of `ci/tests/unit/test_scan.c` against `src/strip_core.c` standalone, plus an ASan/UBSan unit run and an informational coverage report), `ci/tools/sync-stamp.sh --check` |
| `security-scanners.yml` | PR (via `ci.yml`) | flawfinder, clang-tidy, semgrep over the module sources |
| `fuzzing.yml` | PR (via `ci.yml`) | 20s/target fast fuzz regression across all 6 strip kinds (html/css/js/json/svg/xml) |
| `asan.yml` | PR (via `ci.yml`) | dedicated ASan+UBSan run of the Test::Nginx suite under a static build |
| `codeql.yml` | PR (via `ci.yml`) + monthly | CodeQL |
| `valgrind.yml` | weekly + dispatch (+ `workflow_call`) | Test::Nginx suite once under Valgrind memcheck (lite soak) — **deliberately removed from the PR lane** (was the 769s budget-setter; PR-lane wall-clock went 12m52s → 5m59s); per-PR memory-safety coverage is `asan.yml`. See `memory/labs/nginx-strip-filter-module/skeleton-findings.md` § F-VG. |
| `ci-deep.yml` | monthly + dispatch | exhaustive dynamic analysis — long fuzz, full memcheck + helgrind soak, Discord failure notify |
| `bump.yml` | weekly + dispatch | checks nginx.org/angie.software for newer pins, opens a PR against master if anything moved |

There is no `lint.yml` in this module yet — the reference skeleton's fuller
`ci/linter/` (perlcritic, yamllint, zizmor, spelling, its own `lint.yml`
runner) has not been ported; `security-scanners.yml` and `build-test.yml`
cover the equivalent tools directly. See "Linting" below for what this repo
does have.

## Requirements

- An nginx (or Angie) source tree, built with `--with-compat`, to build the
  dynamic module against.
- `gcc`/`clang`, `perl` + `Test::Nginx::Socket` (for `ci/t/`), `prove`.
- `clang` with libFuzzer support for `ci/fuzz/`.
- See [ci/linter/README.md](ci/linter/README.md) for the local lint toolchain
  (flawfinder, semgrep, shellcheck, actionlint, ruff, clang-tidy).

## Linting

Local lint gate lives under `ci/linter/` — install with `ci/linter/install.sh`,
run with `ci/linter/run-all.sh`. Full checker list, thresholds, the tracked
git hook, and how it relates to `.pre-commit-config.yaml`:
[ci/linter/README.md](ci/linter/README.md).

## Installing from deb.myguard.nl

```bash
curl -fsSL https://deb.myguard.nl/pubkey.gpg | sudo gpg --dearmor -o /etc/apt/trusted.gpg.d/myguard.gpg
echo "deb https://deb.myguard.nl/ $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/myguard.list
sudo apt update && sudo apt install libnginx-mod-http-strip-filter
```

Then add to `/etc/nginx/nginx.conf`:

```nginx
load_module modules/ngx_http_strip_filter_module.so;
```

## Caveats

- **Whole-body buffering is intentional, and `flush`/`sync` buffers are not
  honoured.** The filter accumulates the entire response body before minifying
  it, so a streamed or SSE-style response is held until it is complete rather
  than being forwarded incrementally. An upstream buffer carrying `flush` or
  `sync` without a terminal flag does *not* cause an early emit. This is a
  deliberate architectural constraint, not an oversight: the minifier keeps no
  lexer state between calls, so emitting a partial body would split it into
  independent minification passes and corrupt any comment or string that spans
  the seam — a correctness bug in exchange for latency. Buffering is bounded by
  `strip_max_size`; set it to skip very large or streaming responses (video
  manifests, event streams, long-polling endpoints), which are then passed
  through untouched. Making the minifier resumable so flush can be honoured is
  tracked as future work.
- Inline `<script>`/`<style>` bodies in HTML are preserved verbatim; they are
  not recursively minified. Enable `strip_js`/`strip_css` to minify standalone
  `.js`/`.css` files separately.
- Does not handle multi-part or chunked-encoded upstream responses that arrive
  in more than one chain beyond the last buffer — in practice nginx upstream
  modules always set `last_buf` on the final buffer of a response.
- Attribute-value unquoting is HTML-only; SVG/XML attribute values always stay
  quoted (XML syntax requires it). CSS `url(...)` tokens are passed through
  verbatim (no whitespace/zero rewriting inside them).

## License

BSD-2-Clause (same terms as nginx and Angie) — see [LICENSE](LICENSE).

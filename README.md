# ngx_http_strip_filter_module

[![Build & Test](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/build-test.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/build-test.yml)
[![Security scanners](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/security-scanners.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/security-scanners.yml)
[![Fuzzing](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/fuzzing.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/fuzzing.yml)
[![Valgrind](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/valgrind.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/valgrind.yml)
[![CI Deep](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/ci-deep.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/ci-deep.yml)
[![CodeQL](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/codeql.yml/badge.svg)](https://github.com/myguard-labs/nginx-strip-filter-module/actions/workflows/codeql.yml)

A dynamic nginx response-body minifier. Strips newlines, redundant whitespace
and comments from HTML, CSS, JavaScript and JSON responses — context-aware, so
significant bytes are never removed.

**See also:** [nginx-strip-filter-module: CSS and JavaScript Minification](https://deb.myguard.nl/nginx-strip-filter-module-css-javascript-minification/) — full write-up, benchmarks and config examples on deb.myguard.nl.

## Features

| Content type | What is stripped |
|---|---|
| `text/html` | `<!-- -->` comments, inter-tag whitespace/newline runs, boolean attrs (`disabled="disabled"` → `disabled`), safe attribute-value unquoting (`class="btn"` → `class=btn`) |
| `text/css` | `/* */` comments, redundant whitespace, trailing `;` before `}`, zero units (`0px` → `0`), leading zeros (`0.5` → `.5`), 6→3-digit hex colors (`#ffaabb` → `#fab`) |
| `application/javascript`, `text/javascript` | `//` and `/* */` comments, safe newline collapse |
| `application/json` | all structural whitespace |
| `image/svg+xml` | XML comments, inter-tag whitespace (CDATA preserved) |
| `text/xml`, `application/xml`, `*+xml` | XML comments, inter-tag whitespace (CDATA preserved) — RSS/Atom/sitemap |

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

Or use `tools/ci-build.sh` which downloads and builds nginx automatically:

```bash
bash tools/ci-build.sh nginx 1.31.1
```

## Testing

Two suites, deliberately separate.

**Core unit tests** — `strip_core.c` has no nginx dependency, so its state
machines are driven directly. No server, no Perl, no network; the whole suite
runs in well under a second and emits TAP.

```bash
cc -std=c11 -Wall -Wextra -Werror -I. -o /tmp/t t/strip_core_test.c strip_core.c && /tmp/t
```

**Request-path tests** — `t/basic.t` drives the filter through a real nginx via
`Test::Nginx::Socket`, which is the right instrument for directive handling,
content types and buffering.

```bash
TEST_NGINX_BINARY=/path/to/nginx \
TEST_NGINX_LOAD_MODULES=/path/to/ngx_http_strip_filter_module.so \
prove -v t/
```

### Coverage

The goal is 100% coverage of `strip_core.c`; the current figure is **99.79%**,
with the single uncovered line carrying a `COVERAGE:` comment explaining why it
is unreachable. That is the standard: every uncovered line either gets a real
test or an honest note.

```bash
work=$(mktemp -d)
gcc -O0 -g --coverage -std=c11 -I. -o "$work/t" t/strip_core_test.c strip_core.c
"$work/t" >/dev/null
gcov -o "$work" "$work/t-strip_core.gcda"
grep -n '#####' strip_core.c.gcov     # lines still needing a test or a note
```

There is **no coverage-percent gate in CI, by design.** The fastest way to move
a coverage number is a test that executes lines without asserting anything. The
gate that matters is the control mutation: every test group in
`t/strip_core_test.c` names, in a comment, a one-line change to `strip_core.c`
that makes that group fail. A test whose control does not red it covers the line
and proves nothing. Two cases in this suite were caught being vacuous exactly
that way, and both comments record what the control taught us.

The method is documented in full at
[nginx-test-harness/docs/COVERAGE.md](https://github.com/myguard-labs/nginx-test-harness/blob/main/docs/COVERAGE.md)
and [COVERAGE-HOWTO.md](https://github.com/myguard-labs/nginx-test-harness/blob/main/docs/COVERAGE-HOWTO.md).

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

- Bodies are buffered whole before minification. Set `strip_max_size` to skip
  very large responses (streaming, video manifests, etc.).
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

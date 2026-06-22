# ngx_http_strip_filter_module

A dynamic nginx response-body minifier. Strips newlines, redundant whitespace
and comments from HTML, CSS, JavaScript and JSON responses — context-aware, so
significant bytes are never removed.

**See also:** [deb.myguard.nl](https://deb.myguard.nl) — Debian/Ubuntu packages
including this module for nginx and Angie.

## Features

| Content type | What is stripped |
|---|---|
| `text/html` | `<!-- -->` comments, inter-tag whitespace/newline runs |
| `text/css` | `/* */` comments, redundant whitespace |
| `application/javascript`, `text/javascript` | `//` and `/* */` comments, safe newline collapse |
| `application/json` | all structural whitespace |

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

## License

MIT — see [LICENSE](LICENSE).

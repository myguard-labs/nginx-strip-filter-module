# Fuzz targets — provenance

Six libFuzzer targets, one per `strip_kind_t` value in
[`src/strip_core.h`](../../src/strip_core.h). All six link and call the real
`strip_minify()` from [`src/strip_core.c`](../../src/strip_core.c) — there is
no reimplementation. `ci/fuzz/build.sh` compiles
[`fuzz_strip.c`](fuzz_strip.c) (the shared libFuzzer harness) together with
`src/strip_core.c`, selecting the target kind via `-DFUZZ_KIND=N`:

| target            | `FUZZ_KIND` | `strip_kind_t`  | dispatched to      |
|-------------------|:-----------:|-----------------|---------------------|
| `fuzz_strip_html` | 0           | `STRIP_HTML`    | `strip_html()`      |
| `fuzz_strip_css`  | 1           | `STRIP_CSS`     | `strip_css()`       |
| `fuzz_strip_js`   | 2           | `STRIP_JS`      | `strip_js()`        |
| `fuzz_strip_json` | 3           | `STRIP_JSON`    | `strip_json()`      |
| `fuzz_strip_svg`  | 4           | `STRIP_SVG`     | `strip_svg()`       |
| `fuzz_strip_xml`  | 5           | `STRIP_XML`     | `strip_svg()`       |

`STRIP_SVG` and `STRIP_XML` share one minifier (`strip_svg`, see the
`strip_minify` dispatch switch, `src/strip_core.c:1018`) — SVG has no
`<pre>`/`<script>` raw-text semantics at the filter layer, and generic XML
(RSS/Atom/sitemap) needs the same comment-strip + CDATA-passthrough +
inter-tag-whitespace-collapse behavior. Two targets, one code path, by design.

## Surface-to-target map

Each target's byte surface is the attacker-controlled HTTP response body of
the matching `Content-Type`, as selected by `ngx_http_strip_select()` in
[`src/ngx_http_strip_filter_module.c:188-264`](../../src/ngx_http_strip_filter_module.c#L188):

- `fuzz_strip_css` — `text/css` bodies
- `fuzz_strip_js` — `application/javascript` / `text/javascript` bodies
- `fuzz_strip_json` — `application/json` bodies
- `fuzz_strip_svg` — `image/svg+xml` bodies
- `fuzz_strip_xml` — `text/xml`, `application/xml`, any `+xml` subtype
- `fuzz_strip_html` — anything matching `strip_types` (default `text/html`)

The harness ([`fuzz_strip.c`](fuzz_strip.c)) feeds libFuzzer's input
byte-for-byte as `[data, data+size)` straight into `strip_minify()`, which is
exactly the contract `strip_core.h` documents: "a NUL-free byte range". The
module never rejects embedded NULs before buffering, so that's a difference
between the fuzz harness's implicit assumption and the real module's input
handling — see Uncovered surfaces below.

## Corpus provenance

All seeds are hand-authored, minimal, and tracked in git (`ci/fuzz/corpus_*/`).
Each file is named for the transform feature it exercises and is under 140
bytes:

| kind | seeds | files (feature exercised) |
|------|:-----:|----------------------------|
| css  | 4 | `basic` (comment + multi-space run), `dense` (adjacent rules, no whitespace), `media` (`@media` block nesting), `string` (quoted string content must survive verbatim) |
| html | 5 | `attrs` (multi-line tag, quoted attr value), `basic` (comment strip + text collapse), `pre` (`<pre>` raw-text passthrough), `script_style` (`<script>`/`<style>` raw-text passthrough), `textarea` (`<textarea>` raw-text passthrough) |
| js   | 3 | `basic` (line + block comments), `literals` (string, template literal, regex literal — must not be touched), `spaces` (token-separating whitespace collapse) |
| json | 3 | `array` (array-of-objects whitespace), `basic` (mixed types + string with internal spaces), `nested` (nested-object whitespace collapse) |
| svg  | 3 | `basic` (comment + self-closing element + multi-line attrs), `circle` (dense single-line), `text` (`<text>` content whitespace) |
| xml  | 3 | `basic` (XML declaration + comment), `cdata` (`<![CDATA[...]]>` passthrough), `nested` (nested-element whitespace collapse) |

Beyond the tracked seeds, each target accumulates libFuzzer-discovered corpus
entries locally under `ci/fuzz/corpus_<kind>/` as content-addressed
(SHA1-named, extensionless) files. These are coverage-guided exploration
output, not authored — they are gitignored (see `.gitignore`) and never
committed; they persist on disk between local runs but are not provenance for
anything beyond "libFuzzer found this input interesting".

No past-crash reproducers exist for this module (no fuzzing-found bug has
been filed to date), so none are seeded. If one is found, its minimized
reproducer should be added as a tracked, named seed (e.g.
`corpus_css/regress-<short-desc>.css`) — not left as an anonymous
crash-artifact.

## Dictionary provenance

Each `fuzz.dict.<kind>` supplies libFuzzer with tokens drawn from the
transform's own literal/keyword handling in `src/strip_core.c` — not a
generic web-syntax wordlist. Verified counts (`wc -l ci/fuzz/fuzz.dict.*`):

| kind | tokens | source |
|------|:------:|--------|
| css  | 19 | property/value keywords, `@media`, comment delimiters, hex color, whitespace markers |
| html | 24 | tag/attribute tokens, raw-text element names, comment delimiters |
| js   | 18 | comment delimiters, operator/punctuation tokens, literal delimiters |
| json | 14 | structural punctuation, literal keywords (`true`/`false`/`null`) |
| svg  | 18 | element names (`<svg>`, `<rect`, `<circle`, `<path`, `<text>`, `<g>`), comment delimiters, attribute tokens |
| xml  | 15 | `<?xml`, CDATA delimiters, comment delimiters, element/attribute tokens |

## Uncovered surfaces

The fuzz targets exercise `strip_minify()` in isolation on an in-memory byte
range. They do **not** reach:

- **Content-type negotiation / kind selection** —
  `ngx_http_strip_select()`, `src/ngx_http_strip_filter_module.c:188-264`.
  The `+xml` suffix match, the `;`/space media-type-token trim, and the
  per-kind `slcf->*` enable flags are ordinary C string logic with no fuzz
  coverage; a targeted unit/fuzz target over `ngx_str_t` content-type values
  would be cheap but does not exist yet.
- **Buffer-chain accumulation and bypass logic** —
  `ngx_http_strip_body_filter()`, `src/ngx_http_strip_filter_module.c:328-420`.
  This owns the multi-buffer coalescing, the file-backed-buffer bypass
  (`n > 0 && !ngx_buf_in_memory(b)`, line 353), and the `min_size`/`max_size`
  threshold logic (lines 358-362, 420) that decide whether `strip_minify()`
  even runs. None of this chain-walking/threshold code executes inside a
  fuzz target — the harness always hands `strip_minify()` a single
  already-assembled buffer.
- **Directive / config parsing** — `strip_types`, `strip_min_size`,
  `strip_max_size`, and the per-kind `strip_css`/`strip_js`/`strip_json`/
  `strip_svg`/`strip_xml` directive handlers (the loc-conf merge/parse code
  in this same file, outside the functions above) are nginx config-time
  code, unreached by any fuzz target and not a good fuzz target itself
  (parsed once at startup, from trusted config, not attacker input).
- **NUL-byte handling above the seam** — `strip_core.h` documents the core's
  contract as a "NUL-free byte range", but libFuzzer's corpus can and does
  contain embedded NULs; whether the module guarantees NUL-free input before
  calling `strip_minify()` is not verified by these targets. Not fixed here
  (behavior change, out of scope for this card) — flagged as a follow-up
  worth a direct look.

None of the above are wired up as fuzz targets in this change; they are
recorded here as the acknowledged gap per the adoption card's "if no real
seam is reachable, record the gap" instruction.

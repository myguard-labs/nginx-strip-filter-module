/*
 * strip_core.h - ngx-independent content minifier core.
 *
 * Pure C, no nginx headers, so the same code is exercised by the libFuzzer
 * harness (fuzz/) and the runtime module (ngx_http_strip_filter_module.c).
 *
 * Each strip_* function consumes a NUL-free byte range [src, src+len) and
 * appends the minified result to a caller-grown buffer via the sink callback.
 * The core never allocates; the caller owns all memory. Output is always <=
 * input length, so a single output buffer the size of the input is sufficient.
 */

#ifndef STRIP_CORE_H
#define STRIP_CORE_H

#include <stddef.h>

/* Content kinds the core knows how to minify. */
typedef enum {
    STRIP_HTML = 0,
    STRIP_CSS,
    STRIP_JS,
    STRIP_JSON,
    STRIP_SVG,
    STRIP_XML
} strip_kind_t;

/*
 * In-place-style minifier: reads [src, len), writes to dst (which must have
 * capacity >= len), returns number of bytes written. dst and src must not
 * overlap. Output is never larger than input.
 *
 * The transform is "smart": it collapses runs of \r\n\t and spaces and strips
 * comments only where doing so cannot change rendered/parsed meaning. Regions
 * that must survive verbatim (HTML <pre>/<textarea>/<script>/<style> bodies,
 * JS/CSS/JSON string + template + regex literals) are copied through untouched.
 */
size_t strip_minify(strip_kind_t kind,
                    const unsigned char *src, size_t len,
                    unsigned char *dst);

#endif /* STRIP_CORE_H */

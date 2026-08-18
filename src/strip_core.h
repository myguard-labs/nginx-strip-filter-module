/*
 * strip_core.h - ngx-independent content minifier core.
 *
 * Pure C, no nginx headers, so the same code is exercised by the libFuzzer
 * harness (ci/fuzz/) and the runtime module (ngx_http_strip_filter_module.c).
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
 * flags bit: enable additional transforms that are NOT guaranteed to
 * preserve meaning on all valid input (lossy/aggressive minification).
 * Unset (0) is the default and is conservative: it strips only where doing
 * so cannot change rendered/parsed meaning. Callers that want the extra
 * transforms must opt in explicitly by passing STRIP_F_AGGRESSIVE.
 */
#define STRIP_F_AGGRESSIVE 0x1u

/*
 * In-place-style minifier: reads [src, len), writes to dst (which must have
 * capacity >= len), returns number of bytes written. src and dst may be the
 * same pointer; other partial overlaps are unsupported. Output is never
 * larger than input, in both modes: gating an
 * aggressive transform behind STRIP_F_AGGRESSIVE only ever emits MORE bytes
 * than the aggressive path would, and the conservative path never emits more
 * than input.
 *
 * By default (flags == 0) the transform is conservative. JavaScript is copied
 * byte-for-byte because safe minification requires a real lexer/parser. HTML,
 * SVG, and XML preserve character-data whitespace; their comments are removed
 * without synthesizing text. Passing STRIP_F_AGGRESSIVE enables the historical
 * byte-level transforms that are NOT guaranteed to preserve meaning on all
 * valid input.
 */
size_t strip_minify(strip_kind_t kind,
                    const unsigned char *src, size_t len,
                    unsigned char *dst, unsigned flags);

#endif /* STRIP_CORE_H */

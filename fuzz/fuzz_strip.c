/*
 * fuzz_strip.c - libFuzzer harness for strip_core.c
 *
 * Six fuzz targets share this file; the build selects one via -DFUZZ_KIND=N.
 * This keeps the fuzz corpus and build independent per content type while
 * reusing a single harness body.
 *
 * FUZZ_KIND values:
 *   0 = STRIP_HTML  (default)
 *   1 = STRIP_CSS
 *   2 = STRIP_JS
 *   3 = STRIP_JSON
 *   4 = STRIP_SVG
 *   5 = STRIP_XML
 */

#include "../strip_core.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef FUZZ_KIND
#define FUZZ_KIND 0
#endif

/* output buffer: strip_minify output is always <= input length */
static unsigned char *g_out = NULL;
static size_t         g_out_cap = 0;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t out;

    if (size == 0) {
        return 0;
    }

    /* grow output buffer lazily */
    if (size > g_out_cap) {
        free(g_out);
        g_out = malloc(size);
        if (!g_out) {
            return 0;
        }
        g_out_cap = size;
    }

    out = strip_minify((strip_kind_t) FUZZ_KIND, data, size, g_out);

    /* core invariant: output never exceeds input. A violation means a write
     * past g_out happened (heap overflow) — fail loudly so the fuzzer keeps
     * the reproducer. With ASan the overflow itself also traps; this assert
     * catches the off-by-one cases where the byte still lands inside g_out. */
    assert(out <= size);
    (void) out;

    return 0;
}

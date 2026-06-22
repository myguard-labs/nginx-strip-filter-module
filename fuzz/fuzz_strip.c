/*
 * fuzz_strip.c - libFuzzer harness for strip_core.c
 *
 * Four fuzz targets share this file; the build selects one via -DFUZZ_KIND=N.
 * This keeps the fuzz corpus and build independent per content type while
 * reusing a single harness body.
 *
 * FUZZ_KIND values:
 *   0 = STRIP_HTML  (default)
 *   1 = STRIP_CSS
 *   2 = STRIP_JS
 *   3 = STRIP_JSON
 */

#include "../strip_core.h"
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

    strip_minify((strip_kind_t) FUZZ_KIND, data, size, g_out);

    return 0;
}

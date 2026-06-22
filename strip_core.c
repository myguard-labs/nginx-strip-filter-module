/*
 * strip_core.c - ngx-independent content minifier core.
 *
 * See strip_core.h for the contract. Four state machines:
 *   - HTML: collapse inter-tag whitespace, strip comments, preserve the bodies
 *           of <pre> <textarea> <script> <style>.
 *   - CSS:  strip block comments, collapse whitespace, trim around { } : ; ,.
 *   - JS:   strip line and block comments, collapse newlines with ASI safety,
 *           preserve string/template/regex literals.
 *   - JSON: strip every whitespace byte outside string literals.
 *
 * Output is always <= input length, so callers size dst == input length.
 */

#include "strip_core.h"

#include <string.h>

/* ---- byte classifiers -------------------------------------------------- */

static int
sc_is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'
           || c == '\v';
}

/* JS identifier / keyword bytes — used for ASI-safe newline decisions. */
static int
sc_is_word(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9') || c == '_' || c == '$'
           || c >= 0x80; /* keep multibyte identifier bytes joined */
}

static int
sc_ci_eq(const unsigned char *p, size_t len, const char *lit)
{
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char a = p[i];
        unsigned char b = (unsigned char) lit[i];

        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char) (a + 32);
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* ---- JSON -------------------------------------------------------------- */

static size_t
strip_json(const unsigned char *src, size_t len, unsigned char *dst)
{
    size_t i;
    size_t o = 0;

    for (i = 0; i < len; i++) {
        unsigned char c = src[i];

        if (c == '"') {
            /* copy the whole string literal verbatim, honouring escapes */
            dst[o++] = c;
            for (i++; i < len; i++) {
                unsigned char d = src[i];
                dst[o++] = d;
                if (d == '\\' && i + 1 < len) {
                    dst[o++] = src[++i];
                    continue;
                }
                if (d == '"') {
                    break;
                }
            }
            continue;
        }

        if (sc_is_space(c)) {
            continue; /* all structural whitespace is redundant in JSON */
        }

        dst[o++] = c;
    }

    return o;
}

/* ---- CSS --------------------------------------------------------------- */

static size_t
strip_css(const unsigned char *src, size_t len, unsigned char *dst)
{
    size_t i;
    size_t o = 0;
    int pending_space = 0; /* a collapsed whitespace run is buffered */

    for (i = 0; i < len; i++) {
        unsigned char c = src[i];

        /* block comment */
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) {
                i++;
            }
            i++; /* lands on the trailing '/', loop ++ steps past it */
            pending_space = 1; /* comment acts as a separator */
            continue;
        }

        /* string literal — copy verbatim */
        if (c == '"' || c == '\'') {
            unsigned char q = c;
            if (pending_space && o > 0) {
                unsigned char prev = dst[o - 1];
                if (prev != ':' && prev != '{' && prev != '}' && prev != ';'
                    && prev != ',' && prev != '(' && prev != '>')
                {
                    dst[o++] = ' ';
                }
            }
            pending_space = 0;
            dst[o++] = c;
            for (i++; i < len; i++) {
                unsigned char d = src[i];
                dst[o++] = d;
                if (d == '\\' && i + 1 < len) {
                    dst[o++] = src[++i];
                    continue;
                }
                if (d == q) {
                    break;
                }
            }
            continue;
        }

        if (sc_is_space(c)) {
            pending_space = 1;
            continue;
        }

        /* a real token: flush a single space only if it is meaningful (i.e.
         * not adjacent to punctuation that doesn't need separation) */
        if (pending_space) {
            if (o > 0) {
                unsigned char prev = dst[o - 1];
                if (prev != '{' && prev != '}' && prev != ';' && prev != ':'
                    && prev != ',' && prev != '(' && prev != '>'
                    && c != '{' && c != '}' && c != ';' && c != ':'
                    && c != ',' && c != ')' && c != '>')
                {
                    dst[o++] = ' ';
                }
            }
            pending_space = 0;
        }

        dst[o++] = c;
    }

    return o;
}

/* ---- JS ---------------------------------------------------------------- */

/*
 * Decide whether the byte before a stripped newline could end a statement, in
 * which case we must keep the newline so Automatic Semicolon Insertion still
 * fires. Conservative: keep the newline whenever the surrounding tokens are
 * word/`)`/`]`/`}` on the left and word/`(`/`[`/`{`/`+`/`-`/`/` on the right.
 */
static int
js_keep_newline(unsigned char prev, unsigned char next)
{
    int left = sc_is_word(prev) || prev == ')' || prev == ']' || prev == '}'
               || prev == '"' || prev == '\'' || prev == '`';
    int right = sc_is_word(next) || next == '(' || next == '[' || next == '{'
                || next == '+' || next == '-' || next == '/' || next == '"'
                || next == '\'' || next == '`' || next == '!' || next == '~';

    return left && right;
}

/* Does the byte stream ending at dst[o-1] expect a regex (vs division) next? */
static int
js_regex_allowed(const unsigned char *dst, size_t o)
{
    size_t k = o;

    /* skip trailing spaces we already emitted */
    while (k > 0 && dst[k - 1] == ' ') {
        k--;
    }
    if (k == 0) {
        return 1;
    }

    unsigned char p = dst[k - 1];

    /* after a value/identifier/closing bracket, `/` is division */
    if (sc_is_word(p) || p == ')' || p == ']' || p == '}' || p == '"'
        || p == '\'' || p == '`')
    {
        return 0;
    }
    return 1;
}

static size_t
strip_js(const unsigned char *src, size_t len, unsigned char *dst)
{
    size_t i;
    size_t o = 0;
    int pending_nl = 0;    /* a collapsed run contained a newline */
    int pending_sp = 0;    /* a collapsed run contained spaces only */

    for (i = 0; i < len; i++) {
        unsigned char c = src[i];

        /* line comment // ... */
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            i += 2;
            while (i < len && src[i] != '\n') {
                i++;
            }
            i--;             /* let the loop see the '\n' as whitespace */
            continue;
        }

        /* block comment */
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            int had_nl = 0;
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') {
                    had_nl = 1;
                }
                i++;
            }
            i++;
            if (had_nl) {
                pending_nl = 1;
            } else {
                pending_sp = 1;
            }
            continue;
        }

        /* string / template literal — copy verbatim */
        if (c == '"' || c == '\'' || c == '`') {
            unsigned char q = c;
            if ((pending_sp || pending_nl) && o > 0
                && (sc_is_word(dst[o - 1]) || dst[o - 1] == ')'
                    || dst[o - 1] == ']'))
            {
                dst[o++] = ' ';
            }
            pending_sp = 0;
            pending_nl = 0;
            dst[o++] = c;
            for (i++; i < len; i++) {
                unsigned char d = src[i];
                dst[o++] = d;
                if (d == '\\' && i + 1 < len) {
                    dst[o++] = src[++i];
                    continue;
                }
                if (d == q) {
                    break;
                }
            }
            continue;
        }

        /* regex literal /.../ — only when a regex is grammatically allowed */
        if (c == '/' && js_regex_allowed(dst, o)) {
            int in_class = 0;
            if ((pending_sp || pending_nl) && o > 0
                && (sc_is_word(dst[o - 1]) || dst[o - 1] == ')'
                    || dst[o - 1] == ']'))
            {
                dst[o++] = ' ';
            }
            pending_sp = 0;
            pending_nl = 0;
            dst[o++] = c;
            for (i++; i < len; i++) {
                unsigned char d = src[i];
                dst[o++] = d;
                if (d == '\\' && i + 1 < len) {
                    dst[o++] = src[++i];
                    continue;
                }
                if (d == '[') {
                    in_class = 1;
                } else if (d == ']') {
                    in_class = 0;
                } else if (d == '/' && !in_class) {
                    break;
                } else if (d == '\n') {
                    break; /* unterminated; bail defensively */
                }
            }
            continue;
        }

        if (sc_is_space(c)) {
            if (c == '\n' || c == '\r') {
                pending_nl = 1;
            } else {
                pending_sp = 1;
            }
            continue;
        }

        /* flush buffered whitespace before a real token */
        if (pending_nl || pending_sp) {
            unsigned char prev = o > 0 ? dst[o - 1] : 0;

            if (pending_nl && o > 0 && js_keep_newline(prev, c)) {
                dst[o++] = '\n';
            } else if (o > 0 && (sc_is_word(prev) || prev == ')' || prev == ']')
                       && (sc_is_word(c) || c == '(' || c == '['))
            {
                /* keep a single separating space between adjacent words */
                dst[o++] = ' ';
            }
            pending_nl = 0;
            pending_sp = 0;
        }

        dst[o++] = c;
    }

    return o;
}

/* ---- HTML -------------------------------------------------------------- */

/* Copy a raw element body (<pre>/<textarea>/<script>/<style>) verbatim up to
 * and including its closing tag, returning the source index just past it. */
static size_t
html_copy_raw(const unsigned char *src, size_t len, size_t i,
              const char *close, size_t close_len,
              unsigned char *dst, size_t *op)
{
    size_t o = *op;

    while (i < len) {
        if (src[i] == '<' && i + 1 < len && src[i + 1] == '/'
            && i + 2 + close_len <= len
            && sc_ci_eq(src + i + 2, close_len, close))
        {
            /* emit the closing tag and stop */
            while (i < len) {
                dst[o++] = src[i];
                if (src[i] == '>') {
                    i++;
                    break;
                }
                i++;
            }
            break;
        }
        dst[o++] = src[i++];
    }

    *op = o;
    return i;
}

static size_t
strip_html(const unsigned char *src, size_t len, unsigned char *dst)
{
    size_t i;
    size_t o = 0;
    int pending_space = 0;

    for (i = 0; i < len; i++) {
        unsigned char c = src[i];

        /* HTML comment <!-- ... --> (but not <!DOCTYPE> or conditional IE) */
        if (c == '<' && i + 3 < len && src[i + 1] == '!' && src[i + 2] == '-'
            && src[i + 3] == '-')
        {
            i += 4;
            while (i + 2 < len
                   && !(src[i] == '-' && src[i + 1] == '-' && src[i + 2] == '>'))
            {
                i++;
            }
            i += 2; /* lands on '>', loop ++ steps past */
            pending_space = 1;
            continue;
        }

        if (c == '<') {
            /* flush at most one space if it was meaningful text before a tag */
            if (pending_space && o > 0 && dst[o - 1] != '>') {
                dst[o++] = ' ';
            }
            pending_space = 0;

            /* detect raw-text elements whose bodies must survive verbatim */
            static const struct { const char *name; size_t len; } raw[] = {
                { "pre",      3 },
                { "textarea", 8 },
                { "script",   6 },
                { "style",    5 },
            };
            size_t r;
            int matched = -1;

            if (i + 1 < len && src[i + 1] != '/') {
                for (r = 0; r < sizeof(raw) / sizeof(raw[0]); r++) {
                    size_t nl = raw[r].len;
                    if (i + 1 + nl <= len
                        && sc_ci_eq(src + i + 1, nl, raw[r].name))
                    {
                        unsigned char after = (i + 1 + nl < len)
                                                  ? src[i + 1 + nl] : 0;
                        if (after == '>' || sc_is_space(after) || after == 0) {
                            matched = (int) r;
                            break;
                        }
                    }
                }
            }

            /* copy the tag itself up to and including '>' */
            while (i < len) {
                dst[o++] = src[i];
                if (src[i] == '>') {
                    i++;
                    break;
                }
                i++;
            }
            i--; /* loop ++ will re-advance */

            if (matched >= 0) {
                size_t next = html_copy_raw(src, len, i + 1, raw[matched].name,
                                            raw[matched].len, dst, &o);
                i = next - 1;
            }
            continue;
        }

        if (sc_is_space(c)) {
            pending_space = 1;
            continue;
        }

        if (pending_space) {
            /* collapse the run to a single space between text and the next
             * non-space; drop it entirely right after a tag close */
            if (o > 0 && dst[o - 1] != '>') {
                dst[o++] = ' ';
            }
            pending_space = 0;
        }

        dst[o++] = c;
    }

    return o;
}

/* ---- dispatch ---------------------------------------------------------- */

size_t
strip_minify(strip_kind_t kind, const unsigned char *src, size_t len,
             unsigned char *dst)
{
    size_t n;
    int src_had_nl = (len > 0 && src[len - 1] == '\n');

    switch (kind) {
    case STRIP_JSON:
        return strip_json(src, len, dst);
    case STRIP_CSS:
        n = strip_css(src, len, dst);
        break;
    case STRIP_JS:
        n = strip_js(src, len, dst);
        break;
    case STRIP_HTML:
    default:
        n = strip_html(src, len, dst);
        break;
    }

    /* restore exactly one trailing newline if the source had one and strip
     * consumed it — keeps output well-formed for tools that expect it */
    if (src_had_nl && n > 0 && dst[n - 1] != '\n') {
        dst[n++] = '\n';
    }

    return n;
}

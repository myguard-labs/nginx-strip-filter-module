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
sc_is_hex(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
           || (c >= 'A' && c <= 'F');
}

static unsigned char
sc_hex_lo(unsigned char c)
{
    return (c >= 'a' && c <= 'f') ? c : (c >= 'A' && c <= 'F') ? (unsigned char)(c + 32) : c;
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

/*
 * Skip a CSS unit suffix after a literal '0' at src[i].
 * Returns the new i (pointing past the unit) or the original i if no unit.
 * Units: px em rem ex ch vw vh vmin vmax pt pc cm mm in %
 */
/*
 * Returns the index just past the unit if src[i..] starts with a CSS unit
 * that should be stripped from a zero value, or i if no unit matched.
 * Caller checks: if (ni > i) { i = ni - 1; } so loop ++ lands past unit.
 */
static size_t
css_skip_zero_unit(const unsigned char *src, size_t len, size_t i)
{
    static const char * const units[] = {
        "rem", "vmin", "vmax", "px", "em", "ex", "ch", "vw", "vh",
        "pt", "pc", "cm", "mm", "in", "%", NULL
    };
    int u;

    for (u = 0; units[u]; u++) {
        size_t ul = strlen(units[u]);
        if (i + ul <= len && sc_ci_eq(src + i, ul, units[u])) {
            unsigned char after = (i + ul < len) ? src[i + ul] : 0;
            /* unit must be followed by boundary: space, punct, or end */
            if (after == 0 || sc_is_space(after) || after == ';' || after == '}'
                || after == ')' || after == ',' || after == '!')
            {
                return i + ul; /* exclusive end: caller does i = ni - 1 */
            }
        }
    }
    return i; /* no match: caller checks ni > i */
}

/*
 * Try to shorten a 6-digit hex color #rrggbb → #rgb.
 * src[i] is the first hex digit (after '#' already emitted to dst at o-1).
 * If collapsible: overwrites dst[o-1] (the '#') with the 3-char form and
 * returns new i (past the 6th digit). Otherwise returns original i unchanged
 * and caller emits normally.
 */
static size_t
css_try_short_hex(const unsigned char *src, size_t len, size_t i,
                  unsigned char *dst, size_t *op)
{
    size_t o = *op;

    if (i + 6 > len) {
        return i;
    }

    unsigned char r0 = src[i],   r1 = src[i+1];
    unsigned char g0 = src[i+2], g1 = src[i+3];
    unsigned char b0 = src[i+4], b1 = src[i+5];

    if (!sc_is_hex(r0) || !sc_is_hex(r1) || !sc_is_hex(g0) ||
        !sc_is_hex(g1) || !sc_is_hex(b0) || !sc_is_hex(b1))
    {
        return i;
    }

    /* pairs must match (case-insensitive) */
    if (sc_hex_lo(r0) != sc_hex_lo(r1) || sc_hex_lo(g0) != sc_hex_lo(g1)
        || sc_hex_lo(b0) != sc_hex_lo(b1))
    {
        return i;
    }

    /* must be followed by a non-hex character (or end) to be a color token */
    unsigned char after = (i + 6 < len) ? src[i + 6] : 0;
    if (after != 0 && sc_is_hex(after)) {
        return i;
    }

    /* emit #rgb (lowercase) — the '#' is already at dst[o-1] */
    dst[o++] = sc_hex_lo(r0);
    dst[o++] = sc_hex_lo(g0);
    dst[o++] = sc_hex_lo(b0);
    *op = o;
    return i + 6; /* skip the 6 original digits; loop ++ not needed here */
}

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

        /* url(...) — copy the whole token verbatim. Unquoted url() content is
         * NOT whitespace-collapsible or zero-trimmable, so guard it here. A
         * quoted argument (url("...")) is handled by the inner quote copy. */
        if ((c == 'u' || c == 'U') && i + 3 < len
            && sc_ci_eq(src + i, 3, "url") && src[i + 3] == '(')
        {
            /* url(...) is an ordinary CSS token; a collapsed whitespace run in
             * front of it must survive as a single space when it separates two
             * value tokens (`background:red url(x)` must NOT become
             * `background:redurl(x)`). Apply the same boundary rule as the
             * generic token path: drop the space only next to punctuation that
             * doesn't need a separator. */
            if (pending_space) {
                if (o > 0) {
                    unsigned char prev = dst[o - 1];
                    if (prev != '{' && prev != '}' && prev != ';' && prev != ':'
                        && prev != ',' && prev != '(' && prev != '>')
                    {
                        dst[o++] = ' ';
                    }
                }
                pending_space = 0;
            }
            dst[o++] = src[i];     /* u */
            dst[o++] = src[i + 1]; /* r */
            dst[o++] = src[i + 2]; /* l */
            dst[o++] = '(';
            for (i += 4; i < len; i++) {
                unsigned char d = src[i];
                if (d == '"' || d == '\'') {
                    unsigned char q = d;
                    dst[o++] = d;
                    for (i++; i < len; i++) {
                        unsigned char e = src[i];
                        dst[o++] = e;
                        if (e == '\\' && i + 1 < len) {
                            dst[o++] = src[++i];
                            continue;
                        }
                        if (e == q) {
                            break;
                        }
                    }
                    continue;
                }
                dst[o++] = d;
                if (d == ')') {
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

        /* #rrggbb → #rgb */
        if (c == '#') {
            dst[o++] = c;
            size_t ni = css_try_short_hex(src, len, i + 1, dst, &o);
            if (ni != i + 1) {
                i = ni - 1; /* -1: loop ++ advances past last consumed char */
                continue;
            }
            continue;
        }

        /* drop a redundant ';' right before '}': {a:b;} → {a:b} */
        if (c == '}' && o > 0 && dst[o - 1] == ';') {
            dst[o - 1] = '}';
            continue;
        }

        /* leading-zero strip: 0.5 → .5. Only when the '0' is immediately
         * followed by '.' and the char before it is not part of a number or
         * identifier (so 10.5 and a0.5 are untouched). */
        if (c == '0' && i + 1 < len && src[i + 1] == '.') {
            unsigned char prev = (o > 0) ? dst[o - 1] : 0;
            if (!((prev >= '0' && prev <= '9')
                  || (prev >= 'a' && prev <= 'z')
                  || (prev >= 'A' && prev <= 'Z')
                  || prev == '.' || prev == '_' || prev == '-'))
            {
                continue; /* skip emitting the '0'; '.' emitted next iter */
            }
        }

        dst[o++] = c;

        /* 0<unit> → 0: only when the '0' is a standalone numeric zero.
         * Check that the char before the '0' is not a digit (to avoid
         * trimming units from e.g. 10px, 20em). */
        if (c == '0') {
            unsigned char prev2 = (o >= 2) ? dst[o - 2] : 0;
            if ((prev2 < '0' || prev2 > '9') && prev2 != '.') {
                size_t ni = css_skip_zero_unit(src, len, i + 1);
                if (ni > i + 1) {
                    i = ni - 1; /* loop ++ will step past last unit char */
                }
            }
        }
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

/*
 * Keywords after which a `/` begins a regex literal, not a division. After
 * `return /re/.test(x)` the slash is a regex; a plain identifier (`a/b`) is
 * division. We can only tell the two apart by looking at the whole trailing
 * word, not its last byte.
 */
static int
js_word_allows_regex(const unsigned char *w, size_t n)
{
    static const char * const kw[] = {
        "return", "typeof", "instanceof", "in", "of", "new", "delete",
        "void", "do", "else", "yield", "await", "case", "throw", "default",
        NULL
    };
    int k;

    for (k = 0; kw[k]; k++) {
        if (strlen(kw[k]) == n && memcmp(w, kw[k], n) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Flush a pending collapsed-whitespace run before a JS token `c` is emitted.
 * Used before string/template/regex literals so they get the SAME ASI-safe
 * treatment as the generic token path: if the run contained a newline and the
 * surrounding tokens require it for Automatic Semicolon Insertion, a real '\n'
 * is kept (e.g. `return\n"x"` must NOT become `return "x"`); otherwise a single
 * separating space is emitted only between adjacent word/`)`/`]` and the token.
 * Returns the new output index. Never expands beyond the consumed whitespace.
 */
static size_t
js_flush_pending(unsigned char *dst, size_t o, int pending_nl, int pending_sp,
                 unsigned char c)
{
    unsigned char prev;

    if (!(pending_nl || pending_sp) || o == 0) {
        return o;
    }

    prev = dst[o - 1];

    if (pending_nl && js_keep_newline(prev, c)) {
        dst[o++] = '\n';
    } else if (sc_is_word(prev) || prev == ')' || prev == ']') {
        dst[o++] = ' ';
    }
    return o;
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

    /* after a value/closing bracket, `/` is division. After a bare word it is
     * usually division (a/b) — UNLESS the word is a keyword like `return` or
     * `typeof`, after which a regex is expected. */
    if (sc_is_word(p)) {
        size_t e = k;
        while (k > 0 && sc_is_word(dst[k - 1])) {
            k--;
        }
        /* dst[k..e) is the trailing identifier/keyword; reject if the byte
         * before it is part of a member access (foo.return is a property). */
        if (k > 0 && dst[k - 1] == '.') {
            return 0;
        }
        return js_word_allows_regex(dst + k, e - k);
    }

    if (p == ')' || p == ']' || p == '}' || p == '"' || p == '\'' || p == '`')
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
            o = js_flush_pending(dst, o, pending_nl, pending_sp, c);
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
            o = js_flush_pending(dst, o, pending_nl, pending_sp, c);
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

/*
 * True if `name` (length n, case-insensitive) is one of the HTML5 boolean
 * attributes whose presence alone is meaningful, so `attr="attr"` can be
 * collapsed to `attr` without changing semantics. A generic value==name
 * match is NOT safe: e.g. id="id", class="class", title="title" are ordinary
 * string attributes whose value must be preserved.
 */
static int
html_is_boolean_attr(const unsigned char *name, size_t n)
{
    static const char * const battrs[] = {
        "allowfullscreen", "async", "autofocus", "autoplay", "checked",
        "controls", "default", "defer", "disabled", "formnovalidate",
        "hidden", "ismap", "itemscope", "loop", "multiple", "muted",
        "nomodule", "novalidate", "open", "playsinline", "readonly",
        "required", "reversed", "selected", NULL
    };
    int k;

    for (k = 0; battrs[k]; k++) {
        if (strlen(battrs[k]) == n && sc_ci_eq(name, n, battrs[k])) {
            return 1;
        }
    }
    return 0;
}

/*
 * Copy a tag from src[i] (pointing at '<') up to and including '>',
 * collapsing boolean attributes of the form attr="attr" or attr='attr'
 * to just attr. Returns new i (past '>').
 *
 * We parse at byte level: tag name, then attr tokens.
 * We only collapse when the quoted value is an exact case-sensitive match
 * of the attribute name AND the name is an HTML5 boolean attribute.
 */
static size_t
html_copy_tag(const unsigned char *src, size_t len, size_t i,
              unsigned char *dst, size_t *op, int allow_unquote)
{
    size_t o = *op;

    /* emit '<' */
    dst[o++] = src[i++];

    /* copy tag name (and optional '/' for closing tags) verbatim */
    while (i < len && !sc_is_space(src[i]) && src[i] != '>' && src[i] != '/') {
        dst[o++] = src[i++];
    }

    /* process attributes until '>' */
    while (i < len && src[i] != '>') {
        unsigned char c = src[i];

        if (sc_is_space(c)) {
            dst[o++] = c;
            i++;
            continue;
        }

        /* self-closing slash */
        if (c == '/') {
            dst[o++] = c;
            i++;
            continue;
        }

        /* attribute name */
        size_t name_start = o;
        while (i < len && src[i] != '=' && src[i] != '>' && !sc_is_space(src[i])) {
            dst[o++] = src[i++];
        }
        size_t name_len = o - name_start;

        if (i >= len || src[i] != '=') {
            /* bare attribute (already a boolean form) — done */
            continue;
        }

        /* consume '=' */
        i++;

        if (i >= len) {
            dst[o++] = '=';
            break;
        }

        unsigned char q = src[i];
        if (q != '"' && q != '\'') {
            /* unquoted value — copy verbatim */
            dst[o++] = '=';
            while (i < len && !sc_is_space(src[i]) && src[i] != '>') {
                dst[o++] = src[i++];
            }
            continue;
        }

        /* quoted value: check if value == name */
        i++; /* skip opening quote */
        size_t val_start = i;
        while (i < len && src[i] != q) {
            i++;
        }
        size_t val_len = i - val_start;
        int    had_close = (i < len);   /* a closing quote was actually seen */
        if (had_close) {
            i++; /* skip closing quote */
        }

        /* Unterminated quoted value (input ran out before the closing quote):
         * emit the bytes verbatim, opening quote included, NO synthesized
         * closing quote. Synthesizing one would make the output longer than
         * the input and break the output<=input invariant the caller relies
         * on for buffer sizing (heap overflow). */
        if (!had_close) {
            size_t v;
            dst[o++] = '=';
            dst[o++] = q;
            for (v = val_start; v < val_start + val_len; v++) {
                dst[o++] = src[v];
            }
            break; /* reached end of input mid-attribute */
        }

        /* Collapse attr="attr" → attr only for genuine HTML5 boolean
         * attributes (allow_unquote gates HTML vs XML/SVG; XML/SVG have no
         * boolean-attr concept and must keep the value). id="id" / class=
         * "class" are NOT boolean and must survive. */
        if (allow_unquote
            && val_len == name_len
            && memcmp(src + val_start, dst + name_start, name_len) == 0
            && html_is_boolean_attr(dst + name_start, name_len))
        {
            /* boolean attr — already emitted the name, skip ="value" */
            continue;
        }

        /* A value can drop its quotes (HTML5 unquoted-attr syntax) only when
         * it is non-empty and contains none of the bytes that would end or
         * mis-parse an unquoted value: whitespace, quote, backtick, < > =.
         * Otherwise emit it quoted verbatim. */
        size_t v;
        /* Unquoting is an HTML-only transform: XML/SVG syntax requires every
         * attribute value to stay quoted. */
        int can_unquote = allow_unquote && (val_len > 0);
        /* An unquoted value ends only at whitespace or '>'. The closing quote
         * in the source may be directly followed by another attribute
         * (href="x"id=y) or a self-closing slash (src="a"/>); dropping the
         * quotes there would merge tokens (href=xid=y) or absorb the slash
         * (src=a/). Only unquote when the next byte is a clean boundary:
         * whitespace or '>'. src[i] is the byte past the closing quote. */
        if (can_unquote
            && !(i >= len || sc_is_space(src[i]) || src[i] == '>'))
        {
            can_unquote = 0;
        }
        for (v = val_start; can_unquote && v < val_start + val_len; v++) {
            unsigned char vc = src[v];
            if (sc_is_space(vc) || vc == '"' || vc == '\'' || vc == '`'
                || vc == '<' || vc == '>' || vc == '=')
            {
                can_unquote = 0;
            }
        }

        dst[o++] = '=';
        if (!can_unquote) {
            dst[o++] = q;
        }
        for (v = val_start; v < val_start + val_len; v++) {
            dst[o++] = src[v];
        }
        if (!can_unquote) {
            dst[o++] = q;
        }
    }

    /* emit closing '>' */
    if (i < len && src[i] == '>') {
        dst[o++] = src[i++];
    }

    *op = o;
    return i;
}

/* Copy a raw element body (<pre>/<textarea>/<script>/<style>) verbatim up to
 * and including its closing tag, returning the source index just past it. */
static size_t
html_copy_raw(const unsigned char *src, size_t len, size_t i,
              const char *close, size_t close_len,
              unsigned char *dst, size_t *op)
{
    size_t o = *op;

    while (i < len) {
        /* A real end tag requires a name boundary after the element name:
         * '>', whitespace, '/', or end of input. Without this, raw text that
         * merely starts with the name (</scriptx>, </styled>) would falsely
         * close the raw element and the rest would be minified as HTML. */
        if (src[i] == '<' && i + 1 < len && src[i + 1] == '/'
            && i + 2 + close_len <= len
            && sc_ci_eq(src + i + 2, close_len, close)
            && (i + 2 + close_len == len
                || src[i + 2 + close_len] == '>'
                || src[i + 2 + close_len] == '/'
                || sc_is_space(src[i + 2 + close_len])))
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

            /* copy the tag, collapsing boolean attrs and unquoting safe
             * values (HTML allows unquoted attribute values) */
            size_t next_i = html_copy_tag(src, len, i, dst, &o, 1);
            i = next_i - 1; /* loop ++ will re-advance */

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
            /* Collapse the run to a single space. Whitespace immediately after
             * a tag close ('>') is only insignificant when the next thing is
             * another tag ('<') — i.e. pure inter-tag whitespace. When text
             * follows ("</span> world"), the space is meaningful and must be
             * kept, otherwise inline elements get glued to the next word. */
            if (o > 0 && !(dst[o - 1] == '>' && c == '<')) {
                dst[o++] = ' ';
            }
            pending_space = 0;
        }

        dst[o++] = c;
    }

    return o;
}

/* ---- SVG --------------------------------------------------------------- */

/*
 * SVG/XML minifier: strip <!-- comments -->, pass <![CDATA[...]]> verbatim,
 * collapse inter-tag whitespace. Structurally identical to strip_html but
 * without raw-text element handling (SVG has no <pre>/<script> semantics at
 * the filter layer) and with CDATA passthrough.
 */
static size_t
strip_svg(const unsigned char *src, size_t len, unsigned char *dst)
{
    size_t i;
    size_t o = 0;
    int pending_space = 0;

    for (i = 0; i < len; i++) {
        unsigned char c = src[i];

        if (c == '<') {
            if (pending_space && o > 0 && dst[o - 1] != '>') {
                dst[o++] = ' ';
            }
            pending_space = 0;

            /* XML comment <!-- ... --> */
            if (i + 3 < len && src[i + 1] == '!' && src[i + 2] == '-'
                && src[i + 3] == '-')
            {
                i += 4;
                while (i + 2 < len
                       && !(src[i] == '-' && src[i + 1] == '-'
                            && src[i + 2] == '>'))
                {
                    i++;
                }
                i += 2; /* lands on '>', loop ++ steps past */
                pending_space = 1;
                continue;
            }

            /* CDATA section <![CDATA[...]]> — copy verbatim */
            if (i + 8 < len && src[i + 1] == '!' && src[i + 2] == '['
                && src[i + 3] == 'C' && src[i + 4] == 'D'
                && src[i + 5] == 'A' && src[i + 6] == 'T'
                && src[i + 7] == 'A' && src[i + 8] == '[')
            {
                while (i < len) {
                    dst[o++] = src[i];
                    if (src[i] == ']' && i + 2 < len
                        && src[i + 1] == ']' && src[i + 2] == '>')
                    {
                        dst[o++] = src[i + 1];
                        dst[o++] = src[i + 2];
                        i += 3;
                        break;
                    }
                    i++;
                }
                i--; /* loop ++ */
                continue;
            }

            /* regular tag: copy using html_copy_tag for boolean attr collapse.
             * allow_unquote=0 — XML/SVG attribute values must stay quoted. */
            size_t next_i = html_copy_tag(src, len, i, dst, &o, 0);
            i = next_i - 1;
            continue;
        }

        if (sc_is_space(c)) {
            pending_space = 1;
            continue;
        }

        if (pending_space) {
            /* Same rule as HTML: only inter-tag whitespace ('>' … '<') is
             * safe to drop. XML/SVG text nodes are whitespace-sensitive, so a
             * space between '>' and text content is preserved. */
            if (o > 0 && !(dst[o - 1] == '>' && c == '<')) {
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
    case STRIP_SVG:
    case STRIP_XML:
        /* SVG and generic XML (RSS/Atom/sitemap) share one minifier: strip
         * comments, pass CDATA verbatim, collapse inter-tag whitespace. */
        n = strip_svg(src, len, dst);
        break;
    case STRIP_HTML:
    default:
        n = strip_html(src, len, dst);
        break;
    }

    if (src_had_nl && n > 0 && dst[n - 1] != '\n') {
        dst[n++] = '\n';
    }

    return n;
}

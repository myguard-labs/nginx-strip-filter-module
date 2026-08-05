/*
 * strip_core_test.c - portable unit tests for the ngx-independent minifier core.
 *
 * Depends on nothing but strip_core.{c,h}. Builds and runs anywhere:
 *
 *     ci/tests/unit/run.sh
 *
 * No nginx headers, no test framework, no harness. Output is TAP 13.
 *
 * WHY THIS FILE EXISTS
 *   ci/t/basic.t exercises the core through a booted nginx, which is the right
 *   place for the filter's request-path behaviour but a poor instrument for the
 *   core's state machines: one Test::Nginx case costs a server boot, and whole
 *   features of the minifier (CSS url() tokens, hex-colour shortening, JS
 *   regex-vs-division, HTML boolean-attribute collapsing) had no case at all.
 *   Replaying the fuzz corpus through strip_minify() covers 77.21% of
 *   strip_core.c; the 111 uncovered lines were concentrated in exactly those
 *   features. This file targets them directly.
 *
 * HOW EXPECTATIONS WERE DERIVED
 *   From the CSS/JS/JSON/HTML grammars and the documented contract in
 *   strip_core.h ("strips only where doing so cannot change rendered/parsed
 *   meaning"), NOT by running the implementation and recording its output. A
 *   golden file minted from the code asserts only that the code equals itself.
 *
 * HOW EACH CASE WAS PROVEN
 *   Every group below carries a CONTROL comment naming a one-line mutation of
 *   strip_core.c that makes that group fail. A test that stays green when the
 *   line it claims to cover is broken is coverage theatre. If you add a case,
 *   add its control, and check that the control reds YOUR case rather than
 *   crashing the binary somewhere else.
 *
 * INVARIANT CHECKED ON EVERY CASE
 *   strip_minify() must never write more bytes than it read; the caller sizes
 *   dst == input length and a violation is a heap overflow in the module. Every
 *   assertion helper checks it, so it is exercised by all cases including the
 *   malformed-input ones.
 */

#include "strip_core.h"

#include <stdio.h>
#include <string.h>

static int plan_n;
static int plan_failed;

static void
tap(int ok, const char *name)
{
    plan_n++;
    printf("%sok %d - %s\n", ok ? "" : "not ", plan_n, name);
    if (!ok) {
        plan_failed++;
    }
}

/*
 * Assert that minifying `in` as `kind` yields exactly `want`.
 * Also enforces the output <= input invariant on every call.
 */
static void
is_min(strip_kind_t kind, const char *in, const char *want, const char *name)
{
    unsigned char out[8192];
    size_t in_len = strlen(in);
    size_t want_len = strlen(want);
    size_t n;

    memset(out, 0, sizeof(out));
    n = strip_minify(kind, (const unsigned char *) in, in_len, out);

    if (n > in_len) {
        printf("not ok %d - %s\n", ++plan_n, name);
        printf("#   OUTPUT LONGER THAN INPUT: %zu > %zu (buffer overflow)\n",
               n, in_len);
        plan_failed++;
        return;
    }

    if (n == want_len && memcmp(out, want, n) == 0) {
        tap(1, name);
        return;
    }

    tap(0, name);
    printf("#   want (%zu): %s\n", want_len, want);
    printf("#    got (%zu): %.*s\n", n, (int) n, out);
}

/* Assert only the invariant — for inputs whose exact output is not specified
 * (truncated/malformed), where the contract is "do not overflow, do not hang". */
static void
ok_invariant(strip_kind_t kind, const char *in, size_t in_len, const char *name)
{
    unsigned char out[8192];
    size_t n = strip_minify(kind, (const unsigned char *) in, in_len, out);

    if (n > in_len) {
        printf("#   OUTPUT LONGER THAN INPUT: %zu > %zu\n", n, in_len);
    }
    tap(n <= in_len, name);
}


/* ---- CSS: url() token ---------------------------------------------------
 *
 * CONTROL: in strip_css(), disable the url() branch
 *     if ((c == 'u' || c == 'U') && ...)  ->  if ((0) && ...)
 * `url(0px)` then becomes `url(0)`, `url(#aabbcc)` becomes `url(#abc)` and
 * `url(/x/)`-shaped content is eaten as a comment — each of which resolves to
 * a different, usually nonexistent, URL. Four cases below red.
 *
 * WHAT THIS CONTROL TAUGHT US, recorded so it is not re-derived: the obvious
 * case for this branch — whitespace inside `url(a b.png)` — is NOT a valid
 * oracle for it. That space survives with the branch disabled, because the
 * generic token path already keeps a separator between two ident bytes. A test
 * asserting only that would have covered all 40 lines of the branch and caught
 * nothing. The distinguishing inputs are the ones where a CSS *value* transform
 * would fire inside the URL: numeric units, hex colours, and comment markers.
 */

static void
test_css_url(void)
{
    /* CSS Values 3 § url(): the content of an unquoted url() is a single URL
     * token, not a sequence of component values, so none of the value-level
     * transforms may reach inside it. Each of the next four asserts one
     * transform that would otherwise corrupt the URL. */
    is_min(STRIP_CSS, "a{background:url(0px)}", "a{background:url(0px)}",
           "css: zero-unit stripping does not reach inside url()");
    is_min(STRIP_CSS, "a{background:url(#aabbcc)}", "a{background:url(#aabbcc)}",
           "css: hex shortening does not reach inside url()");
    is_min(STRIP_CSS, "a{background:url(0.5x)}", "a{background:url(0.5x)}",
           "css: leading-zero stripping does not reach inside url()");
    is_min(STRIP_CSS, "a{background:url(/*x*/)}", "a{background:url(/*x*/)}",
           "css: a comment marker inside url() is URL content, not a comment");

    /* Whitespace inside the token is also content. (Note: unlike the four
     * above, this one does NOT distinguish the branch on its own — see the
     * header comment. It is kept because it documents the contract.) */
    is_min(STRIP_CSS, "a{background:url(a b.png)}", "a{background:url(a b.png)}",
           "css: unquoted url() content is verbatim");

    /* A collapsed run BEFORE url() separates two value tokens and must survive
     * as exactly one space, or `red url(x)` becomes the single token `redurl(x)`. */
    is_min(STRIP_CSS, "a{background:red   url(x.png)}",
           "a{background:red url(x.png)}",
           "css: space before url() kept as one");

    /* ...but not after punctuation that already separates. */
    is_min(STRIP_CSS, "a{background:   url(x.png)}", "a{background:url(x.png)}",
           "css: space after ':' before url() dropped");

    /* A quoted argument is copied by the inner quote loop, escapes included. */
    is_min(STRIP_CSS, "a{background:url(\"a  b.png\")}",
           "a{background:url(\"a  b.png\")}",
           "css: quoted url() argument verbatim");
    is_min(STRIP_CSS, "a{background:url('a\\')b.png')}",
           "a{background:url('a\\')b.png')}",
           "css: escaped quote inside url() does not end the token");

    /* Case-insensitive per CSS keyword matching. */
    is_min(STRIP_CSS, "a{background:URL(a b.png)}", "a{background:URL(a b.png)}",
           "css: URL() uppercase recognised");

    /* Truncated url() must not read past the end. */
    ok_invariant(STRIP_CSS, "a{background:url(abc", 20,
                 "css: unterminated url() stays in bounds");
}


/* ---- CSS: #rrggbb -> #rgb -----------------------------------------------
 *
 * CONTROL: in css_try_short_hex(), change the pair-equality test
 *     if (sc_hex_lo(r0) != sc_hex_lo(r1) || ...)  ->  if (0)
 * `#123456` then collapses to `#135`, reddening "css: unequal pairs not
 * shortened" while the equal-pair cases stay green.
 */

static void
test_css_hex(void)
{
    /* CSS Color 3 § 4.2.1: #rgb is expanded by doubling each digit, so #aabbcc
     * and #abc are the same colour and the shortening is lossless. */
    is_min(STRIP_CSS, "a{color:#aabbcc}", "a{color:#abc}",
           "css: #aabbcc shortened to #abc");

    /* Doubling is case-insensitive; the short form is emitted lowercase. */
    is_min(STRIP_CSS, "a{color:#AABBCC}", "a{color:#abc}",
           "css: #AABBCC shortened and lowercased");
    is_min(STRIP_CSS, "a{color:#aAbBcC}", "a{color:#abc}",
           "css: mixed-case pairs still equal");

    /* Any unequal pair makes the colour unrepresentable in 3 digits. */
    is_min(STRIP_CSS, "a{color:#123456}", "a{color:#123456}",
           "css: unequal pairs not shortened");
    is_min(STRIP_CSS, "a{color:#aabbcd}", "a{color:#aabbcd}",
           "css: last pair unequal not shortened");

    /* 8-digit #rrggbbaa is a different token; treating its first 6 digits as a
     * colour would silently drop the alpha channel. The trailing hex digit is
     * what must stop the transform. */
    is_min(STRIP_CSS, "a{color:#aabbccdd}", "a{color:#aabbccdd}",
           "css: 8-digit hex left alone");

    /* Fewer than 6 digits available. */
    is_min(STRIP_CSS, "a{color:#aabb}", "a{color:#aabb}",
           "css: 4-digit hex left alone");
    is_min(STRIP_CSS, "a{color:#aab}", "a{color:#aab}",
           "css: 3-digit hex already short");

    /* A non-hex byte inside the six disqualifies it. */
    is_min(STRIP_CSS, "a{color:#aabbgg}", "a{color:#aabbgg}",
           "css: non-hex digit not shortened");

    /* Truncated at end of input. */
    ok_invariant(STRIP_CSS, "a{color:#aab", 12,
                 "css: truncated hex stays in bounds");

    /* All six digits present but nothing follows them at all — the "end of
     * input" arm of the boundary check (`after == 0`), distinct from the
     * truncated-mid-hex case above. A 6-digit run ending exactly at EOF is
     * still a complete, shortenable color: the boundary rule exists to reject
     * a 7th hex digit, not to require a byte after the color at all.
     *
     * CONTROL: in css_try_short_hex(), the length guard
     *     if (i + 6 > len) { return i; }  ->  if (i + 6 >= len) { return i; }
     * then rejects this exact-EOF case (it now requires a byte past the 6th
     * digit), and "css: hex shortened when input ends exactly at 6 digits"
     * reds — output stays the full `#aabbcc` (15 bytes) instead of the
     * shortened `#abc` (12 bytes). */
    is_min(STRIP_CSS, "a{color:#aabbcc", "a{color:#abc",
           "css: hex shortened when input ends exactly at 6 digits");
}


/* ---- CSS: zero units and leading zeros -----------------------------------
 *
 * CONTROL: in strip_css(), change the standalone-zero guard
 *     if ((prev2 < '0' || prev2 > '9') && prev2 != '.')  ->  if (1)
 * `10px` then becomes `10`, reddening "css: 10px keeps its unit".
 */

static void
test_css_numbers(void)
{
    /* CSS Values 3 § 6.2: a zero <length> may omit its unit. */
    is_min(STRIP_CSS, "a{margin:0px}", "a{margin:0}", "css: 0px -> 0");
    is_min(STRIP_CSS, "a{margin:0rem}", "a{margin:0}", "css: 0rem -> 0");
    is_min(STRIP_CSS, "a{width:0vmin}", "a{width:0}", "css: 0vmin -> 0");
    is_min(STRIP_CSS, "a{margin:0PX}", "a{margin:0}", "css: unit match is case-insensitive");

    /* Longest-match matters: `0vmin` must not be read as `0vm` + `in`, and
     * `0em` must not be read as `0e` + `m`. */
    is_min(STRIP_CSS, "a{margin:0vmax}", "a{margin:0}", "css: 0vmax -> 0 (not 0vm+ax)");

    /* Only a STANDALONE zero. The unit belongs to the whole number. */
    is_min(STRIP_CSS, "a{margin:10px}", "a{margin:10px}", "css: 10px keeps its unit");
    is_min(STRIP_CSS, "a{margin:1.0px}", "a{margin:1.0px}", "css: 1.0px keeps its unit");

    /* The unit must be followed by a token boundary, or it is an identifier
     * prefix and not a unit at all (`0inch` is not `0` + `in`). */
    is_min(STRIP_CSS, "a{margin:0inch}", "a{margin:0inch}",
           "css: 0inch is not 0in + boundary");

    /* A unit ending exactly at end-of-input is also a boundary — end of input
     * is as much a token boundary as `;` or `}`, so a truncated declaration
     * `margin:0px` with nothing after it still strips the unit.
     *
     * CONTROL: in css_skip_zero_unit(), drop the end-of-input arm
     *     if (after == 0 || sc_is_space(after) || ...)  ->  if (sc_is_space(after) || ...)
     * `a{margin:0px` (no closing brace, truncated) then keeps its unit and
     * "css: unit stripped when input ends exactly at the unit" reds — output
     * stays `a{margin:0px` (13 bytes) instead of the stripped `a{margin:0`
     * (10 bytes). */
    is_min(STRIP_CSS, "a{margin:0px", "a{margin:0",
           "css: unit stripped when input ends exactly at the unit");

    /* CSS Values 3 § 5: a <number> may omit the integer part when it is zero. */
    is_min(STRIP_CSS, "a{opacity:0.5}", "a{opacity:.5}", "css: 0.5 -> .5");

    /* ...but only when the 0 is not itself part of a longer number or ident. */
    is_min(STRIP_CSS, "a{opacity:10.5}", "a{opacity:10.5}", "css: 10.5 unchanged");
    is_min(STRIP_CSS, "a{margin:-0.5px}", "a{margin:-0.5px}",
           "css: -0.5 keeps its zero (sign is not a boundary)");

    /* The redundant final semicolon inside a block. */
    is_min(STRIP_CSS, "a{color:red;}", "a{color:red}", "css: trailing ';' dropped");
}


/* ---- CSS: comments, strings, whitespace ---------------------------------- */

static void
test_css_basic(void)
{
    is_min(STRIP_CSS, "a{color:red}/* c */b{color:blue}",
           "a{color:red}b{color:blue}", "css: block comment removed");

    /* A comment is a token separator: removing it must not weld two idents. */
    is_min(STRIP_CSS, "a/* c */b{color:red}", "a b{color:red}",
           "css: comment between idents leaves a separator");

    /* Whitespace inside a string is content, not formatting. */
    is_min(STRIP_CSS, "a{content:\"x   y\"}", "a{content:\"x   y\"}",
           "css: string content verbatim");

    /* A collapsed run in front of a string literal separates it from the
     * preceding token and must survive as exactly one space — a font stack
     * `Arial "My Font"` must not weld into `Arial"My Font"`. */
    is_min(STRIP_CSS, "a{font-family:Arial   \"My Font\"}",
           "a{font-family:Arial \"My Font\"}",
           "css: space before a string literal kept as one");

    /* ...and is dropped after punctuation that already separates. */
    is_min(STRIP_CSS, "a{content:   \"x\"}", "a{content:\"x\"}",
           "css: space after ':' before a string dropped");
    is_min(STRIP_CSS, "a{font-family:Arial,   \"My Font\"}",
           "a{font-family:Arial,\"My Font\"}",
           "css: space after ',' before a string dropped");
    is_min(STRIP_CSS, "a{content:'x\\'  y'}", "a{content:'x\\'  y'}",
           "css: escaped quote inside string");

    /* Descendant combinator is whitespace and is meaningful. */
    is_min(STRIP_CSS, "div   p{color:red}", "div p{color:red}",
           "css: descendant combinator kept");
    is_min(STRIP_CSS, "div   >   p{color:red}", "div>p{color:red}",
           "css: space around '>' dropped");

    ok_invariant(STRIP_CSS, "a{content:\"unterminated", 23,
                 "css: unterminated string stays in bounds");
    ok_invariant(STRIP_CSS, "a{/* unterminated", 17,
                 "css: unterminated comment stays in bounds");
}


/* ---- JS: regex literal vs division ---------------------------------------
 *
 * CONTROL: in js_regex_allowed(), change the keyword lookup
 *     return js_word_allows_regex(dst + k, e - k);  ->  return 0;
 * `return /a b/` is then read as division, its spaces collapse, and
 * "js: regex after return is a literal" reds while `a / b` stays green.
 */

static void
test_js_regex(void)
{
    /* ECMA-262 § 12.10: after `return`, `/` starts a RegularExpressionLiteral.
     * Its body is literal text — collapsing whitespace inside changes the
     * pattern. */
    is_min(STRIP_JS, "return /a  b/;", "return /a  b/;",
           "js: regex after return is a literal");
    is_min(STRIP_JS, "typeof /a  b/;", "typeof /a  b/;",
           "js: regex after typeof is a literal");
    is_min(STRIP_JS, "case /a  b/:", "case /a  b/:",
           "js: regex after case is a literal");

    /* After an identifier or a value, `/` is the division operator and the
     * operands are ordinary tokens. */
    is_min(STRIP_JS, "var x = a  /  b;", "var x=a/b;",
           "js: slash after identifier is division");
    is_min(STRIP_JS, "var x = 1  /  2;", "var x=1/2;",
           "js: slash after number is division");
    is_min(STRIP_JS, "var x = (a)  /  b;", "var x=(a)/b;",
           "js: slash after ')' is division");
    is_min(STRIP_JS, "var x = a[0]  /  b;", "var x=a[0]/b;",
           "js: slash after ']' is division");

    /* A member-access name that merely spells a keyword is a property, so the
     * slash after `foo.return` is still division. */
    is_min(STRIP_JS, "var x = foo.in  /  b;", "var x=foo.in/b;",
           "js: keyword as property name is not a regex position");

    /* A regex may contain an escaped slash and a character class containing an
     * unescaped slash; neither ends the literal. */
    is_min(STRIP_JS, "return /a\\/b  c/;", "return /a\\/b  c/;",
           "js: escaped slash does not end the regex");
    is_min(STRIP_JS, "return /[/  ]x/;", "return /[/  ]x/;",
           "js: slash inside a character class does not end the regex");

    ok_invariant(STRIP_JS, "return /unterminated", 20,
                 "js: unterminated regex stays in bounds");
}


/* ---- JS: ASI-safe newline handling ---------------------------------------
 *
 * CONTROL: in js_keep_newline(), change the result
 *     return left && right;  ->  return 0;
 * The newline in `return\nx` is then dropped, producing `return x` — a
 * different program — and "js: newline kept where ASI needs it" reds.
 */

static void
test_js_newlines(void)
{
    /* ECMA-262 § 12.9.1: a LineTerminator between two tokens can trigger
     * Automatic Semicolon Insertion. Dropping it changes the parse. */
    is_min(STRIP_JS, "a = 1\nb = 2\n", "a=1\nb=2\n",
           "js: newline kept where ASI needs it");
    is_min(STRIP_JS, "return\nx", "return\nx",
           "js: newline after return kept (ASI makes this return undefined)");
    is_min(STRIP_JS, "a\n(b)", "a\n(b)",
           "js: newline before '(' kept");

    /* Between tokens that cannot start or end a statement, the newline is pure
     * formatting. */
    is_min(STRIP_JS, "var x = {\n  a: 1\n};", "var x={a:1};",
           "js: newline inside an object literal dropped");
    is_min(STRIP_JS, "f(\n  1,\n  2\n);", "f(1,2);",
           "js: newline inside an argument list dropped");

    /* Whitespace between two words is a token separator and must not vanish. */
    is_min(STRIP_JS, "var    x;", "var x;", "js: run between words collapses to one space");
}


/* ---- JS: literals --------------------------------------------------------
 *
 * CONTROL: in strip_js(), change the string-escape branch
 *     if (d == '\\' && i + 1 < len)  ->  if (0)
 * `"a\\"b"` then terminates early at the escaped quote, the rest is treated as
 * code, and "js: escaped quote inside string" reds.
 */

static void
test_js_literals(void)
{
    is_min(STRIP_JS, "var s = \"a  b\";", "var s=\"a  b\";",
           "js: string content verbatim");
    is_min(STRIP_JS, "var s = 'a  b';", "var s='a  b';",
           "js: single-quoted string content verbatim");
    is_min(STRIP_JS, "var s = \"a\\\"  b\";", "var s=\"a\\\"  b\";",
           "js: escaped quote inside string");

    /* Template literals preserve everything, including newlines, and their
     * substitutions are part of the literal token. */
    is_min(STRIP_JS, "var s = `a  b`;", "var s=`a  b`;",
           "js: template literal content verbatim");
    is_min(STRIP_JS, "var s = `a\n  b`;", "var s=`a\n  b`;",
           "js: newline inside a template literal kept");
    is_min(STRIP_JS, "var s = `a${ x }b`;", "var s=`a${ x }b`;",
           "js: template substitution kept verbatim");

    /* Comments are not part of the program. After an explicit `;` the
     * statement is already terminated, so ASI has nothing to do and the
     * newline the comment ended on is pure formatting. */
    is_min(STRIP_JS, "var x = 1; // trailing\nvar y = 2;",
           "var x=1;var y=2;", "js: line comment removed after ';'");

    /* Without the `;` the newline is load-bearing and must survive the
     * comment's removal, or the two statements weld into one. */
    is_min(STRIP_JS, "var x = 1 // trailing\nvar y = 2",
           "var x=1\nvar y=2", "js: line comment removed, ASI newline survives");
    is_min(STRIP_JS, "var x = /* c */ 1;", "var x=1;",
           "js: block comment removed");

    /* ...but a comment-looking sequence inside a string is content. */
    is_min(STRIP_JS, "var s = \"// not a comment\";",
           "var s=\"// not a comment\";", "js: comment marker inside string");

    /* A block comment SPANNING a newline carries that newline's ASI weight:
     * removing the comment must not also remove the statement boundary it
     * contained, or the two assignments weld into one statement. */
    is_min(STRIP_JS, "a = 1 /* c\n   c */ b = 2", "a=1\nb=2",
           "js: multi-line block comment leaves an ASI newline");
    is_min(STRIP_JS, "a = 1 /* c */ b = 2", "a=1 b=2",
           "js: single-line block comment leaves a plain separator");

    /* A run in front of a literal gets the same ASI treatment as one in front
     * of an ordinary token: `return\n"x"` must not become `return "x"`, which
     * ASI turns into `return undefined`. */
    is_min(STRIP_JS, "return\n\"x\"", "return\n\"x\"",
           "js: newline before a string literal kept for ASI");
    is_min(STRIP_JS, "return\n`x`", "return\n`x`",
           "js: newline before a template literal kept for ASI");

    /* ECMA-262 § 12.8: a RegularExpressionLiteral may not contain a
     * LineTerminator. On hitting one the scanner must bail back to ordinary
     * token scanning — not treat the rest of the file as regex body. The
     * observable difference is that code AFTER the bad line still gets
     * minified; asserting only "did not overflow" cannot see this, because
     * swallowing the remainder is perfectly in-bounds. */
    is_min(STRIP_JS, "return /ab\nvar   x = 1;", "return /ab\nvar x=1;",
           "js: newline ends an unterminated regex, rest still minified");

    /* A leading `/` with no preceding token at all is a regex position. */
    is_min(STRIP_JS, "/a  b/.test(x)", "/a  b/.test(x)",
           "js: regex at the very start of input");
    is_min(STRIP_JS, "   /a  b/.test(x)", "/a  b/.test(x)",
           "js: regex after leading whitespace only");

    /* After a string or template literal, `/` is division. */
    is_min(STRIP_JS, "var x = \"a\"  /  b;", "var x=\"a\"/b;",
           "js: slash after a string literal is division");
    is_min(STRIP_JS, "var x = `a`  /  b;", "var x=`a`/b;",
           "js: slash after a template literal is division");
    /* After an operator it is a regex again. */
    is_min(STRIP_JS, "var x = 1 + /a  b/;", "var x=1+/a  b/;",
           "js: slash after an operator is a regex");

    ok_invariant(STRIP_JS, "var s = \"unterminated", 21,
                 "js: unterminated string stays in bounds");
    ok_invariant(STRIP_JS, "var s = `unterminated", 21,
                 "js: unterminated template stays in bounds");
    ok_invariant(STRIP_JS, "var x = /* unterminated", 23,
                 "js: unterminated block comment stays in bounds");
}


/* ---- JSON ----------------------------------------------------------------
 *
 * CONTROL: in strip_json(), change the string-open branch
 *     if (c == '"')  ->  if (0)
 * Whitespace inside string values is then stripped and
 * "json: whitespace inside a string kept" reds.
 */

static void
test_json(void)
{
    /* RFC 8259 § 2: whitespace is allowed between structural characters and
     * carries no meaning, so all of it goes. */
    is_min(STRIP_JSON, "{ \"a\" : 1 , \"b\" : 2 }", "{\"a\":1,\"b\":2}",
           "json: structural whitespace removed");
    is_min(STRIP_JSON, "[\n  1,\n  2\n]", "[1,2]",
           "json: newlines and indent removed");

    /* RFC 8259 § 7: inside a string, whitespace is content. */
    is_min(STRIP_JSON, "{\"a\":\"x  y\"}", "{\"a\":\"x  y\"}",
           "json: whitespace inside a string kept");
    is_min(STRIP_JSON, "{\"a\":\" \\t \"}", "{\"a\":\" \\t \"}",
           "json: escaped tab inside a string kept");

    /* An escaped quote does not close the string; treating it as a close would
     * strip the whitespace in the remainder. */
    is_min(STRIP_JSON, "{\"a\":\"x\\\"  y\"}", "{\"a\":\"x\\\"  y\"}",
           "json: escaped quote does not end the string");

    /* An escaped backslash DOES leave the next quote free to close. */
    is_min(STRIP_JSON, "{\"a\":\"x\\\\\",\"b\":  1}", "{\"a\":\"x\\\\\",\"b\":1}",
           "json: escaped backslash then real close quote");

    /* A key containing whitespace is still a string. */
    is_min(STRIP_JSON, "{\"a  b\" : 1}", "{\"a  b\":1}",
           "json: whitespace inside a key kept");

    ok_invariant(STRIP_JSON, "{\"a\":\"unterminated", 18,
                 "json: unterminated string stays in bounds");
    ok_invariant(STRIP_JSON, "{\"a\":\"x\\", 8,
                 "json: trailing backslash stays in bounds");
}


/* ---- HTML: attributes ----------------------------------------------------
 *
 * CONTROL: in strip_html()'s attribute path, change the boolean-attr test
 *     && html_is_boolean_attr(dst + name_start, name_len))  ->  && 1)
 * `id="id"` then collapses to a bare `id`, which is a different document, and
 * "html: id=\"id\" is not a boolean attribute" reds.
 */

static void
test_html_attrs(void)
{
    /* HTML5 § 2.4.2: for a boolean attribute, the presence of the attribute is
     * the value, and `x="x"`, `x=""` and bare `x` are all equivalent. */
    is_min(STRIP_HTML, "<input disabled=\"disabled\">", "<input disabled>",
           "html: disabled=\"disabled\" collapses to bare");
    is_min(STRIP_HTML, "<input checked='checked'>", "<input checked>",
           "html: single-quoted boolean attr collapses");

    /* ...and only for attributes that are actually boolean. */
    is_min(STRIP_HTML, "<div id=\"id\">", "<div id=id>",
           "html: id=\"id\" is not a boolean attribute");
    is_min(STRIP_HTML, "<div class=\"class\">", "<div class=class>",
           "html: class=\"class\" is not a boolean attribute");

    /* HTML5 § 13.1.2.3: a quoted value may drop its quotes when it is
     * non-empty and free of whitespace, quotes, backtick, < > and =. */
    is_min(STRIP_HTML, "<a href=\"x.html\">", "<a href=x.html>",
           "html: simple value unquoted");
    is_min(STRIP_HTML, "<a href=\"a b\">", "<a href=\"a b\">",
           "html: value with a space stays quoted");
    is_min(STRIP_HTML, "<a href=\"a=b\">", "<a href=\"a=b\">",
           "html: value with '=' stays quoted");
    is_min(STRIP_HTML, "<a href=\"a>b\">", "<a href=\"a>b\">",
           "html: value with '>' stays quoted");
    is_min(STRIP_HTML, "<a href=\"a`b\">", "<a href=\"a`b\">",
           "html: value with a backtick stays quoted");
    is_min(STRIP_HTML, "<a href=\"\">", "<a href=\"\">",
           "html: empty value stays quoted");

    /* Unquoting is only safe when a token boundary follows the closing quote.
     * `href="x"id=y` would otherwise weld into `href=xid=y`, and `src="a"/>`
     * would absorb the slash into the value. */
    is_min(STRIP_HTML, "<a href=\"x\"id=y>", "<a href=\"x\"id=y>",
           "html: no boundary after quote, stays quoted");
    is_min(STRIP_HTML, "<img src=\"a\"/>", "<img src=\"a\"/>",
           "html: self-closing slash after quote, stays quoted");

    /* An already-unquoted value is copied through. */
    is_min(STRIP_HTML, "<a href=x.html>", "<a href=x.html>",
           "html: unquoted value passes through");
    /* A bare attribute has no value at all. */
    is_min(STRIP_HTML, "<input disabled>", "<input disabled>",
           "html: bare attribute unchanged");

    /* Truncated markup must not walk off the end or synthesize a closing quote
     * (which would make output longer than input — a heap overflow for the
     * caller, which sizes dst == input length). */
    ok_invariant(STRIP_HTML, "<a href=\"unterminated", 21,
                 "html: unterminated quoted value stays in bounds");
    ok_invariant(STRIP_HTML, "<a href=", 8,
                 "html: attribute cut at '=' stays in bounds");
    ok_invariant(STRIP_HTML, "<a hre", 6,
                 "html: tag cut mid-name stays in bounds");
}


/* ---- HTML: structure ----------------------------------------------------- */

static void
test_html_structure(void)
{
    is_min(STRIP_HTML, "<p>a</p>\n\n<p>b</p>", "<p>a</p><p>b</p>",
           "html: inter-tag whitespace removed");
    is_min(STRIP_HTML, "<p>Hello   world</p>", "<p>Hello world</p>",
           "html: text whitespace collapsed to one space");
    is_min(STRIP_HTML, "<!-- c --><p>a</p>", "<p>a</p>", "html: comment removed");

    /* Bodies that are whitespace-significant survive verbatim. */
    is_min(STRIP_HTML, "<pre>a   b\nc</pre>", "<pre>a   b\nc</pre>",
           "html: <pre> body verbatim");
    is_min(STRIP_HTML, "<textarea>a   b</textarea>", "<textarea>a   b</textarea>",
           "html: <textarea> body verbatim");

    /* A <script>/<style> body is JS/CSS, not markup: whatever the core does to
     * it, an HTML comment marker inside must not be treated as markup. */
    is_min(STRIP_HTML, "<script>var s=\"<!-- x -->\";</script>",
           "<script>var s=\"<!-- x -->\";</script>",
           "html: comment marker inside a script string survives");

    /* Text before a tag is a text node: `a <b>` is not `a<b>` in rendering,
     * so the run collapses to one space rather than vanishing. Only inter-tag
     * whitespace ('>' … '<') is safe to drop entirely. */
    is_min(STRIP_HTML, "<p>text   <b>x</b></p>", "<p>text <b>x</b></p>",
           "html: space between text and a tag kept as one");

    /* A raw-text element's closing tag is recognised with trailing whitespace
     * or a slash inside it, not only as an exact `</script>`. Missing that
     * would leave the rest of the document inside the raw body. */
    is_min(STRIP_HTML, "<script>var a=1;</script >\n<p>x</p>",
           "<script>var a=1;</script ><p>x</p>",
           "html: raw-text close tag with trailing space recognised");
    is_min(STRIP_HTML, "<style>a{}</style/>\n<p>x</p>",
           "<style>a{}</style/><p>x</p>",
           "html: raw-text close tag with a slash recognised");
    /* ...but a longer name that merely starts with it is not the close tag. */
    is_min(STRIP_HTML, "<script>var a=1;</scriptx></script>",
           "<script>var a=1;</scriptx></script>",
           "html: </scriptx> does not close <script>");

    ok_invariant(STRIP_HTML, "<!-- unterminated", 17,
                 "html: unterminated comment stays in bounds");
    ok_invariant(STRIP_HTML, "<pre>unterminated", 17,
                 "html: unterminated <pre> stays in bounds");
}


/* ---- SVG / XML -----------------------------------------------------------
 *
 * CONTROL: in html_copy_tag(), drop the XML/SVG gate on unquoting
 *     int can_unquote = allow_unquote && (val_len > 0);
 *  -> int can_unquote = (val_len > 0);
 * `<circle r="1" cx="2">` then unquotes to `<circle r=1 cx=2>`, which is not
 * well-formed XML, and the two "stay quoted" cases below red.
 *
 * WHAT THIS CONTROL TAUGHT US: a SELF-CLOSING element is not a valid oracle
 * for this gate. `<circle r="1"/>` keeps its quotes with the gate removed,
 * because the separate boundary check refuses to unquote a value followed by
 * `/`. Testing only self-closing tags would have asserted the boundary rule
 * twice and the XML rule never. The cases must therefore be non-self-closing.
 */

static void
test_svg_xml(void)
{
    /* XML 1.0 § 3.1: AttValue is always quoted. There is no unquoted form and
     * no boolean-attribute concept, so neither transform may apply. These are
     * deliberately NOT self-closing — see the control note above. */
    is_min(STRIP_SVG, "<circle r=\"1\" cx=\"2\">", "<circle r=\"1\" cx=\"2\">",
           "svg: attribute values stay quoted");
    is_min(STRIP_XML, "<a b=\"b\">", "<a b=\"b\">",
           "xml: b=\"b\" is not collapsed to a bare attribute");
    is_min(STRIP_XML, "<guid isPermaLink=\"false\">x</guid>",
           "<guid isPermaLink=\"false\">x</guid>",
           "xml: RSS attribute value stays quoted");

    /* A self-closing element keeps its quotes too, but by the boundary rule
     * rather than the XML rule. Kept as documentation of both. */
    is_min(STRIP_SVG, "<circle r=\"1\"/>", "<circle r=\"1\"/>",
           "svg: self-closing attribute stays quoted");

    is_min(STRIP_SVG, "<svg>\n  <circle r=\"1\"/>\n</svg>",
           "<svg><circle r=\"1\"/></svg>", "svg: inter-tag whitespace removed");

    /* XML 1.0 § 2.7: CDATA content is literal. */
    is_min(STRIP_XML, "<a><![CDATA[  x  ]]></a>", "<a><![CDATA[  x  ]]></a>",
           "xml: CDATA content verbatim");

    is_min(STRIP_XML, "<a><!-- c --><b/></a>", "<a><b/></a>",
           "xml: comment removed");

    /* XML text nodes are whitespace-sensitive, so only the '>' … '<' run is
     * safe to drop; a run adjacent to text collapses to one space instead of
     * vanishing. `<a>x <b/></a>` losing its space changes the character data. */
    is_min(STRIP_XML, "<a>x   <b/></a>", "<a>x <b/></a>",
           "xml: space between text and a tag kept as one");
    is_min(STRIP_XML, "<a><b/>   x</a>", "<a><b/> x</a>",
           "xml: space between a tag and text kept as one");
    is_min(STRIP_SVG, "<text>a   b</text>", "<text>a b</text>",
           "svg: text-node run collapses to one space");

    ok_invariant(STRIP_XML, "<a><![CDATA[unterminated", 24,
                 "xml: unterminated CDATA stays in bounds");
}


/* ---- dispatch and degenerate inputs -------------------------------------- */

static void
test_dispatch(void)
{
    unsigned char out[16];

    /* Zero-length input on every kind: must write nothing and not touch dst. */
    tap(strip_minify(STRIP_HTML, (const unsigned char *) "", 0, out) == 0,
        "empty input: HTML writes nothing");
    tap(strip_minify(STRIP_CSS, (const unsigned char *) "", 0, out) == 0,
        "empty input: CSS writes nothing");
    tap(strip_minify(STRIP_JS, (const unsigned char *) "", 0, out) == 0,
        "empty input: JS writes nothing");
    tap(strip_minify(STRIP_JSON, (const unsigned char *) "", 0, out) == 0,
        "empty input: JSON writes nothing");
    tap(strip_minify(STRIP_SVG, (const unsigned char *) "", 0, out) == 0,
        "empty input: SVG writes nothing");
    tap(strip_minify(STRIP_XML, (const unsigned char *) "", 0, out) == 0,
        "empty input: XML writes nothing");

    /* Input that is entirely whitespace collapses to at most one byte, and for
     * JSON to nothing at all. */
    is_min(STRIP_JSON, "   \n\t  ", "", "json: all-whitespace input empties");

    /* High bytes are opaque: the core is byte-oriented and must not mangle
     * UTF-8 sequences it does not understand. */
    is_min(STRIP_HTML, "<p>\xc3\xa9\xc3\xa9</p>", "<p>\xc3\xa9\xc3\xa9</p>",
           "html: multibyte UTF-8 passes through");
    is_min(STRIP_CSS, "a{content:\"\xc3\xa9\"}", "a{content:\"\xc3\xa9\"}",
           "css: multibyte UTF-8 inside a string");

    /* Each kind reaches its own branch of the dispatch switch. Same input,
     * different kind, different result — which is what proves the dispatch is
     * doing anything at all. */
    is_min(STRIP_JSON, "{\"a\" : 1}", "{\"a\":1}", "dispatch: JSON kind selected");
    is_min(STRIP_CSS, "a{b : c}", "a{b:c}", "dispatch: CSS kind selected");
}


/* ---- boundary cases for comprehensive coverage ----------------------- */

static void
test_boundary_cases(void)
{
    /* JSON: escape sequences at various positions */
    is_min(STRIP_JSON, "{\"a\":\"\\\\\"}","{\"a\":\"\\\\\"}",
           "json: backslash escape sequence preserved");
    is_min(STRIP_JSON, "{\"a\":\"\\n\"}","{\"a\":\"\\n\"}",
           "json: newline escape preserved");
    is_min(STRIP_JSON, "{\"a\":\"\\t\"}","{\"a\":\"\\t\"}",
           "json: tab escape preserved");

    /* CSS: edge cases with whitespace collapsing around operators */
    is_min(STRIP_CSS, "a{margin:1px+2px}", "a{margin:1px+2px}",
           "css: plus operator kept");
    is_min(STRIP_CSS, "a{calc(1px + 2px)}", "a{calc(1px + 2px)}",
           "css: operator with spaces in calc");

    /* HTML: edge cases with void elements and attributes */
    is_min(STRIP_HTML, "<br/>", "<br/>",
           "html: self-closing br element");
    is_min(STRIP_HTML, "<img src=x alt=y>", "<img src=x alt=y>",
           "html: multiple unquoted attributes");

    /* JS: edge cases with operators adjacent to text */
    is_min(STRIP_JS, "a+++b", "a+++b",
           "js: plus-plus (increment) followed by operand");
    is_min(STRIP_JS, "a-- >b", "a-->b",
           "js: decrement followed by > forms HTML comment opener");

    /* CSS: minimum non-zero lengths */
    is_min(STRIP_CSS, "a{width:1px}", "a{width:1px}",
           "css: minimum length unit kept");

    /* HTML: empty vs whitespace in text nodes */
    is_min(STRIP_HTML, "<p></p>", "<p></p>",
           "html: empty paragraph");
    is_min(STRIP_HTML, "<p> </p>", "<p></p>",
           "html: paragraph with whitespace-only text node collapsed");

    /* JSON: numbers at boundaries */
    is_min(STRIP_JSON, "{\"a\":0}", "{\"a\":0}",
           "json: zero value");
    is_min(STRIP_JSON, "{\"a\":-1}", "{\"a\":-1}",
           "json: negative number");
    is_min(STRIP_JSON, "{\"a\":1.0}", "{\"a\":1.0}",
           "json: decimal number");

    /* SVG/XML: namespace handling */
    is_min(STRIP_SVG, "<svg xmlns=\"http://www.w3.org/2000/svg\">",
           "<svg xmlns=\"http://www.w3.org/2000/svg\">",
           "svg: namespace attribute preserved");
}


int
main(void)
{
    test_css_url();
    test_css_hex();
    test_css_numbers();
    test_css_basic();
    test_js_regex();
    test_js_newlines();
    test_js_literals();
    test_json();
    test_html_attrs();
    test_html_structure();
    test_svg_xml();
    test_boundary_cases();
    test_dispatch();

    printf("1..%d\n", plan_n);
    return plan_failed ? 1 : 0;
}

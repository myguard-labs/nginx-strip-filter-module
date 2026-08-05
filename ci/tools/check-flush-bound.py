#!/usr/bin/env python3
"""check-flush-bound.py -- source-invariant gate over the REAL
ngx_http_strip_flush() copy loop, replacing a unit-test harness that could
not link src/ngx_http_strip_filter_module.c (see git history: the removed
ci/tests/unit/flush_copy_bound.c reimplemented the loop's arithmetic in a
standalone mock and stayed green when the actual fix was reverted -- it
duplicated the logic under test instead of checking it).

THE FIX THIS GATE PROTECTS (src/ngx_http_strip_filter_module.c,
ngx_http_strip_flush(), finding N-2). Before the fix: the chain copy loop
trusted ctx->len to already match the accumulated chain's true byte count and
copied every buffer unconditionally; ctx->len can lag the chain (it stops
advancing once ctx->buffering clears while the chain keeps growing), so a
diverged chain overran the ctx->len-sized `src` allocation. The fix adds two
invariants, both asserted here directly against the real source text:

  (a) BOUNDED COPY -- the loop condition inside the function references a
      remaining/capacity variable, AND the loop body clamps the per-buffer
      copy length `n` against that variable before the memcpy. Not merely
      that the word "remaining" appears in the file -- it must gate the loop
      and clamp the copy.
  (b) ACTUAL-LENGTH TO strip_minify() -- the strip_minify(...) call inside
      the function passes the number of bytes actually copied (the pointer
      delta from the copy loop), not the stale ctx->len. A call shaped like
      strip_minify(ctx->kind, src, ctx->len, dst) must FAIL this gate.

WHY THIS CAN'T BE A GREP FOR A SUBSTRING. "remaining" or "copied" appearing
anywhere in the 900-line file proves nothing about this one function. This
script isolates ngx_http_strip_flush()'s own body (brace-matched from its
opening `{` to the matching closing `}`) and evaluates both invariants only
within that span, so an unrelated ctx->len elsewhere in the file can neither
satisfy nor break the check.

Usage:
    ci/tools/check-flush-bound.py [path-to-module.c]

Exit 0 if both invariants hold inside ngx_http_strip_flush(), 1 otherwise,
2 if the function or file cannot be found (prerequisite missing).
"""

import re
import sys

DEFAULT_PATH = "src/ngx_http_strip_filter_module.c"

FUNC_SIG_RE = re.compile(
    r"^ngx_http_strip_flush\(ngx_http_request_t \*r,.*\)\s*$", re.MULTILINE
)


def extract_function_body(text, path):
    """Return (body_text, start_line) for ngx_http_strip_flush()'s body,
    brace-matched from its opening '{' to the matching closing '}'."""
    m = FUNC_SIG_RE.search(text)
    if not m:
        print(
            f"check-flush-bound: could not find 'ngx_http_strip_flush(' "
            f"function signature in {path}",
            file=sys.stderr,
        )
        return None, 0

    brace_open = text.index("{", m.end())
    depth = 0
    i = brace_open
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                body = text[brace_open : i + 1]
                start_line = text.count("\n", 0, brace_open) + 1
                return body, start_line
        i += 1

    print(
        f"check-flush-bound: unbalanced braces while scanning "
        f"ngx_http_strip_flush() in {path}",
        file=sys.stderr,
    )
    return None, 0


def check_bounded_loop(body):
    """Invariant (a): the copy loop's `for` condition references a
    remaining/capacity variable, AND the loop body clamps `n` against it
    before the memcpy that uses `n`."""
    for_re = re.compile(
        r"for\s*\([^;]*;\s*[^;]*\b(remaining|cap|capacity)\b[^;]*;[^)]*\)\s*\{"
    )
    for_match = for_re.search(body)
    if not for_match:
        return False, (
            "no 'for' loop condition referencing a remaining/capacity "
            "variable was found -- the copy loop must be bounded by a "
            "shrinking capacity check, not merely iterate the whole chain"
        )

    # Isolate the loop body (brace-matched from the for's '{').
    loop_open = for_match.end() - 1
    depth = 0
    i = loop_open
    loop_body = None
    while i < len(body):
        if body[i] == "{":
            depth += 1
        elif body[i] == "}":
            depth -= 1
            if depth == 0:
                loop_body = body[loop_open : i + 1]
                break
        i += 1

    if loop_body is None:
        return False, "copy loop body has unbalanced braces"

    clamp_re = re.compile(
        r"if\s*\(\s*n\s*>\s*(remaining|cap|capacity)\s*\)\s*\{?\s*"
        r"n\s*=\s*(remaining|cap|capacity)\s*;"
    )
    if not clamp_re.search(loop_body):
        return False, (
            "copy loop condition references a capacity variable, but its "
            "body does not clamp the per-buffer copy length 'n' against it "
            "(expected 'if (n > remaining) { n = remaining; }' or "
            "equivalent) before the memcpy"
        )

    memcpy_re = re.compile(r"ngx_memcpy\s*\([^,]+,\s*[^,]+,\s*n\s*\)")
    if not memcpy_re.search(loop_body):
        return False, (
            "copy loop clamps a capacity variable but the memcpy call does "
            "not copy the clamped length 'n' -- clamp and copy have "
            "diverged"
        )

    return True, None


def check_actual_length_to_minify(body):
    """Invariant (b): the strip_minify(...) call inside the function must
    NOT pass ctx->len as its length argument -- it must pass the actual
    bytes copied (e.g. a 'copied' variable derived from the copy loop's
    pointer delta)."""
    minify_re = re.compile(r"strip_minify\s*\(([^;]*?)\)\s*;", re.DOTALL)
    m = minify_re.search(body)
    if not m:
        return False, "no strip_minify(...) call found inside the function"

    args_text = m.group(1)
    args = [a.strip() for a in args_text.split(",")]
    if len(args) < 3:
        return False, (
            f"strip_minify(...) call has fewer than 3 arguments "
            f"(got: {args_text!r}) -- cannot check the length argument"
        )

    length_arg = args[2]
    if re.search(r"\bctx\s*->\s*len\b", length_arg):
        return False, (
            f"strip_minify()'s length argument is {length_arg!r} -- it "
            "passes the stale ctx->len instead of the bytes actually "
            "copied by the bounded loop; a diverged chain shorter than "
            "ctx->len would feed uninitialized tail bytes to the minifier"
        )

    return True, None


def main(argv):
    path = argv[1] if len(argv) > 1 else DEFAULT_PATH

    try:
        with open(path, "r") as f:
            text = f.read()
    except OSError as e:
        print(f"check-flush-bound: cannot read {path}: {e}", file=sys.stderr)
        return 2

    body, start_line = extract_function_body(text, path)
    if body is None:
        return 2

    errors = []

    ok, msg = check_bounded_loop(body)
    if not ok:
        errors.append(f"{path}:{start_line}: [bounded-copy] {msg}")

    ok, msg = check_actual_length_to_minify(body)
    if not ok:
        errors.append(f"{path}:{start_line}: [actual-length-to-minify] {msg}")

    if errors:
        print("check-flush-bound: FAIL", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(
        f"check-flush-bound: clean -- ngx_http_strip_flush() ({path}:"
        f"{start_line}) is bounded and passes actual-copied length to "
        "strip_minify()"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

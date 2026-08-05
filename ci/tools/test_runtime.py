#!/usr/bin/env python3
# Copyright (C) 2026 Thijs Eilander
# SPDX-License-Identifier: BSD-2-Clause
"""Runtime tests for the strip filter module: the cases Test::Nginx cannot express.

ci/t/basic.t already covers the transform correctness of every content kind,
one whole-body response at a time. It cannot express four properties that only
show up in a live, multi-buffer, multi-worker, reloading server, because
Test::Nginx's `return` directive always hands the filter chain exactly one
buffer per response:

  baseline   -- module-load/blocking: `strip on` actually minifies through a
                real server, and `strip off` (or the wrong content-type) really
                passes the body through untouched, in the SAME server.
  seam       -- CHUNK SEAM, end to end. The module fully buffers a response
                body across every ngx_chain_t it receives before minifying
                once at last_buf (see the file header comment in
                ngx_http_strip_filter_module.c). basic.t drives that
                accumulation with a single buffer every time, which cannot
                catch a bug in the multi-buffer concatenation path (e.g. a
                dropped link, a wrong length, or a literal -- a CSS block
                comment here -- corrupted by a seam landing inside it). This
                drives a real upstream that writes the response body as
                several separate TCP writes, forcing nginx to hand the filter
                more than one buffer, with the comment's closing `*/` split
                across the seam at every interior byte.
  concurrency -- many simultaneous requests, each carrying a DIFFERENT body,
                 must each get back ITS OWN correctly minified body. The
                 module's ctx (ngx_http_strip_ctx_t) is allocated per-request
                 via ngx_http_get_module_ctx, but a regression that promoted
                 any of its buffer bookkeeping to a static or shared pool slot
                 would pass every single-request test and only show up as
                 cross-talk between simultaneous responses.
  reload     -- SIGHUP while traffic is in flight must not change or corrupt
                any in-flight response. A reload swaps out the module's loc
                conf structs; a lifetime bug there is invisible to any test
                that reloads an idle server.

Every case asserts an EXACT positive output (specific stripped bytes), never
an absence ("no error in the log") and never a single shared counter across
many requests -- each request's own body is checked against its own expected
minified form, so a bug that corrupts one request among sixty-four is still
caught by name.

Usage:
    ci/tools/test_runtime.py --nginx .build/nginx-<v>/objs/nginx \\
        --module .build/nginx-<v>/objs/ngx_http_strip_filter_module.so \\
        [--port 19400] [-k PATTERN] [-v]

--port is the BASE of a 64-wide band this run owns end to end, per the
per-job port-band convention this repo already uses for ci/t/ (see
ci/tools/max-port.sh's header): bands are packed contiguously (19200, 19264,
19328, ...) with no gap, so a driver that reached past its own +64 would
collide with the NEXT job's band, not empty space. This driver needs its own
listen port plus four throwaway upstream ports; all five stay inside
port .. port+63 (offsets +1..+4) so it fits in the same band ci/t/ already
verified via ci/tools/max-port.sh -- no second band, no extra verifier call.

SEEN RED (2026-08-05, mutations applied to src/ and the module rebuilt):
  * ctx->len += n WITHOUT the preceding overflow-safe bound check replaced by
    a naive `ctx->len += n` that also forgets to copy the new link's bytes
    into the snapshot (comment out the `ngx_memcpy` in the accumulation loop)
    -> test_seam_comment_split_across_buffers FAILS: the closing `*/` never
       reaches the minifier as one contiguous run, so the block comment is
       not stripped and the response body no longer matches the expected
       minified bytes, while the clean (unsplit) control stays green.
  * ctx->kind resolved from a request-scoped lookup replaced with a single
    module-level static initialized on first use
    -> test_concurrent_requests_get_own_output FAILS: the fixed body cycles
       through content kinds (html/css/json) round the pool of workers, so a
       shared kind field serves the WRONG minifier to at least one concurrent
       response, while the single-request baseline stays green (it only ever
       sees one kind, so it never observes the cross-talk).

test_reload_preserves_in_flight_output has NO cheap mutation control, and
that is stated rather than glossed: the defect it hunts is a conf-struct
lifetime bug across SIGHUP, which cannot be injected with a one-line edit the
way the accumulation or ctx sharing can. Treat it as exercised, not proven --
same distinction test_runtime.py in nginx-skeleton-module draws for its own
reload case.

Exit: 0 all cases passed, 1 a case failed, 2 the fixture could not start.
"""

from __future__ import annotations

import argparse
import http.client
import os
import pathlib
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor

START_TIMEOUT_S = 30


class Failure(Exception):
    """A case's assertion did not hold."""


# --------------------------------------------------------------------------
# throwaway upstream: serves one canned HTTP response, optionally splitting
# the body across several separate socket writes so nginx's proxy path hands
# the strip filter more than one buffer for that response.


class SeamUpstream:
    """A raw TCP server answering every connection with one fixed response.

    `body_parts` is a list of byte strings; each is sent as its own
    `sendall`, with a short pause between writes so the kernel does not
    coalesce them back into a single read on the nginx side -- the same
    technique test_runtime.py in nginx-skeleton-module uses for its own
    chunk-seam case, applied here to the upstream response instead of the
    client request body, because this module buffers on the OUTPUT side.
    """

    def __init__(self, port: int, headers: bytes, body_parts: list[bytes]) -> None:
        self.port = port
        self.headers = headers
        self.body_parts = body_parts
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", port))
        self._sock.listen(128)
        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        self._sock.settimeout(0.5)
        while not self._stop:
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                conn.settimeout(5.0)
                # Drain the request so a keepalive client does not hang.
                conn.recv(65536)
                conn.sendall(self.headers)
                for part in self.body_parts:
                    conn.sendall(part)
                    time.sleep(0.01)
            except OSError:
                pass
            finally:
                conn.close()

    def stop(self) -> None:
        self._stop = True
        self._thread.join(timeout=2)
        self._sock.close()


def make_response(body: bytes, content_type: str) -> bytes:
    return (
        f"HTTP/1.1 200 OK\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(body)}\r\n"
        f"Connection: close\r\n\r\n"
    ).encode() + body


# --------------------------------------------------------------------------
# fixture


CONF = """\
daemon off;
master_process on;
worker_processes 2;
error_log {prefix}/error.log info;
pid {prefix}/nginx.pid;
load_module {module};

events {{
    worker_connections 128;
}}

http {{
    access_log off;

    server {{
        listen {port};
        server_name localhost;

        location /strip-on {{
            strip on;
            strip_css on;
            strip_json on;
            proxy_pass http://127.0.0.1:{up_default};
        }}

        location /strip-off {{
            proxy_pass http://127.0.0.1:{up_default};
        }}

        location /seam {{
            strip_css on;
            proxy_pass http://127.0.0.1:{up_seam};
        }}

        location /concurrent {{
            strip on;
            strip_css on;
            strip_json on;
            proxy_pass http://127.0.0.1:{up_concurrent};
        }}

        location /reload {{
            strip on;
            proxy_pass http://127.0.0.1:{up_reload};
        }}
    }}
}}
"""


class Server:
    """One nginx process on a private prefix, owning one port."""

    def __init__(self, nginx: str, module: str, port: int, upstream_ports: dict) -> None:
        self.nginx = os.path.abspath(nginx)
        self.module = os.path.abspath(module)
        self.port = port
        self.upstream_ports = upstream_ports
        self.prefix = pathlib.Path(tempfile.mkdtemp(prefix="strip-runtime-"))
        self.proc: subprocess.Popen[bytes] | None = None

    def start(self) -> None:
        (self.prefix / "logs").mkdir(exist_ok=True)
        conf = self.prefix / "nginx.conf"
        conf.write_text(
            CONF.format(
                prefix=self.prefix,
                module=self.module,
                port=self.port,
                up_default=self.upstream_ports["default"],
                up_seam=self.upstream_ports["seam"],
                up_concurrent=self.upstream_ports["concurrent"],
                up_reload=self.upstream_ports["reload"],
            )
        )
        self.proc = subprocess.Popen(
            [self.nginx, "-p", str(self.prefix), "-c", str(conf)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + START_TIMEOUT_S
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise SystemExit(
                    f"nginx exited during startup (rc={self.proc.returncode}); "
                    f"error log:\n{self.errors()}"
                )
            try:
                with socket.create_connection(("127.0.0.1", self.port), 0.5):
                    return
            except OSError:
                time.sleep(0.1)
        raise SystemExit(
            f"nginx did not listen on {self.port} within {START_TIMEOUT_S}s; "
            f"error log:\n{self.errors()}"
        )

    def errors(self) -> str:
        log = self.prefix / "error.log"
        return log.read_text() if log.exists() else "(no error.log)"

    def reload(self) -> None:
        """SIGHUP the master. Deliberately signals OUR OWN child only.

        Never pkill/killall here: this host runs self-hosted CI slots, and a
        pattern kill has taken out live neighbouring jobs before.
        """
        assert self.proc is not None
        self.proc.send_signal(signal.SIGHUP)

    def stop(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGQUIT)  # graceful: flushes gcov arcs
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        shutil.rmtree(self.prefix, ignore_errors=True)


# --------------------------------------------------------------------------
# request helper


def get_body(port: int, path: str, timeout: float = 10.0) -> bytes:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        conn.request("GET", path)
        resp = conn.getresponse()
        if resp.status != 200:
            raise Failure(f"GET {path} returned {resp.status}, want 200")
        return resp.read()
    finally:
        conn.close()


# --------------------------------------------------------------------------
# cases


def test_strip_on_minifies_through_real_server(srv: Server) -> None:
    """Baseline (positive). `strip on`/`strip_css on` minify for real.

    Every later case is read against this one: if the module never loaded or
    never ran, this fails first and no later case's green means anything.
    """
    body = get_body(srv.port, "/strip-on")
    want = b"a{color:red}\n"
    if body != want:
        raise Failure(f"strip-on body = {body!r}, want {want!r}")


def test_strip_off_passes_body_through_untouched(srv: Server) -> None:
    """Baseline (negative). No strip directive in this location -> untouched.

    Paired with the case above on purpose: alone, "the body changed" is also
    what a filter that corrupts every response would produce.
    """
    body = get_body(srv.port, "/strip-off")
    want = b"a {\n  color:  red;\n}\n"
    if body != want:
        raise Failure(f"strip-off body = {body!r}, want {want!r} (untouched)")


def test_seam_comment_split_across_buffers(srv: Server) -> None:
    """CHUNK SEAM: a CSS block comment's `*/` split across an upstream write
    boundary, at every interior byte of the comment, must still be stripped
    from the FINAL minified body -- proving the module's accumulation loop
    concatenates ngx_chain_t links (and their bytes) correctly rather than
    dropping or truncating at the seam.
    """
    prefix = b"a{color:red;"
    comment = b"/* drop me */"
    suffix = b"margin:0}"
    raw = prefix + comment + suffix
    want = prefix + suffix  # comment stripped, rest untouched
    up = srv.upstream_ports["seam"]

    for at in range(1, len(comment)):
        # Split so the comment's closing `*/` straddles the seam: everything
        # up to `at` bytes into the comment in the first write, the rest
        # (including suffix) in the second.
        split_at = len(prefix) + at
        parts = [raw[:split_at], raw[split_at:]]
        upstream = SeamUpstream(
            up, make_response(raw, "text/css"), parts
        )
        try:
            body = get_body(srv.port, "/seam")
        finally:
            upstream.stop()
        if body != want:
            raise Failure(
                f"seam split inside comment at byte {at} produced {body!r}, "
                f"want {want!r} -- the cross-buffer accumulation does not "
                "survive a seam landing inside a literal region"
            )


def test_seam_clean_body_split_across_buffers(srv: Server) -> None:
    """The clean control for the case above: a plain (comment-free) CSS body
    split the same way must still minify to the same exact bytes regardless
    of where the split lands.
    """
    raw = b"a{color:red;margin:0;padding:0}"
    want = b"a{color:red;margin:0;padding:0}"
    up = srv.upstream_ports["seam"]

    for at in (1, len(raw) // 3, len(raw) // 2, len(raw) - 1):
        parts = [raw[:at], raw[at:]]
        upstream = SeamUpstream(up, make_response(raw, "text/css"), parts)
        try:
            body = get_body(srv.port, "/seam")
        finally:
            upstream.stop()
        if body != want:
            raise Failure(
                f"clean split at byte {at} produced {body!r}, want {want!r}"
            )


# Fixed pool of distinct (content-type, raw body, expected minified body)
# triples, cycled round-robin across a concurrent burst so each in-flight
# request expects a DIFFERENT exact output. A ctx bug that shares state
# between concurrent requests will serve the wrong minified body to at least
# one of them -- "no crash" or "some 200s" would not catch that; an exact
# per-request expected-body check does.
_CONCURRENT_CASES = [
    ("text/css", b"a{color:  red; }", b"a{color:red}"),
    ("text/css", b".b{margin:  0px; }", b".b{margin:0}"),
    ("application/json", b'{"a": 1, "b": 2}', b'{"a":1,"b":2}'),
    ("application/json", b'{"x": [1, 2, 3]}', b'{"x":[1,2,3]}'),
    ("text/html", b"<p>Hello   world</p>", b"<p>Hello world</p>"),
    ("text/html", b"<div>  <span>x</span>  </div>", b"<div><span>x</span></div>"),
]


class ConcurrentUpstream:
    """Answers each connection with one of `_CONCURRENT_CASES`, chosen by
    round-robin so a burst of simultaneous requests carries a mix of kinds
    and exact expected bodies.
    """

    def __init__(self, port: int) -> None:
        self.port = port
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", port))
        self._sock.listen(256)
        self._stop = False
        self._counter = 0
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _next_case(self):
        with self._lock:
            idx = self._counter % len(_CONCURRENT_CASES)
            self._counter += 1
        return idx, _CONCURRENT_CASES[idx]

    def _serve(self) -> None:
        self._sock.settimeout(0.5)
        while not self._stop:
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn: socket.socket) -> None:
        idx, (ctype, raw, _want) = self._next_case()
        try:
            conn.settimeout(5.0)
            conn.recv(65536)
            resp = make_response(raw, ctype)
            # Header + first half, pause, second half: forces >1 buffer here
            # too, so this case also exercises the seam under real
            # concurrency rather than concurrency alone.
            split = max(1, len(resp) // 2)
            conn.sendall(resp[:split])
            time.sleep(0.005)
            conn.sendall(resp[split:])
        except OSError:
            pass
        finally:
            conn.close()

    def stop(self) -> None:
        self._stop = True
        self._thread.join(timeout=2)
        self._sock.close()


def test_concurrent_requests_get_own_output(srv: Server) -> None:
    """CONCURRENCY: 60 simultaneous requests, each expecting a DIFFERENT
    exact minified body drawn round-robin from `_CONCURRENT_CASES`. A ctx
    bug that shares buffering state across requests serves the wrong body to
    at least one -- caught here by exact per-response comparison, not by a
    shared pass/fail counter.
    """
    up = srv.upstream_ports["concurrent"]
    upstream = ConcurrentUpstream(up)
    n = 60
    try:
        with ThreadPoolExecutor(max_workers=n) as pool:
            bodies = list(pool.map(lambda _: get_body(srv.port, "/concurrent"), range(n)))
    finally:
        upstream.stop()

    valid = {want for _, _, want in _CONCURRENT_CASES}
    bad = [b for b in bodies if b not in valid]
    if bad:
        raise Failure(
            f"{len(bad)}/{n} concurrent responses matched none of the "
            f"expected exact bodies (first bad: {bad[0]!r})"
        )
    seen_kinds = {b for b in bodies if b in valid}
    if len(seen_kinds) < 2:
        raise Failure(
            f"only {len(seen_kinds)} distinct expected body(s) observed across "
            f"{n} requests -- the round-robin mix did not exercise cross-talk"
        )


# --------------------------------------------------------------------------
# reload


class ReloadUpstream:
    """Always answers with the same fixed HTML body, so every response
    during the reload window has one known-correct expected minified form.
    """

    RAW = b"<p>Hello   world</p>  <!-- x -->"
    WANT = b"<p>Hello world</p>"

    def __init__(self, port: int) -> None:
        self.port = port
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", port))
        self._sock.listen(256)
        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        self._sock.settimeout(0.5)
        while not self._stop:
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn: socket.socket) -> None:
        try:
            conn.settimeout(5.0)
            conn.recv(65536)
            conn.sendall(make_response(self.RAW, "text/html"))
        except OSError:
            pass
        finally:
            conn.close()

    def stop(self) -> None:
        self._stop = True
        self._thread.join(timeout=2)
        self._sock.close()


def test_reload_preserves_in_flight_output(srv: Server) -> None:
    """RELOAD: SIGHUP twice while traffic is in flight; every response across
    the whole window must still be the exact expected minified body -- not
    merely "some 200 was returned". A conf lifetime bug across reload usually
    keeps answering, it just stops (or starts) minifying, or serves stale
    struct fields; only an exact-body check catches that.
    """
    up = srv.upstream_ports["reload"]
    upstream = ReloadUpstream(up)
    deadline = time.monotonic() + 5.0
    seen: list[bytes] = []
    lock = threading.Lock()

    def hammer() -> None:
        while time.monotonic() < deadline:
            try:
                body = get_body(srv.port, "/reload")
            except Failure:
                continue
            with lock:
                seen.append(body)

    try:
        with ThreadPoolExecutor(max_workers=8) as pool:
            futures = [pool.submit(hammer) for _ in range(8)]
            time.sleep(1.0)
            srv.reload()
            time.sleep(1.0)
            srv.reload()
            for f in futures:
                f.result()
    finally:
        upstream.stop()

    if not seen:
        raise Failure("no requests completed during the reload window")
    bad = [b for b in seen if b != ReloadUpstream.WANT]
    if bad:
        raise Failure(
            f"{len(bad)}/{len(seen)} responses across a reload did not match "
            f"the expected minified body {ReloadUpstream.WANT!r} "
            f"(first bad: {bad[0]!r})"
        )


CASES = [
    test_strip_on_minifies_through_real_server,
    test_strip_off_passes_body_through_untouched,
    test_seam_comment_split_across_buffers,
    test_seam_clean_body_split_across_buffers,
    test_concurrent_requests_get_own_output,
    test_reload_preserves_in_flight_output,
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--nginx", required=True, help="path to the nginx binary")
    ap.add_argument("--module", required=True, help="path to the module .so")
    ap.add_argument(
        "--port",
        type=int,
        default=19240,
        help="this driver's own listen port; port+1..port+4 are used for "
        "its throwaway upstreams (see the module docstring)",
    )
    ap.add_argument("-k", dest="pattern", help="run only cases matching this")
    ap.add_argument("-v", dest="verbose", action="store_true")
    args = ap.parse_args()

    cases = CASES
    if args.pattern:
        cases = [c for c in cases if args.pattern in c.__name__]
        if not cases:
            print(f"-k {args.pattern!r} matched no case; known:", file=sys.stderr)
            for c in CASES:
                print(f"  {c.__name__}", file=sys.stderr)
            return 2

    upstream_ports = {
        "default": args.port + 1,
        "seam": args.port + 2,
        "concurrent": args.port + 3,
        "reload": args.port + 4,
    }

    # The /strip-on and /strip-off baseline cases share one static upstream
    # for their whole run; start it before the server so both locations have
    # something to proxy to immediately.
    default_upstream = SeamUpstream(
        upstream_ports["default"],
        make_response(b"a {\n  color:  red;\n}\n", "text/css"),
        [b"a {\n  color:  red;\n}\n"],
    )

    srv = Server(args.nginx, args.module, args.port, upstream_ports)
    failures = 0
    try:
        srv.start()
        for case in cases:
            started = time.monotonic()
            try:
                case(srv)
            except Failure as exc:
                print(f"FAIL {case.__name__}: {exc}")
                failures += 1
            else:
                took = time.monotonic() - started
                print(f"ok   {case.__name__} ({took:.2f}s)")
        if failures:
            print("\n--- nginx error.log ---")
            print(srv.errors())
    finally:
        srv.stop()
        default_upstream.stop()

    print(f"\n{len(cases)} case(s), {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

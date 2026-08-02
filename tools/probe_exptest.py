#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Drive web/__probe_exptest.html (or any probe page) in headless Chrome over CDP.

Written against the raw DevTools protocol rather than Playwright so it needs no
package install: Chrome is launched with --remote-debugging-port, the target list
is read over HTTP, and Runtime.evaluate is issued over the websocket. The only
non-stdlib piece would be a websocket client, so a minimal RFC6455 client is
implemented inline (single-frame text messages, no extensions, no fragmentation
on send — enough for evaluate/await round trips).

Usage:
    python3 tools/probe_exptest.py [--page /__probe_exptest.html]
                                   [--var __exptest] [--shots-dir /tmp/shots]
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request

CHROME_CANDIDATES = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    shutil.which("chromium") or "",
    shutil.which("google-chrome") or "",
]


def find_chrome() -> str:
    for c in CHROME_CANDIDATES:
        if c and os.path.exists(c):
            return c
    raise SystemExit("no Chrome/Chromium binary found")


class WS:
    """Minimal websocket client: text frames, client-masked, no fragmentation."""

    def __init__(self, url: str) -> None:
        assert url.startswith("ws://")
        rest = url[len("ws://") :]
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self.sock = socket.create_connection((host, int(port or 80)), timeout=60)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            self.buf += self.sock.recv(4096)
        head, _, self.buf = self.buf.partition(b"\r\n\r\n")
        if b"101" not in head.split(b"\r\n")[0]:
            raise RuntimeError(f"websocket handshake failed: {head[:200]!r}")

    def send(self, text: str) -> None:
        payload = text.encode()
        n = len(payload)
        header = bytearray([0x81])  # FIN + text
        mask_bit = 0x80
        if n < 126:
            header.append(mask_bit | n)
        elif n < 65536:
            header.append(mask_bit | 126)
            header += struct.pack(">H", n)
        else:
            header.append(mask_bit | 127)
            header += struct.pack(">Q", n)
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def _read(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("websocket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self) -> str:
        """Reassemble one message; server frames are never masked."""
        chunks = []
        while True:
            b0, b1 = self._read(2)
            fin = b0 & 0x80
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            chunks.append(self._read(length))
            if fin:
                return b"".join(chunks).decode("utf-8", "replace")


class CDP:
    def __init__(self, ws_url: str) -> None:
        self.ws = WS(ws_url)
        self.next_id = 1

    def call(self, method: str, params: dict | None = None, timeout: float = 180.0):
        mid = self.next_id
        self.next_id += 1
        self.ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == mid:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg["result"]
        raise TimeoutError(method)

    # Named after the CDP method it wraps (Runtime.evaluate), not Python's eval().
    # Every expression passed in is a literal written in this file and runs inside
    # a throwaway headless browser against a page we serve ourselves, so there is
    # no untrusted input anywhere in the path.
    def eval(self, expr: str, await_promise: bool = False):
        r = self.call(
            "Runtime.evaluate",
            {
                "expression": expr,
                "returnByValue": True,
                "awaitPromise": await_promise,
                "allowUnsafeEvalBlockedByCSP": True,
            },
        )
        if r.get("exceptionDetails"):
            raise RuntimeError(json.dumps(r["exceptionDetails"])[:800])
        return r["result"].get("value")


def launch(url: str, port: int) -> tuple[subprocess.Popen, CDP]:
    profile = tempfile.mkdtemp(prefix="exptest-chrome-")
    proc = subprocess.Popen(
        [
            find_chrome(),
            "--headless=new",
            f"--remote-debugging-port={port}",
            f"--user-data-dir={profile}",
            "--no-first-run",
            "--no-default-browser-check",
            "--disable-gpu",
            "--mute-audio",
            "--window-size=1024,900",
            url,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    ws_url = None
    for _ in range(120):
        try:
            targets = json.loads(
                urllib.request.urlopen(f"http://127.0.0.1:{port}/json", timeout=2).read()
            )
            for t in targets:
                if t.get("type") == "page" and "__probe" in t.get("url", ""):
                    ws_url = t["webSocketDebuggerUrl"]
                    break
            if ws_url:
                break
        except Exception:
            pass
        time.sleep(0.5)
    if not ws_url:
        proc.kill()
        raise SystemExit("could not attach to the probe page")
    return proc, CDP(ws_url)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--base", default="http://localhost:8000")
    p.add_argument("--page", default="/__probe_exptest.html")
    p.add_argument("--var", default="__exptest", help="window property the probe fills in")
    p.add_argument("--port", type=int, default=9222)
    p.add_argument("--timeout", type=float, default=180.0)
    p.add_argument("--shots-dir", default="")
    p.add_argument("--json-out", default="")
    args = p.parse_args()

    proc, cdp = launch(args.base + args.page, args.port)
    try:
        cdp.call("Runtime.enable")
        deadline = time.time() + args.timeout
        payload = None
        while time.time() < deadline:
            payload = cdp.eval(
                f"(window.{args.var} && window.{args.var}.done) "
                f"? JSON.stringify({{results: window.{args.var}.results, "
                f"error: window.{args.var}.error, "
                f"seen: window.{args.var}.seen, "
                f"trace: window.{args.var}.trace, "
                f"shotKeys: Object.keys(window.{args.var}.shots || {{}})}}) : null"
            )
            if payload:
                break
            time.sleep(0.5)
        if not payload:
            # surface whatever the page managed to log before it stalled
            tail = cdp.eval("document.getElementById('log') && document.getElementById('log').textContent")
            raise SystemExit(f"probe did not finish in {args.timeout}s. log tail:\n{tail}")

        data = json.loads(payload)
        print(json.dumps(data, indent=2, ensure_ascii=False))

        if args.shots_dir and data.get("shotKeys"):
            d = pathlib.Path(args.shots_dir)
            d.mkdir(parents=True, exist_ok=True)
            for k in data["shotKeys"]:
                b64 = cdp.eval(f"window.{args.var}.shots[{json.dumps(k)}].split(',')[1]")
                (d / f"{k}.png").write_bytes(base64.b64decode(b64))
                print(f"shot: {d / (k + '.png')}")

        if args.json_out:
            pathlib.Path(args.json_out).write_text(json.dumps(data, indent=2))
        return 1 if data.get("error") else 0
    finally:
        proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())

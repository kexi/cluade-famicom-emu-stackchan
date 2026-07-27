#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Serve web/ and relay cartridge pins, console control and pad input to a CoreS3.

The browser cannot send UDP, so the page POSTs here and this process forwards
each request as the packet the firmware already parses. Serving the page from the
same origin is what keeps those POSTs out of CORS preflight territory.

    POST /api/pins    {"host":..., "mask":"<16 hex>"}     -> type 1
    POST /api/reset   {"host":...}                        -> type 2, RESET
    POST /api/volume  {"host":..., "volume":0-255}        -> type 2, volume
    POST /api/pad     {"host":..., "pad1":0-255, "pad2":} -> type 0 (as procon)

Usage:
    uv run tools/serve_web.py [--port 8000] [--device-port 5555] [--bind ADDR]

Then open http://localhost:8000/?device=<CoreS3 IP>.

Binds to 127.0.0.1 by default: this relays to arbitrary hosts, so it should not
be reachable from off-machine unless you deliberately want a phone to drive it
(--bind 0.0.0.0), on a network you trust.
"""

import argparse
import json
import re
import socket
import struct
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PROTOCOL_MAGIC = b"NP"
PROTOCOL_VERSION = 1
TYPE_PAD = 0
TYPE_PINS = 1
TYPE_CTRL = 2
CTRL_RESET = 0x01
CTRL_VOLUME = 0x02

# Keep the relay from being turned into a general-purpose packet cannon.
HOST_RE = re.compile(r"^\d{1,3}(\.\d{1,3}){3}$")
MASK_RE = re.compile(r"^[0-9a-fA-F]{1,16}$")
MAX_BODY_BYTES = 4096


def _is_byte(v) -> bool:
    """True for a plain int in 0-255.

    bool is an int subclass, so it is rejected explicitly — otherwise True would
    silently pass as the value 1.
    """
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 255


def build_pin_packet(seq: int, mask: int) -> bytes:
    """Pack one 14-byte pin-state frame.

    Layout: magic 'NP' | version | type=1 | seq (uint16 LE) | mask (uint64 LE).
    Byte [3] is the type field the firmware switches on; the controller packet
    leaves it zero, which is why pads keep working unchanged.
    """
    return struct.pack(
        "<2sBBHQ",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        TYPE_PINS,
        seq & 0xFFFF,
        mask & 0xFFFFFFFFFFFFFFFF,
    )


def build_pad_packet(seq: int, pad1: int, pad2: int = 0) -> bytes:
    """Pack one 8-byte controller frame.

    Byte-for-byte the packet tools/procon_udp.py sends, so the browser and a real
    Pro Controller are indistinguishable to the firmware. Byte [3] stays 0, which
    the firmware reads as type 0.

    Buttons: bit0:A 1:B 2:Select 3:Start 4:Up 5:Down 6:Left 7:Right.
    """
    return struct.pack(
        "<2sBBHBB",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        TYPE_PAD,
        seq & 0xFFFF,
        pad1 & 0xFF,
        pad2 & 0xFF,
    )


def build_ctrl_packet(seq: int, cmd: int, arg: int = 0) -> bytes:
    """Pack one 8-byte console-control frame.

    Layout: magic 'NP' | version | type=2 | seq (uint16 LE) | cmd | arg.
    cmd bit 0 = press RESET, bit 1 = set master volume (level in arg, 0-255).

    RESET exists because pulling a CPU-bus pin can leave the emulated program
    wedged, and reseating the cartridge alone will not recover it — the console
    needs the reset vector re-fetched, exactly as on real hardware.
    """
    return struct.pack(
        "<2sBBHBB",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        TYPE_CTRL,
        seq & 0xFFFF,
        cmd & 0xFF,
        arg & 0xFF,
    )


class PinRelay:
    """Owns the UDP socket and the packet sequence counter."""

    def __init__(self, device_port: int):
        self.device_port = device_port
        self.seq = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def _send(self, host: str, packet: bytes) -> bytes:
        self.seq = (self.seq + 1) & 0xFFFF
        self.sock.sendto(packet, (host, self.device_port))
        return packet

    def send(self, host: str, mask: int) -> bytes:
        return self._send(host, build_pin_packet(self.seq, mask))

    def send_reset(self, host: str) -> bytes:
        return self._send(host, build_ctrl_packet(self.seq, CTRL_RESET))

    def send_volume(self, host: str, volume: int) -> bytes:
        return self._send(host, build_ctrl_packet(self.seq, CTRL_VOLUME, volume))

    def send_pad(self, host: str, pad1: int, pad2: int) -> bytes:
        return self._send(host, build_pad_packet(self.seq, pad1, pad2))


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, relay: PinRelay, **kwargs):
        self.relay = relay
        super().__init__(*args, **kwargs)

    def _reply(self, code: int, payload: dict):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        route = self.path.split("?")[0]
        if route not in ("/api/pins", "/api/reset", "/api/volume", "/api/pad"):
            self._reply(404, {"error": "not found"})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._reply(400, {"error": "bad content-length"})
            return
        if length <= 0 or length > MAX_BODY_BYTES:
            self._reply(400, {"error": "bad content-length"})
            return

        try:
            payload = json.loads(self.rfile.read(length))
        except (ValueError, UnicodeDecodeError):
            self._reply(400, {"error": "bad json"})
            return

        host = str(payload.get("host", ""))
        if not HOST_RE.match(host):
            self._reply(400, {"error": "bad host"})
            return

        if route == "/api/reset":
            try:
                packet = self.relay.send_reset(host)
            except OSError as exc:
                self._reply(502, {"error": f"send failed: {exc}"})
                return
            self._reply(200, {"sent": len(packet), "host": host, "reset": True})
            return

        if route == "/api/pad":
            pad1 = payload.get("pad1")
            pad2 = payload.get("pad2", 0)
            if not _is_byte(pad1):
                self._reply(400, {"error": "bad pad1"})
                return
            if not _is_byte(pad2):
                self._reply(400, {"error": "bad pad2"})
                return
            try:
                packet = self.relay.send_pad(host, pad1, pad2)
            except OSError as exc:
                self._reply(502, {"error": f"send failed: {exc}"})
                return
            self._reply(200, {"sent": len(packet), "host": host, "pad1": pad1, "pad2": pad2})
            return

        if route == "/api/volume":
            raw = payload.get("volume")
            if not _is_byte(raw):
                self._reply(400, {"error": "bad volume"})
                return
            try:
                packet = self.relay.send_volume(host, raw)
            except OSError as exc:
                self._reply(502, {"error": f"send failed: {exc}"})
                return
            self._reply(200, {"sent": len(packet), "host": host, "volume": raw})
            return

        mask_text = str(payload.get("mask", ""))
        if not MASK_RE.match(mask_text):
            self._reply(400, {"error": "bad mask"})
            return

        try:
            packet = self.relay.send(host, int(mask_text, 16))
        except OSError as exc:
            self._reply(502, {"error": f"send failed: {exc}"})
            return

        self._reply(200, {"sent": len(packet), "host": host, "mask": mask_text})

    def log_message(self, fmt, *args):
        # The default logs every static asset, which buries the pin traffic.
        if "/api/" in (self.path or ""):
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8000, help="HTTP port")
    parser.add_argument("--device-port", type=int, default=5555, help="CoreS3 UDP port")
    parser.add_argument("--dir", default=None, help="directory to serve (default: web/)")
    parser.add_argument(
        "--bind",
        default="127.0.0.1",
        help="address to bind (default: 127.0.0.1). Use 0.0.0.0 to reach it from "
             "a phone on the same LAN — see the README warning first.",
    )
    args = parser.parse_args()

    root = Path(args.dir) if args.dir else Path(__file__).resolve().parent.parent / "web"
    if not root.is_dir():
        print(f"no such directory: {root}", file=sys.stderr)
        return 1

    relay = PinRelay(args.device_port)
    handler = partial(Handler, relay=relay, directory=str(root))
    server = ThreadingHTTPServer((args.bind, args.port), handler)
    print(f"serving {root} at http://{args.bind}:{args.port}")
    print(f"relay -> UDP :{args.device_port}  (open /?device=<CoreS3 IP>)")
    if args.bind != "127.0.0.1":
        print(f"WARNING: bound to {args.bind} — anyone on this network can drive "
              "the device and reach any host it can. Use only on a network you trust.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

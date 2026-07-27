#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Serve web/ and relay cartridge-pin state to a CoreS3 over UDP.

The browser cannot send UDP, so the connector UI POSTs its pin mask here and
this process forwards it as the same type=1 packet the firmware already parses.
Serving the page from the same origin is what keeps that POST out of CORS
preflight territory.

Usage:
    uv run tools/serve_web.py [--port 8000] [--device-port 5555]

Then open http://localhost:8000/?device=<CoreS3 IP>.

Binds to 127.0.0.1 only: this relays to arbitrary hosts on the LAN, so it must
not be reachable from off-machine.
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
TYPE_PINS = 1
TYPE_CTRL = 2
CTRL_RESET = 0x01
CTRL_VOLUME = 0x02

# Keep the relay from being turned into a general-purpose packet cannon.
HOST_RE = re.compile(r"^\d{1,3}(\.\d{1,3}){3}$")
MASK_RE = re.compile(r"^[0-9a-fA-F]{1,16}$")
MAX_BODY_BYTES = 4096


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
        if route not in ("/api/pins", "/api/reset", "/api/volume"):
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

        if route == "/api/volume":
            raw = payload.get("volume")
            # bool is an int subclass, so reject it explicitly rather than
            # silently accepting True as volume 1.
            valid = isinstance(raw, int) and not isinstance(raw, bool) and 0 <= raw <= 255
            if not valid:
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
    args = parser.parse_args()

    root = Path(args.dir) if args.dir else Path(__file__).resolve().parent.parent / "web"
    if not root.is_dir():
        print(f"no such directory: {root}", file=sys.stderr)
        return 1

    relay = PinRelay(args.device_port)
    handler = partial(Handler, relay=relay, directory=str(root))
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(f"serving {root} at http://localhost:{args.port}")
    print(f"pin relay -> UDP :{args.device_port}  (open /?device=<CoreS3 IP>)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

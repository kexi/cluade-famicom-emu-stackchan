#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Serve web/ and relay cartridge pins, console control and debug reads to a CoreS3.

The browser cannot send UDP, so the page POSTs here and this process forwards
each request as the packet the firmware already parses. Serving the page from the
same origin is what keeps those POSTs out of CORS preflight territory.

    POST /api/pins    {"host":..., "mask":"<16 hex>"}  -> type 1
    POST /api/reset   {"host":...}                     -> type 2, RESET
    POST /api/volume  {"host":..., "volume":0-255}     -> type 2, volume
    POST /api/debug   {"host":...}                     -> type 3, and returns the
                      reassembled snapshot as application/octet-stream (504 if
                      the device does not answer)
    POST /api/rom?host=<ip>&swap=0|1[&progress=1]      -> type 4, body is the raw
                      .nes image (not JSON, not base64); relayed as a
                      BEGIN/DATA*/END transfer and answered once it lands.
                      With progress=1 the answer is an NDJSON stream of
                      {"sent":..,"chunks":..} lines whose last line carries the
                      verdict; without it the answer is a single JSON object and
                      an HTTP status code, unchanged, for curl.

Usage:
    uv run tools/serve_web.py [--port 8000] [--device-port 5555]

Then open http://localhost:8000/?device=<CoreS3 IP>.

Binds to 127.0.0.1 only: this relays to arbitrary hosts on the LAN, so it must
not be reachable from off-machine.
"""

import argparse
import json
import random
import re
import socket
import struct
import sys
import time
import zlib
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

PROTOCOL_MAGIC = b"NP"
PROTOCOL_VERSION = 1
TYPE_PINS = 1
TYPE_CTRL = 2
TYPE_DEBUG = 3
TYPE_ROM = 4
CTRL_RESET = 0x01
CTRL_VOLUME = 0x02

ROM_OP_BEGIN = 0
ROM_OP_DATA = 1
ROM_OP_END = 2
ROM_OP_ABORT = 3
ROM_FLAG_SWAP = 0x01
# Same MTU reasoning as the debug reply chunking: stays inside one Ethernet frame.
ROM_CHUNK = 1400
ROM_DATA_HEADER = 12
ROM_MAX_SIZE = 1024 * 1024
# Stop-and-wait: the device answers from the UDP task, so the turnaround is a LAN
# round trip rather than a frame boundary — but reuse the debug budget anyway so a
# busy device does not get declared dead.
ROM_TIMEOUT_S = 0.3
ROM_RETRIES = 8
# One transfer is ~750 chunks; allow a few percent loss before giving up rather
# than letting a flaky link retry forever.
ROM_MAX_TOTAL_RETRIES = 64
# Progress thinning. One line per chunk would be ~750 writes for a large cart,
# which costs more than the transfer itself on the wire and gives the page
# nothing a human can read; a step or an interval, whichever comes first, keeps
# the bar moving smoothly even on a slow link.
ROM_PROGRESS_STEP = 0.03
ROM_PROGRESS_INTERVAL_S = 0.05

ACK_MAGIC = b"NR"
ACK_SIZE = 12
ROM_STATUS_OK = 0
ROM_STATUS_NAMES = {
    0: "OK",
    1: "BUSY",
    2: "TOO_BIG",
    3: "ALLOC",
    4: "SEQ",
    5: "SIZE_MISMATCH",
    6: "CRC",
    7: "BAD_HEADER",
    8: "UNSUPPORTED_MAPPER",
    9: "NO_SESSION",
}
ROM_STATUS_SEQ = 4
# How each device verdict surfaces to the page. 409 for BUSY so a retry reads as
# "try again", 422 for the checks that condemn the image itself.
ROM_STATUS_HTTP = {
    1: 409,
    2: 413,
    3: 502,
    5: 422,
    6: 422,
    7: 422,
    8: 422,
    9: 502,
}

DEBUG_MAGIC = b"ND"
DEBUG_HEADER = 7
DEBUG_FLAG_WAVES = 0x01
# Measured round trip is ~50ms median, ~160ms p90 (the device answers on a frame
# boundary), so 300ms clears the tail without making a dead device feel hung.
DEBUG_TIMEOUT_S = 0.3

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


def build_rom_begin(session: int, size: int, crc: int, swap: bool) -> bytes:
    """Pack the 16-byte BEGIN frame that opens a ROM transfer.

    Layout: magic 'NP' | version | type=4 | session (uint16 LE) | op=0 |
    flags | size (uint32 LE) | crc32 (uint32 LE). The size and CRC go up front so
    the device can reject an oversized image before allocating anything, and can
    verify the whole thing at END without keeping a running hash.
    """
    return struct.pack(
        "<2sBBHBBII",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        TYPE_ROM,
        session & 0xFFFF,
        ROM_OP_BEGIN,
        ROM_FLAG_SWAP if swap else 0,
        size & 0xFFFFFFFF,
        crc & 0xFFFFFFFF,
    )


def build_rom_data(session: int, index: int, payload: bytes) -> bytes:
    """Pack one DATA frame: 12-byte header then the chunk itself.

    The payload length is carried explicitly rather than derived from the
    datagram size, so the device can reject a truncated frame instead of
    silently storing a short chunk.
    """
    return struct.pack(
        "<2sBBHBBHH",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        TYPE_ROM,
        session & 0xFFFF,
        ROM_OP_DATA,
        0,
        index & 0xFFFF,
        len(payload),
    ) + payload


def build_rom_mark(session: int, op: int) -> bytes:
    """Pack an 8-byte END or ABORT frame (no payload, no arguments)."""
    return struct.pack(
        "<2sBBHBB",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        TYPE_ROM,
        session & 0xFFFF,
        op,
        0,
    )


def parse_rom_ack(reply: bytes, session: int, op: int):
    """Decode a 12-byte ACK, or None if it is not this frame's answer.

    Layout: magic 'NR' | version | op echo | session (uint16 LE) |
    chunk index echo (uint16 LE) | status | expected index (uint16 LE) | 0.
    """
    if len(reply) < ACK_SIZE or reply[0:2] != ACK_MAGIC:
        return None
    if reply[2] != PROTOCOL_VERSION or reply[3] != op:
        return None
    if (reply[4] | (reply[5] << 8)) != session:
        return None
    return {
        "index": reply[6] | (reply[7] << 8),
        "status": reply[8],
        "expected": reply[9] | (reply[10] << 8),
    }


class RomTransferError(Exception):
    """A transfer that ended with a verdict rather than an exception.

    `status` is the device status byte, or None when the device never answered.
    """

    def __init__(self, status, message: str):
        super().__init__(message)
        self.status = status


def _progress_thinner(on_progress, total: int):
    """Wrap a progress callback so it fires on a step or an interval, not per chunk.

    Returns `report(sent, force=False)`. `force` is for the first and last line,
    which must always go out — a bar that never reaches 100% reads as a hang even
    when the transfer succeeded.
    """
    if on_progress is None:
        return lambda sent, force=False: None

    state = {"sent": -1, "at": 0.0}
    step = max(1, int(total * ROM_PROGRESS_STEP))

    def report(sent: int, force: bool = False):
        now = time.monotonic()
        stepped = sent - state["sent"] >= step
        waited = now - state["at"] >= ROM_PROGRESS_INTERVAL_S
        if not force and not stepped and not waited:
            return
        state["sent"] = sent
        state["at"] = now
        on_progress(sent, total)

    return report


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

    def fetch_debug(self, host: str, waves: bool = False):
        """Query a debug snapshot and reassemble the reply datagrams.

        Returns the snapshot bytes, or None on timeout / incomplete reply. Uses
        its own socket so a reply cannot be confused with anything else in
        flight, and matches on the echoed seq so a late answer to a previous
        query is discarded rather than served as current state.

        The part count comes from the datagrams themselves rather than a
        constant, because a wave-bearing snapshot needs more of them.
        """
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFFFF
        flags = DEBUG_FLAG_WAVES if waves else 0
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.settimeout(DEBUG_TIMEOUT_S)
            sock.sendto(
                struct.pack("<2sBBHBB", PROTOCOL_MAGIC, PROTOCOL_VERSION,
                            TYPE_DEBUG, seq, flags, 0),
                (host, self.device_port),
            )
            parts: dict[int, bytes] = {}
            nparts = None
            deadline = time.monotonic() + DEBUG_TIMEOUT_S
            while time.monotonic() < deadline:
                if nparts is not None and len(parts) >= nparts:
                    break
                sock.settimeout(max(0.001, deadline - time.monotonic()))
                try:
                    data, _ = sock.recvfrom(2048)
                except socket.timeout:
                    break
                if len(data) < DEBUG_HEADER or data[0:2] != DEBUG_MAGIC:
                    continue
                if data[2] != PROTOCOL_VERSION:
                    continue
                if (data[5] | (data[6] << 8)) != seq:
                    continue      # stale answer to an earlier query
                nparts = data[4]
                parts[data[3]] = data[DEBUG_HEADER:]
            if not nparts or len(parts) != nparts:
                return None
            return b"".join(parts[i] for i in sorted(parts))
        finally:
            sock.close()

    def send_rom(self, host: str, data: bytes, swap: bool, on_progress=None) -> dict:
        """Push a whole .nes image to the device and wait for it to land.

        Stop-and-wait: every frame is acknowledged before the next goes out. A
        LAN round trip is 2-5ms, so even a 768KB cart finishes in a couple of
        seconds and a window would only buy complexity. Uses its own socket for
        the same reason fetch_debug does — an ACK must not be mistaken for, or
        consumed by, anything else in flight.

        `on_progress(sent, total)` is called as chunks land, already thinned to
        the rate below — callers that just want the verdict leave it None.

        Raises RomTransferError when the device refuses or goes silent, OSError
        when the send itself fails.
        """
        session = random.randrange(1, 0x10000)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        chunks = [data[i:i + ROM_CHUNK] for i in range(0, len(data), ROM_CHUNK)]
        started = time.monotonic()
        retries = 0
        report = _progress_thinner(on_progress, len(chunks))

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.settimeout(ROM_TIMEOUT_S)
            begin = build_rom_begin(session, len(data), crc, swap)
            retries += self._rom_exchange(sock, host, session, ROM_OP_BEGIN, 0, begin)
            report(0, force=True)

            index = 0
            while index < len(chunks):
                packet = build_rom_data(session, index, chunks[index])
                ack = self._rom_send_until_ack(sock, host, session, ROM_OP_DATA, index, packet)
                retries += ack["retries"]
                is_out_of_order = ack["status"] == ROM_STATUS_SEQ
                if is_out_of_order:
                    # The device tells us where it actually is; rewinding beats
                    # aborting, because the usual cause is a dropped ACK that
                    # left us one chunk ahead of its cursor.
                    expected = ack["expected"]
                    if expected > len(chunks):
                        raise RomTransferError(ROM_STATUS_SEQ, "device asked for a chunk past the end")
                    index = expected
                    retries += 1
                    if retries > ROM_MAX_TOTAL_RETRIES:
                        raise RomTransferError(None, "too many retries")
                    continue
                if ack["status"] != ROM_STATUS_OK:
                    raise RomTransferError(ack["status"], ROM_STATUS_NAMES.get(ack["status"], "?"))
                index += 1
                report(index, force=index == len(chunks))
                if retries > ROM_MAX_TOTAL_RETRIES:
                    raise RomTransferError(None, "too many retries")

            end = build_rom_mark(session, ROM_OP_END)
            retries += self._rom_exchange(sock, host, session, ROM_OP_END, 0, end)
            return {
                "ok": True,
                "bytes": len(data),
                "chunks": len(chunks),
                "retries": retries,
                "ms": int((time.monotonic() - started) * 1000),
            }
        finally:
            sock.close()

    def _rom_exchange(self, sock, host, session, op, index, packet) -> int:
        """Send one frame, wait for its ACK, and insist the status is OK.

        Returns the number of retries it took. Used for BEGIN and END, where a
        non-OK status is always terminal — DATA is the only op with a status
        (SEQ) worth recovering from.
        """
        ack = self._rom_send_until_ack(sock, host, session, op, index, packet)
        if ack["status"] != ROM_STATUS_OK:
            raise RomTransferError(ack["status"], ROM_STATUS_NAMES.get(ack["status"], "?"))
        return ack["retries"]

    def _rom_send_until_ack(self, sock, host, session, op, index, packet) -> dict:
        """Send one frame and read the matching ACK, retransmitting on silence.

        Datagrams that are not this session's answer to this op are dropped and
        the read continues within the same timeout window, so a late ACK from an
        earlier frame cannot be mistaken for the current one.
        """
        for attempt in range(ROM_RETRIES):
            sock.sendto(packet, (host, self.device_port))
            deadline = time.monotonic() + ROM_TIMEOUT_S
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                sock.settimeout(remaining)
                try:
                    reply, _ = sock.recvfrom(64)
                except socket.timeout:
                    break
                ack = parse_rom_ack(reply, session, op)
                if ack is None:
                    continue      # stale or foreign datagram
                ack["retries"] = attempt
                return ack
        raise RomTransferError(None, "device did not answer")


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

    def _post_rom(self):
        """Relay a raw .nes body to the device as a type=4 transfer.

        Kept off the JSON path: the body is a megabyte of binary, which base64 in
        a JSON envelope would only inflate, and MAX_BODY_BYTES exists to stop
        exactly that kind of body on the control routes.
        """
        query = parse_qs(urlparse(self.path).query)
        host = (query.get("host") or [""])[0]
        if not HOST_RE.match(host):
            self._reply(400, {"error": "bad host"})
            return
        swap = (query.get("swap") or ["0"])[0] == "1"

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._reply(400, {"error": "bad content-length"})
            return
        if length <= 0:
            self._reply(400, {"error": "bad content-length"})
            return
        if length > ROM_MAX_SIZE:
            self._reply(413, {"error": "rom too large"})
            return

        data = self.rfile.read(length)
        if len(data) != length:
            self._reply(400, {"error": "short body"})
            return

        wants_progress = (query.get("progress") or ["0"])[0] == "1"
        if wants_progress:
            self._stream_rom(host, data, swap)
            return

        try:
            result = self.relay.send_rom(host, data, swap)
        except RomTransferError as exc:
            code, payload = self._rom_failure(exc)
            self._reply(code, payload)
            return
        except OSError as exc:
            self._reply(502, {"error": f"send failed: {exc}"})
            return

        self._reply(200, result)

    @staticmethod
    def _rom_failure(exc: RomTransferError):
        """Map a transfer verdict onto (HTTP code, payload).

        Shared by both answer shapes so the streaming path reports the same
        verdict the synchronous one puts in the status line — it just carries the
        code in the body, having already committed to 200 in the headers.
        """
        if exc.status is None:
            # 504 rather than 502: the packets went out fine, the device just
            # never answered — the page offers a retry for that.
            return 504, {"error": str(exc)}
        return ROM_STATUS_HTTP.get(exc.status, 502), {
            "error": ROM_STATUS_NAMES.get(exc.status, "?"),
            "status": exc.status,
        }

    def _stream_rom(self, host: str, data: bytes, swap: bool):
        """Answer a transfer as NDJSON: progress lines, then one verdict line.

        Length is unknown when the headers go out, so this closes the connection
        to delimit the body rather than chunk-encoding it — the page reads with a
        stream reader either way, and curl -N shows the lines as they land.
        """
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        def emit(payload: dict):
            self.wfile.write(json.dumps(payload).encode() + b"\n")
            self.wfile.flush()

        def on_progress(sent: int, chunks: int):
            emit({"sent": sent, "chunks": chunks})

        try:
            result = self.relay.send_rom(host, data, swap, on_progress=on_progress)
        except RomTransferError as exc:
            code, payload = self._rom_failure(exc)
            emit({"error": payload["error"], "http": code})
            return
        except OSError as exc:
            emit({"error": f"send failed: {exc}", "http": 502})
            return

        emit(result)

    def do_POST(self):
        route = self.path.split("?")[0]
        if route == "/api/rom":
            self._post_rom()
            return

        if route not in ("/api/pins", "/api/reset", "/api/volume", "/api/debug"):
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

        if route == "/api/debug":
            snapshot = self.relay.fetch_debug(host, waves=bool(payload.get("waves")))
            if snapshot is None:
                # 504, not 502: the request was sent fine, the device just did
                # not answer in time — which the page retries rather than treats
                # as a configuration error.
                self._reply(504, {"error": "device did not answer"})
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(snapshot)))
            self.end_headers()
            self.wfile.write(snapshot)
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

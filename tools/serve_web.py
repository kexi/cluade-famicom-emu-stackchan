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
                      &save=<file>[&load=0] additionally writes the image to the
                      device's SD card; the verdict then carries "sd" with the
                      card's own status. load=0 saves without disturbing what is
                      running, and requires a save name.
    POST /api/rom/url?host=<ip>&url=<http(s)>&...      -> the same transfer, with
                      this process doing the download. The device has no TLS
                      stack, which is the whole reason the fetch lives here.
                      Takes the same swap/progress/save/load parameters.
    POST /api/sd/list   {"host":...}                   -> type 5 LIST, answered
                      with the listing plus the card's capacity
    POST /api/sd/load   {"host":..., "name":...}       -> type 5 LOAD
    POST /api/sd/delete {"host":..., "name":...}       -> type 5 DELETE
    POST /api/sd/rename {"host":..., "name":..., "to":...}  -> type 5 RENAME

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
import urllib.error
import urllib.request
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
TYPE_SD = 5
CTRL_RESET = 0x01
CTRL_VOLUME = 0x02

ROM_OP_BEGIN = 0
ROM_OP_DATA = 1
ROM_OP_END = 2
ROM_OP_ABORT = 3
ROM_FLAG_SWAP = 0x01
ROM_FLAG_SAVE_SD = 0x02
ROM_FLAG_NO_LOAD = 0x04
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

# The device reports an attempted SD write in its own datagram rather than in the
# END ACK: the ACK goes out from the UDP task as soon as the CRC checks, while the
# card write happens later on core 1 at a frame boundary. See UDP_ROM_SAVE_EVENT_SIZE
# in m5stack/src/config.h.
#   'N','S' | version | TYPE_ROM | session u16 LE | SdStatus | 0
SD_EVENT_MAGIC = b"NS"
SD_ACK_SIZE = 8
# Generous against the ROM ACK budget: the reply comes from core 1 at a frame
# boundary, and a save of a megabyte can hold that loop for a second or more.
SD_SAVE_TIMEOUT_S = 6.0

SD_OP_LIST = 0
SD_OP_LOAD = 1
SD_OP_DELETE = 2
SD_OP_RENAME = 3
SD_HEADER = 8
# LIST reply header, ahead of the entries: part | nparts | total u16 LE |
# count u16 LE | totalBytes u64 LE | freeBytes u64 LE.
SD_LIST_HEADER = 30
# Every type 5 reply is produced on core 1 at a frame boundary, so the turnaround
# is a frame rather than a LAN round trip — and a LOAD reads a whole image off the
# card before it answers. Retries stay few because the request is idempotent for
# LIST but emphatically not for DELETE/RENAME.
SD_TIMEOUT_S = 3.0
SD_RETRIES = 3
SD_STATUS_OK = 0
SD_STATUS_BUSY = 7
# Overall budget for an idempotent op that keeps answering BUSY. The firmware
# serves one type 5 request at a time and answers BUSY to everything else while
# core 1 works, including a retransmission of the very request it is running, so
# a LOAD slow enough to outlast SD_TIMEOUT_S would otherwise be reported as a
# failure at the moment it is about to succeed. Waiting instead of failing needs
# a bound, and this is it.
SD_BUSY_DEADLINE_S = 12.0
# Gap between retransmissions while the device says BUSY. Short enough that the
# answer is picked up promptly once core 1 is free, long enough that the wait is
# not a busy-loop on the device's receive path.
SD_BUSY_RETRY_S = 0.25
# Returned by the receive helpers for "the device said BUSY, ask again". A
# sentinel rather than None because the two mean opposite things to the caller:
# None is silence and spends a retransmission attempt, this is an answer and
# does not.
_SD_BUSY = object()
SD_STATUS_NAMES = {
    0: "OK",
    1: "NOT_MOUNTED",
    2: "NOT_FOUND",
    3: "NO_SPACE",
    4: "TOO_BIG",
    5: "BAD_ROM",
    6: "BAD_NAME",
    7: "BUSY",
    8: "IO_ERROR",
    9: "EXISTS",
}
# Human-readable form, mirroring sdStatusText() on the device so a curl user and
# the page read the same words for the same code.
SD_STATUS_TEXT = {
    0: "ok",
    1: "no SD card mounted",
    2: "file not found",
    3: "not enough space on the card",
    4: "ROM is larger than the device accepts",
    5: "not a supported iNES image",
    6: "invalid file name",
    7: "the device is busy",
    8: "SD card I/O error",
    9: "a file of that name already exists",
}
# Same shape as ROM_STATUS_HTTP: 409 for the states a retry can clear, 404/409 for
# the ones about the named file, 422 for a condemned image.
SD_STATUS_HTTP = {
    1: 503,
    2: 404,
    3: 507,
    4: 413,
    5: 422,
    6: 400,
    7: 409,
    8: 502,
    9: 409,
}
# The longest name the firmware will handle, minus the NUL (SD_ROM_NAME_MAX).
SD_NAME_MAX = 63

# The device has no TLS stack, which is why the download happens here. Only http
# and https: a file:// or data: URL would turn this relay into a reader of the
# serving machine's disk for anyone who can reach the page.
ROM_URL_RE = re.compile(r"^https?://", re.IGNORECASE)
# Long enough for a slow mirror to start answering, short enough that a dead host
# does not hold an HTTP worker for a minute.
ROM_URL_TIMEOUT_S = 20.0
# Read in pieces so an oversized body is abandoned as soon as it passes the limit
# rather than after it has all been buffered — a server that lies in
# Content-Length, or omits it, cannot make this process swallow a DVD image.
ROM_URL_CHUNK = 64 * 1024

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


def build_rom_begin(
    session: int,
    size: int,
    crc: int,
    swap: bool,
    save_name: str | None = None,
    load: bool = True,
) -> bytes:
    """Pack the BEGIN frame that opens a ROM transfer.

    Layout: magic 'NP' | version | type=4 | session (uint16 LE) | op=0 |
    flags | size (uint32 LE) | crc32 (uint32 LE). The size and CRC go up front so
    the device can reject an oversized image before allocating anything, and can
    verify the whole thing at END without keeping a running hash.

    With `save_name`, a `nameLen | name` tail follows. The firmware discriminates
    on length, not on a flag — a sender that predates SD support emits exactly the
    16 bytes above — so the tail is appended only when there is a name to carry,
    and never as an empty field that would look new to old firmware for nothing.
    """
    flags = ROM_FLAG_SWAP if swap else 0
    if save_name:
        flags |= ROM_FLAG_SAVE_SD
    if not load:
        flags |= ROM_FLAG_NO_LOAD
    head = struct.pack(
        "<2sBBHBBII",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        TYPE_ROM,
        session & 0xFFFF,
        ROM_OP_BEGIN,
        flags,
        size & 0xFFFFFFFF,
        crc & 0xFFFFFFFF,
    )
    if not save_name:
        return head
    return head + _name_field(save_name)


def _name_field(name: str) -> bytes:
    """Pack one `nameLen u8 | name` field, as every name-bearing op carries it.

    Raises ValueError for anything the firmware would answer BadName to, so the
    rejection happens here with a message rather than after a round trip with a
    status code. Encoded as UTF-8 and measured in bytes, because the length byte
    counts bytes and a multi-byte name would otherwise overrun.
    """
    raw = name.encode("utf-8")
    if not raw or len(raw) > SD_NAME_MAX:
        raise ValueError("bad name")
    return bytes([len(raw)]) + raw


def build_sd_request(seq: int, op: int, *names: str) -> bytes:
    """Pack a type 5 request: the 8-byte header then one name field per argument.

    Layout: magic 'NP' | version | type=5 | seq (uint16 LE) | op | 0, then LOAD and
    DELETE append one name, RENAME two (from, to) and LIST none.
    """
    head = struct.pack(
        "<2sBBHBB",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        TYPE_SD,
        seq & 0xFFFF,
        op & 0xFF,
        0,
    )
    return head + b"".join(_name_field(n) for n in names)


def parse_sd_ack(reply: bytes, seq: int, op: int):
    """Decode an 8-byte type 5 reply, or None if it is not this request's answer.

    Layout: magic 'NS' | version | op echo | seq (uint16 LE) | SdStatus | 0.
    A LIST datagram starts with these same eight bytes and carries its header
    after them, so this is also the gate every LIST part passes through.
    """
    if len(reply) < SD_ACK_SIZE or reply[0:2] != SD_EVENT_MAGIC:
        return None
    if reply[2] != PROTOCOL_VERSION or reply[3] != op:
        return None
    if (reply[4] | (reply[5] << 8)) != seq:
        return None
    return {"status": reply[6]}


def parse_sd_list_part(reply: bytes):
    """Decode one LIST datagram into its header fields and entries.

    Returns None when the datagram is malformed — a short header, or entries that
    run past the bytes that actually arrived. Trusting the counts over the length
    would let a truncated datagram contribute a half-read name to the listing.
    """
    if len(reply) < SD_LIST_HEADER:
        return None
    part = reply[8]
    nparts = reply[9]
    total = reply[10] | (reply[11] << 8)
    count = reply[12] | (reply[13] << 8)
    total_bytes = int.from_bytes(reply[14:22], "little")
    free_bytes = int.from_bytes(reply[22:30], "little")
    if nparts == 0 or part >= nparts:
        return None

    files = []
    offset = SD_LIST_HEADER
    for _ in range(count):
        if offset + 5 > len(reply):
            return None
        size = int.from_bytes(reply[offset : offset + 4], "little")
        name_len = reply[offset + 4]
        offset += 5
        if offset + name_len > len(reply):
            return None
        name = reply[offset : offset + name_len].decode("utf-8", "replace")
        offset += name_len
        files.append({"name": name, "size": size})

    return {
        "part": part,
        "nparts": nparts,
        "total": total,
        "files": files,
        "total_bytes": total_bytes,
        "free_bytes": free_bytes,
    }


def parse_rom_save_event(reply: bytes, session: int):
    """Decode the separate datagram reporting how the SD write went.

    Layout: magic 'NS' | version | type=4 | session (uint16 LE) | SdStatus | 0.
    Shares its magic with the type 5 replies but carries TYPE_ROM in byte [3],
    which is what tells the two apart on a socket that could see either.
    """
    if len(reply) < SD_ACK_SIZE or reply[0:2] != SD_EVENT_MAGIC:
        return None
    if reply[2] != PROTOCOL_VERSION or reply[3] != TYPE_ROM:
        return None
    if (reply[4] | (reply[5] << 8)) != session:
        return None
    return {"status": reply[6]}


def build_rom_data(session: int, index: int, payload: bytes) -> bytes:
    """Pack one DATA frame: 12-byte header then the chunk itself.

    The payload length is carried explicitly rather than derived from the
    datagram size, so the device can reject a truncated frame instead of
    silently storing a short chunk.
    """
    return (
        struct.pack(
            "<2sBBHBBHH",
            PROTOCOL_MAGIC,
            PROTOCOL_VERSION,
            TYPE_ROM,
            session & 0xFFFF,
            ROM_OP_DATA,
            0,
            index & 0xFFFF,
            len(payload),
        )
        + payload
    )


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


class SdCommandError(Exception):
    """A type 5 command that came back with a status, or with nothing at all.

    `status` is the device's SdStatus byte, or None when it never answered — the
    same split RomTransferError makes, so the two failure paths can share the page's
    "is a retry worth it?" reasoning.
    """

    def __init__(self, status, message: str):
        super().__init__(message)
        self.status = status


class RomDownloadError(Exception):
    """The URL fetch failed before any packet went out.

    Distinct from RomTransferError because the device is blameless: nothing was
    sent, so the page should point at the URL rather than at the CoreS3.
    """

    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def fetch_rom_url(url: str) -> bytes:
    """Download a .nes over http(s) and vet it before a single packet goes out.

    The device has no TLS stack, so this process does the fetch — with ordinary
    certificate verification, which is the point of moving it here rather than
    bolting a trust store onto the firmware.

    Deliberately does not block private or loopback addresses: this is a LAN tool
    whose whole job is talking to hosts on the LAN, and a ROM served off a NAS is
    an ordinary thing to want. The scheme check is the boundary that matters —
    file:// and data: would read the serving machine's disk.
    """
    if not ROM_URL_RE.match(url):
        raise RomDownloadError(400, "url must start with http:// or https://")

    request = urllib.request.Request(url, headers={"User-Agent": "nes-relay/1"})
    try:
        with urllib.request.urlopen(request, timeout=ROM_URL_TIMEOUT_S) as response:
            # Believed only as an early "no", never as an allocation size: a
            # server can understate it, so the read below enforces the ceiling
            # again on the bytes that actually arrive.
            declared = response.headers.get("Content-Length")
            if declared and declared.isdigit() and int(declared) > ROM_MAX_SIZE:
                raise RomDownloadError(413, "rom too large")
            data = b""
            while len(data) <= ROM_MAX_SIZE:
                block = response.read(ROM_URL_CHUNK)
                if not block:
                    break
                data += block
    except urllib.error.HTTPError as exc:
        raise RomDownloadError(502, f"download failed: HTTP {exc.code}") from exc
    except urllib.error.URLError as exc:
        raise RomDownloadError(502, f"download failed: {exc.reason}") from exc
    except (TimeoutError, socket.timeout) as exc:
        raise RomDownloadError(504, "download timed out") from exc
    except OSError as exc:
        raise RomDownloadError(502, f"download failed: {exc}") from exc

    if len(data) > ROM_MAX_SIZE:
        raise RomDownloadError(413, "rom too large")
    # Checked here rather than left to the device: a 404 page served as HTML is
    # the usual outcome of a stale link, and pushing a megabyte of it over UDP
    # only to be told BAD_HEADER wastes the transfer and muddies the message.
    if len(data) < 16 or data[0:4] != b"NES\x1a":
        raise RomDownloadError(422, "not an iNES image")
    return data


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
                struct.pack(
                    "<2sBBHBB",
                    PROTOCOL_MAGIC,
                    PROTOCOL_VERSION,
                    TYPE_DEBUG,
                    seq,
                    flags,
                    0,
                ),
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
                    continue  # stale answer to an earlier query
                nparts = data[4]
                parts[data[3]] = data[DEBUG_HEADER:]
            if not nparts or len(parts) != nparts:
                return None
            return b"".join(parts[i] for i in sorted(parts))
        finally:
            sock.close()

    def send_rom(
        self,
        host: str,
        data: bytes,
        swap: bool,
        on_progress=None,
        save_name: str | None = None,
        load: bool = True,
    ) -> dict:
        """Push a whole .nes image to the device and wait for it to land.

        Stop-and-wait: every frame is acknowledged before the next goes out. A
        LAN round trip is 2-5ms, so even a 768KB cart finishes in a couple of
        seconds and a window would only buy complexity. Uses its own socket for
        the same reason fetch_debug does — an ACK must not be mistaken for, or
        consumed by, anything else in flight.

        `on_progress(sent, total)` is called as chunks land, already thinned to
        the rate below — callers that just want the verdict leave it None.

        With `save_name` the device also writes the image to its SD card, and the
        result comes back in a separate datagram after the END ACK (the card write
        happens on core 1, long after the UDP task acknowledged the CRC). The
        verdict then carries "sd" with that status; `load=False` asks for the save
        without disturbing whatever is running.

        Raises RomTransferError when the device refuses or goes silent, OSError
        when the send itself fails.
        """
        session = random.randrange(1, 0x10000)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        chunks = [data[i : i + ROM_CHUNK] for i in range(0, len(data), ROM_CHUNK)]
        started = time.monotonic()
        retries = 0
        report = _progress_thinner(on_progress, len(chunks))

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.settimeout(ROM_TIMEOUT_S)
            begin = build_rom_begin(session, len(data), crc, swap, save_name, load)
            retries += self._rom_exchange(sock, host, session, ROM_OP_BEGIN, 0, begin)
            report(0, force=True)

            index = 0
            while index < len(chunks):
                packet = build_rom_data(session, index, chunks[index])
                ack = self._rom_send_until_ack(
                    sock, host, session, ROM_OP_DATA, index, packet
                )
                retries += ack["retries"]
                is_out_of_order = ack["status"] == ROM_STATUS_SEQ
                if is_out_of_order:
                    # The device tells us where it actually is; rewinding beats
                    # aborting, because the usual cause is a dropped ACK that
                    # left us one chunk ahead of its cursor.
                    expected = ack["expected"]
                    if expected > len(chunks):
                        raise RomTransferError(
                            ROM_STATUS_SEQ, "device asked for a chunk past the end"
                        )
                    index = expected
                    retries += 1
                    if retries > ROM_MAX_TOTAL_RETRIES:
                        raise RomTransferError(None, "too many retries")
                    continue
                if ack["status"] != ROM_STATUS_OK:
                    raise RomTransferError(
                        ack["status"], ROM_STATUS_NAMES.get(ack["status"], "?")
                    )
                index += 1
                report(index, force=index == len(chunks))
                if retries > ROM_MAX_TOTAL_RETRIES:
                    raise RomTransferError(None, "too many retries")

            end = build_rom_mark(session, ROM_OP_END)
            retries += self._rom_exchange(sock, host, session, ROM_OP_END, 0, end)
            result = {
                "ok": True,
                "bytes": len(data),
                "chunks": len(chunks),
                "retries": retries,
                "ms": int((time.monotonic() - started) * 1000),
            }
            if save_name:
                result["sd"] = self._await_save_event(sock, session)
                result["name"] = save_name
                # The transfer itself succeeded either way — the image reached the
                # device. Only the card write can still be pending or refused, so
                # the overall verdict follows it rather than the ACKs above.
                result["ok"] = result["sd"]["status"] == SD_STATUS_OK
            return result
        finally:
            sock.close()

    def _await_save_event(self, sock, session: int) -> dict:
        """Wait for the datagram reporting the SD write, on the transfer's socket.

        The same socket the ACKs came back on, because the device replies to the
        port the BEGIN came from — opening a second one here would be listening in
        the wrong place. Silence is reported as a status rather than raised: the
        image did land, and "saved, result unknown" is a different thing to tell
        the user than "the transfer failed".
        """
        deadline = time.monotonic() + SD_SAVE_TIMEOUT_S
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return {"status": None, "error": "no save result from the device"}
            sock.settimeout(remaining)
            try:
                reply, _ = sock.recvfrom(64)
            except socket.timeout:
                return {"status": None, "error": "no save result from the device"}
            event = parse_rom_save_event(reply, session)
            if event is None:
                continue  # a late ACK, or somebody else's datagram
            status = event["status"]
            return {
                "status": status,
                "name": SD_STATUS_NAMES.get(status, "?"),
                "message": SD_STATUS_TEXT.get(status, "unknown SD status"),
            }

    def sd_command(self, host: str, op: int, *names: str) -> dict:
        """Run one type 5 command and return the device's answer.

        LIST reassembles its parts and returns the listing; the other ops return
        `{"status": 0}` on success. A non-OK status raises SdCommandError, so a
        caller only has to handle the happy path plus one exception.

        A dedicated socket, as send_rom and fetch_debug use, so a reply cannot be
        confused with anything else this process has in flight.
        """
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFFFF
        packet = build_sd_request(seq, op, *names)

        # DELETE and RENAME change the card, and the firmware keeps no per-seq
        # result cache, so a retransmission after a lost ACK re-runs the op: the
        # second DELETE of a file the first one removed answers NOT_FOUND, and
        # the second RENAME answers EXISTS. Retrying would turn a success into a
        # reported failure. LIST and LOAD are idempotent and keep their retries.
        idempotent = op in (SD_OP_LIST, SD_OP_LOAD)
        attempts = SD_RETRIES if idempotent else 1

        # BUSY means core 1 is occupied, not that this request was rejected —
        # the firmware answers it even to a retransmission of the request it is
        # currently serving. For an idempotent op the right move is therefore to
        # keep asking until it frees up, bounded by an overall deadline, rather
        # than surfacing a 409 for an operation that is about to complete. The
        # busy wait does not consume `attempts`: those exist for lost datagrams,
        # and a BUSY is proof the datagram arrived.
        busy_deadline = time.monotonic() + SD_BUSY_DEADLINE_S

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sent = 0
            while sent < attempts:
                sock.sendto(packet, (host, self.device_port))
                sent += 1
                if op == SD_OP_LIST:
                    listing = self._sd_collect_list(sock, seq)
                else:
                    listing = self._sd_await_ack(sock, seq, op)
                busy = listing is _SD_BUSY
                if busy:
                    # DELETE/RENAME never reach here: _sd_await_ack only returns
                    # the sentinel for an idempotent op, so their single send and
                    # immediate BUSY failure are untouched.
                    if time.monotonic() >= busy_deadline:
                        raise SdCommandError(
                            SD_STATUS_BUSY, SD_STATUS_TEXT[SD_STATUS_BUSY]
                        )
                    time.sleep(SD_BUSY_RETRY_S)
                    sent -= 1  # a BUSY is an answer, so it costs no attempt
                    continue
                if listing is not None:
                    return listing
            if idempotent:
                raise SdCommandError(None, "device did not answer")
            # Sent once and unanswered: the op may well have run. Saying so is
            # the honest answer, and it is what lets the page tell the user to
            # look at the listing instead of asserting a failure.
            raise SdCommandError(
                None, "no result from the device; the operation may have run"
            )
        finally:
            sock.close()

    def _sd_await_ack(self, sock, seq: int, op: int):
        """Read one command's ACK within the timeout.

        Returns None to retransmit (nothing arrived), `_SD_BUSY` when an
        idempotent op should wait and ask again, or the result dict.
        """
        deadline = time.monotonic() + SD_TIMEOUT_S
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            sock.settimeout(remaining)
            try:
                reply, _ = sock.recvfrom(2048)
            except socket.timeout:
                return None
            ack = parse_sd_ack(reply, seq, op)
            if ack is None:
                continue  # stale or foreign datagram
            # Handed back rather than raised, but only for LOAD: for DELETE and
            # RENAME a BUSY is final by design, since re-sending them is what the
            # single-attempt rule exists to prevent.
            busy_and_retriable = ack["status"] == SD_STATUS_BUSY and op == SD_OP_LOAD
            if busy_and_retriable:
                return _SD_BUSY
            if ack["status"] != SD_STATUS_OK:
                raise SdCommandError(
                    ack["status"], SD_STATUS_TEXT.get(ack["status"], "?")
                )
            return {"status": SD_STATUS_OK}

    def _sd_collect_list(self, sock, seq: int):
        """Reassemble a LIST reply, or None when a part is missing at the deadline.

        Returning None rather than a partial listing is what makes the retry in
        sd_command correct: a lost datagram means the caller sees the whole listing
        one round trip later instead of a card that appears to have fewer ROMs on
        it than it does.

        The capacity fields ride on every part, so whichever one arrived first is
        as good a source for them as part 0.
        """
        parts: dict[int, dict] = {}
        nparts = None
        deadline = time.monotonic() + SD_TIMEOUT_S
        while True:
            if nparts is not None and len(parts) >= nparts:
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            sock.settimeout(remaining)
            try:
                reply, _ = sock.recvfrom(2048)
            except socket.timeout:
                return None
            ack = parse_sd_ack(reply, seq, SD_OP_LIST)
            if ack is None:
                continue
            # A LIST that failed before it had anything to list answers with the
            # bare 8-byte ACK, so the status is checked before the header is read.
            #
            # BUSY is the one status that is not a failure: it says core 1 has
            # not got to this request yet. Reported up so the caller can wait,
            # and returned immediately because no part of the listing can follow
            # a BUSY — the device answered the request as a whole.
            if ack["status"] == SD_STATUS_BUSY:
                return _SD_BUSY
            if ack["status"] != SD_STATUS_OK:
                raise SdCommandError(
                    ack["status"], SD_STATUS_TEXT.get(ack["status"], "?")
                )
            parsed = parse_sd_list_part(reply)
            if parsed is None:
                continue
            nparts = parsed["nparts"]
            parts[parsed["part"]] = parsed

        ordered = [parts[i] for i in sorted(parts)]
        files = [f for p in ordered for f in p["files"]]
        return {
            "status": SD_STATUS_OK,
            "files": files,
            "total": ordered[0]["total"],
            "totalBytes": ordered[0]["total_bytes"],
            "freeBytes": ordered[0]["free_bytes"],
        }

    def _rom_exchange(self, sock, host, session, op, index, packet) -> int:
        """Send one frame, wait for its ACK, and insist the status is OK.

        Returns the number of retries it took. Used for BEGIN and END, where a
        non-OK status is always terminal — DATA is the only op with a status
        (SEQ) worth recovering from.
        """
        ack = self._rom_send_until_ack(sock, host, session, op, index, packet)
        if ack["status"] != ROM_STATUS_OK:
            raise RomTransferError(
                ack["status"], ROM_STATUS_NAMES.get(ack["status"], "?")
            )
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
                    continue  # stale or foreign datagram
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

        save_name, load, error = self._save_options(query)
        if error is not None:
            self._reply(400, {"error": error})
            return

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

        self._deliver_rom(query, host, data, swap, save_name, load)

    def _save_options(self, query):
        """Read the save/load query parameters shared by /api/rom and /api/rom/url.

        Returns (save_name, load, error). `load` defaults to true so the existing
        callers, which pass neither parameter, keep their "push and run" meaning.
        """
        raw = (query.get("save") or [""])[0]
        save_name = raw.strip() or None
        if save_name is not None and len(save_name.encode()) > SD_NAME_MAX:
            return None, True, "save name too long"

        load_raw = (query.get("load") or ["1"])[0]
        if load_raw not in ("0", "1"):
            return None, True, "bad load"
        load = load_raw == "1"

        # The device would answer this with a bare ACK and no visible effect: no
        # save target and no install is a transfer that discards its own payload.
        if save_name is None and not load:
            return None, True, "load=0 requires save"
        return save_name, load, None

    def _deliver_rom(self, query, host, data, swap, save_name, load):
        """Hand an image to the relay in whichever answer shape the caller asked for."""
        wants_progress = (query.get("progress") or ["0"])[0] == "1"
        if wants_progress:
            self._stream_rom(host, data, swap, save_name, load)
            return

        try:
            result = self.relay.send_rom(
                host, data, swap, save_name=save_name, load=load
            )
        except RomTransferError as exc:
            code, payload = self._rom_failure(exc)
            self._reply(code, payload)
            return
        except OSError as exc:
            self._reply(502, {"error": f"send failed: {exc}"})
            return

        # The image reached the device even when the card write was refused, so
        # the transfer keeps its 200 and the SD verdict travels in the body. The
        # page reads result.sd; a curl user sees the same fields.
        self._reply(200, result)

    def _post_rom_url(self):
        """Download a .nes here, then push it to the device as an ordinary transfer.

        The fetch lives on this side because the firmware has no TLS stack. Same
        query parameters as /api/rom so the two routes stay interchangeable to the
        page — only the source of the bytes differs.
        """
        query = parse_qs(urlparse(self.path).query)
        host = (query.get("host") or [""])[0]
        if not HOST_RE.match(host):
            self._reply(400, {"error": "bad host"})
            return
        swap = (query.get("swap") or ["0"])[0] == "1"

        save_name, load, error = self._save_options(query)
        if error is not None:
            self._reply(400, {"error": error})
            return

        url = (query.get("url") or [""])[0]
        try:
            data = fetch_rom_url(url)
        except RomDownloadError as exc:
            self._reply(exc.code, {"error": str(exc)})
            return

        self._deliver_rom(query, host, data, swap, save_name, load)

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

    def _stream_rom(
        self,
        host: str,
        data: bytes,
        swap: bool,
        save_name: str | None = None,
        load: bool = True,
    ):
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
            result = self.relay.send_rom(
                host,
                data,
                swap,
                on_progress=on_progress,
                save_name=save_name,
                load=load,
            )
        except RomTransferError as exc:
            code, payload = self._rom_failure(exc)
            emit({"error": payload["error"], "http": code})
            return
        except OSError as exc:
            emit({"error": f"send failed: {exc}", "http": 502})
            return

        emit(result)

    def _sd_failure(self, exc: SdCommandError):
        """Answer a refused or unanswered SD command, mirroring _rom_failure."""
        if exc.status is None:
            # 504 for the same reason the ROM path uses it: the request went out,
            # the device just never answered. `unknown` rides along so the page
            # can distinguish this from a refusal — for DELETE and RENAME the op
            # is sent once and never retried, so silence means the card state is
            # genuinely undetermined, not that nothing happened.
            self._reply(504, {"error": str(exc), "unknown": True})
            return
        self._reply(
            SD_STATUS_HTTP.get(exc.status, 502),
            {
                "error": SD_STATUS_TEXT.get(exc.status, "?"),
                "status": exc.status,
                "name": SD_STATUS_NAMES.get(exc.status, "?"),
            },
        )

    def _sd_names(self, payload: dict, *keys: str):
        """Pull and vet the name arguments an SD op needs.

        Returns (names, error). The length check is here as well as on the device
        so an over-long name is refused before a datagram goes out, and the caller
        gets a message about the name rather than a BAD_NAME code.
        """
        names = []
        for key in keys:
            value = payload.get(key)
            if not isinstance(value, str):
                return None, f"missing {key}"
            value = value.strip()
            if not value:
                return None, f"missing {key}"
            if len(value.encode()) > SD_NAME_MAX:
                return None, f"{key} too long"
            names.append(value)
        return names, None

    def _post_sd(self, route: str, payload: dict, host: str):
        """Run one SD command on behalf of the page.

        Every op funnels through relay.sd_command, so the only per-route work is
        naming the arguments each one takes.
        """
        if route == "/api/sd/list":
            names = []
            op = SD_OP_LIST
        elif route == "/api/sd/load":
            op = SD_OP_LOAD
            names, error = self._sd_names(payload, "name")
        elif route == "/api/sd/delete":
            op = SD_OP_DELETE
            names, error = self._sd_names(payload, "name")
        else:
            op = SD_OP_RENAME
            names, error = self._sd_names(payload, "name", "to")

        if op != SD_OP_LIST and names is None:
            self._reply(400, {"error": error})
            return

        try:
            result = self.relay.sd_command(host, op, *names)
        except SdCommandError as exc:
            self._sd_failure(exc)
            return
        except OSError as exc:
            self._reply(502, {"error": f"send failed: {exc}"})
            return

        self._reply(200, result)

    def do_POST(self):
        route = self.path.split("?")[0]
        if route == "/api/rom":
            self._post_rom()
            return
        if route == "/api/rom/url":
            self._post_rom_url()
            return

        if route not in (
            "/api/pins",
            "/api/reset",
            "/api/volume",
            "/api/debug",
            "/api/sd/list",
            "/api/sd/load",
            "/api/sd/delete",
            "/api/sd/rename",
        ):
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

        if route.startswith("/api/sd/"):
            self._post_sd(route, payload, host)
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
            valid = (
                isinstance(raw, int) and not isinstance(raw, bool) and 0 <= raw <= 255
            )
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
    parser.add_argument(
        "--dir", default=None, help="directory to serve (default: web/)"
    )
    args = parser.parse_args()

    root = (
        Path(args.dir) if args.dir else Path(__file__).resolve().parent.parent / "web"
    )
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

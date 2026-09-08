#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Check that the COBS framing and CRC-16 agree across the two implementations.

The serial transport has a copy of the framing on each side — C++ in
`m5stack/src/serial_link.cpp` and JavaScript in `web/protocol.js` — and neither
can be exercised without hardware. A disagreement between them is not a crash:
frames simply never decode, and the browser sees a device that answers nothing.
So both are re-implemented here and checked against each other on round-trips,
including the cases that actually differ between COBS variants (a payload of
zeros, a run of exactly 254 non-zero bytes, an empty frame).

The reference values are pinned rather than derived, so a change to either
implementation has to be a deliberate change to this file as well.

    uv run tools/test_serial_frame.py
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


# --------------------------------------------------------------- reference


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    code_idx = 0
    out.append(0)
    code = 1
    for byte in data:
        if byte == 0:
            out[code_idx] = code
            code_idx = len(out)
            out.append(0)
            code = 1
            continue
        out.append(byte)
        code += 1
        if code == 0xFF:
            out[code_idx] = code
            code_idx = len(out)
            out.append(0)
            code = 1
    out[code_idx] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes | None:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        if code == 0:
            return None
        i += 1
        for _ in range(code - 1):
            if i >= len(data):
                return None
            out.append(data[i])
            i += 1
        if code < 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (
                ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
            )
    return crc


# ------------------------------------------------------------------- cases

# Chosen for what they exercise, not for coverage: a lone zero and a run of
# zeros are the whole point of COBS, and 254 non-zero bytes is the block
# boundary where the encoder emits a continuation code rather than a length.
CASES = [
    b"",
    b"\x00",
    b"\x00\x00\x00",
    b"NP\x01\x04\x00\x00\x00\x00",
    bytes(range(1, 255)),
    b"\x01" * 254,
    b"\x01" * 255,
    b"\xff" * 300 + b"\x00" + b"\xaa" * 300,
    bytes(1412),
]

# Pinned CRC-16/CCITT-FALSE values. The first is the standard check vector,
# which is what proves this is the same polynomial the firmware uses.
PINNED_CRC = {
    b"123456789": 0x29B1,
    b"": 0xFFFF,
    b"\x00": 0xE1F0,
}


def check_roundtrip() -> list[str]:
    failures = []
    for payload in CASES:
        encoded = cobs_encode(payload)
        if 0 in encoded:
            failures.append(f"encoded body contains a delimiter: {payload[:16]!r}")
        decoded = cobs_decode(encoded)
        if decoded != payload:
            failures.append(
                f"round-trip changed the payload: {payload[:16]!r} -> {decoded!r}"
            )
    return failures


def check_pinned_crc() -> list[str]:
    failures = []
    for data, want in PINNED_CRC.items():
        got = crc16(data)
        if got != want:
            failures.append(f"crc16({data!r}) = {got:#06x}, expected {want:#06x}")
    return failures


def check_js_matches() -> list[str]:
    """Run web/protocol.js's COBS and CRC under node and compare.

    Skipped rather than failed when node is absent: the nix devshell does not
    supply it, and the value here is in catching a divergence when it can be
    checked, not in requiring another toolchain.
    """
    source = (ROOT / "web" / "protocol.js").read_text()
    script = """
%s
const P = globalThis.window.NesProto;
const cases = %s;
const out = [];
for (const hex of cases) {
  const pairs = hex.length ? hex.match(/../g) : [];
  const bytes = Uint8Array.from(pairs.map(h => parseInt(h, 16)));
  const enc = P.cobsEncode(bytes);
  const dec = P.cobsDecode(enc);
  out.push([
    Array.from(enc).map(b => b.toString(16).padStart(2, '0')).join(''),
    Array.from(dec).map(b => b.toString(16).padStart(2, '0')).join(''),
    P.crc16(bytes, bytes.length).toString(16),
  ].join(' '));
}
console.log(out.join('\\n'));
""" % (
        "globalThis.window = globalThis;\n" + source,
        "[" + ",".join('"' + c.hex() + '"' for c in CASES) + "]",
    )

    try:
        result = subprocess.run(
            ["node", "-e", script], capture_output=True, text=True, timeout=30
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        print("note: node not available, skipping the JavaScript comparison")
        return []

    if result.returncode != 0:
        return [
            f"running web/protocol.js under node failed: {result.stderr.strip()[:400]}"
        ]

    failures = []
    lines = result.stdout.strip().split("\n")
    if len(lines) != len(CASES):
        return [f"expected {len(CASES)} results from node, got {len(lines)}"]
    for payload, line in zip(CASES, lines):
        js_enc, js_dec, js_crc = line.split(" ")
        if js_enc != cobs_encode(payload).hex():
            failures.append(f"JS encode differs for {payload[:16]!r}")
        if js_dec != payload.hex():
            failures.append(f"JS round-trip differs for {payload[:16]!r}")
        if int(js_crc, 16) != crc16(payload):
            failures.append(f"JS crc16 differs for {payload[:16]!r}")
    return failures


# Wire vectors pinned in cli/tests/wire_compat.rs. The browser has to agree with
# the Rust client byte for byte, because both talk to the same firmware — and
# the CRC-32 in a BEGIN is what the device checks the staged image against, so a
# divergence here fails every transfer with a CRC status and no clue why.
WIRE_VECTORS = [
    # (session, flags, name, expected hex) over the payload b"rom"
    (0x1234, 0x00, None, "4e5001043412000003000000a10f5279"),
    (1, 0x02, "game.nes", "4e5001040100000203000000a10f52790867616d652e6e6573"),
    (1, 0x01, None, "4e5001040100000103000000a10f5279"),
]

# The one-way packets the emulator page sends. Same source as above
# (cli/tests/wire_compat.rs). The pin mask is the one that matters most: it is
# 60 bits little-endian, which JavaScript can only build with BigInt — the
# bitwise operators would silently truncate it to 32.
SIMPLE_VECTORS = [
    ("pins-all-ok", "buildPins((1n << 60n) - 1n)", "4e5001010000ffffffffffffff0f"),
    ("ctrl-reset", "buildCtrl(P.CTRL_RESET, 0)", "4e50010200000100"),
    ("ctrl-volume", "buildCtrl(P.CTRL_VOLUME, 192)", "4e500102000002c0"),
    ("debug-plain", "buildDebug(0x1234, false)", "4e50010334120000"),
    ("debug-waves", "buildDebug(1, true)", "4e50010301000100"),
]


def check_js_wire_vectors() -> list[str]:
    """The browser's ROM BEGIN must match the Rust client's pinned bytes."""
    source = (ROOT / "web" / "protocol.js").read_text()
    cases = ",".join(
        "[%d,%d,%s]" % (session, flags, "null" if name is None else '"%s"' % name)
        for session, flags, name, _ in WIRE_VECTORS
    )
    script = """
globalThis.window = globalThis;
%s
const P = globalThis.window.NesProto;
const rom = new TextEncoder().encode('rom');
const crc = P.crc32(rom);
const hex = (b) => Array.from(b).map(x => x.toString(16).padStart(2, '0')).join('');
const out = [];
for (const [session, flags, name] of [%s]) {
  out.push(hex(P.buildRomBegin(session, flags, rom.length, crc, name)));
}
%s
console.log(out.join('\\n'));
""" % (
        source,
        cases,
        "\n".join("out.push(hex(P.%s));" % expr for _, expr, _ in SIMPLE_VECTORS),
    )

    try:
        result = subprocess.run(
            ["node", "-e", script], capture_output=True, text=True, timeout=30
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []
    if result.returncode != 0:
        return [f"wire vector check failed to run: {result.stderr.strip()[:400]}"]

    failures = []
    lines = result.stdout.strip().split("\n")
    for (session, _flags, _name, want), got in zip(WIRE_VECTORS, lines):
        if got != want:
            failures.append(
                f"ROM BEGIN for session {session:#x}: JS made {got}, "
                f"cli/tests/wire_compat.rs pins {want}"
            )
    for (name, _expr, want), got in zip(SIMPLE_VECTORS, lines[len(WIRE_VECTORS) :]):
        if got != want:
            failures.append(
                f"{name}: JS made {got}, cli/tests/wire_compat.rs pins {want}"
            )
    return failures


def check_cpp_constants() -> list[str]:
    """The C++ side must reserve the same two bytes for the CRC."""
    config = (ROOT / "m5stack" / "src" / "config.h").read_text()
    match = re.search(r"constexpr uint8_t SERIAL_CRC_SIZE = (\d+);", config)
    if not match:
        return ["SERIAL_CRC_SIZE not found in config.h"]
    if int(match.group(1)) != 2:
        return [f"SERIAL_CRC_SIZE is {match.group(1)}, but the framing assumes 2"]
    return []


def main() -> int:
    failures = []
    failures += check_pinned_crc()
    failures += check_roundtrip()
    failures += check_cpp_constants()
    failures += check_js_matches()
    failures += check_js_wire_vectors()

    if failures:
        print("serial framing check FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(
        f"serial framing OK ({len(CASES)} payloads, {len(PINNED_CRC)} pinned CRCs, "
        f"{len(WIRE_VECTORS) + len(SIMPLE_VECTORS)} wire vectors)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

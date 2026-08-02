#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""A ~200-line hand-assembler for the 6502 subset the test ROMs need.

Not a general assembler: it emits opcode bytes directly from named methods
(`a.lda_imm(0x3F)` -> `A9 3F`), so there is no parser, no expression grammar and
no external toolchain to install. Branch and jump targets are the one thing that
cannot be resolved while emitting, so those go through a fixup list that is
patched in a second pass by `assemble()`.

Addresses are absolute from the start: `Asm(0xC000)` means the first byte emitted
lives at $C000, which is where NROM maps the 16KB PRG bank. Labels therefore hold
real CPU addresses and can be used as JMP operands verbatim.
"""

from __future__ import annotations

import struct


class Asm:
    """Two-pass byte emitter. Pass 1 emits and records fixups, pass 2 patches."""

    def __init__(self, base: int = 0xC000) -> None:
        self.base = base
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        # (offset_in_code, label, kind) where kind is "rel" (1 byte, branch) or
        # "abs" (2 bytes, little-endian)
        self.fixups: list[tuple[int, str, str]] = []

    # ------------------------------------------------------------- plumbing
    @property
    def pc(self) -> int:
        return self.base + len(self.code)

    def label(self, name: str) -> None:
        if name in self.labels:
            raise ValueError(f"duplicate label: {name}")
        self.labels[name] = self.pc

    def _emit(self, *bs: int) -> None:
        for b in bs:
            if not 0 <= b <= 0xFF:
                raise ValueError(f"byte out of range: {b:#x}")
            self.code.append(b)

    def _op_imm(self, opcode: int, v: int) -> None:
        self._emit(opcode, v & 0xFF)

    def _op_zp(self, opcode: int, addr: int) -> None:
        if not 0 <= addr <= 0xFF:
            raise ValueError(f"not a zero-page address: {addr:#x}")
        self._emit(opcode, addr)

    def _op_abs(self, opcode: int, addr: int | str) -> None:
        """Absolute operand. A str is a forward-referenceable label, which is what
        lets data tables be declared after the code that indexes them."""
        if isinstance(addr, str):
            self._op_target(opcode, addr, "abs")
        else:
            self._emit(opcode, addr & 0xFF, (addr >> 8) & 0xFF)

    def _op_target(self, opcode: int, target: int | str, kind: str) -> None:
        """Emit an opcode whose operand is a label or a literal address."""
        self._emit(opcode)
        if isinstance(target, str):
            self.fixups.append((len(self.code), target, kind))
            if kind == "rel":
                self._emit(0x00)
            else:
                self._emit(0x00, 0x00)
        elif kind == "rel":
            raise ValueError("branch to a literal address is not supported")
        else:
            self._emit(target & 0xFF, (target >> 8) & 0xFF)

    # -------------------------------------------------------------- opcodes
    # load / store
    def lda_imm(self, v: int) -> None: self._op_imm(0xA9, v)
    def lda_zp(self, a: int) -> None: self._op_zp(0xA5, a)
    def lda_zpx(self, a: int) -> None: self._op_zp(0xB5, a)
    def lda_abs(self, a: int | str) -> None: self._op_abs(0xAD, a)
    def lda_absx(self, a: int | str) -> None: self._op_abs(0xBD, a)
    def ldx_imm(self, v: int) -> None: self._op_imm(0xA2, v)
    def ldx_zp(self, a: int) -> None: self._op_zp(0xA6, a)
    def ldy_imm(self, v: int) -> None: self._op_imm(0xA0, v)
    def ldy_zp(self, a: int) -> None: self._op_zp(0xA4, a)
    def sta_zp(self, a: int) -> None: self._op_zp(0x85, a)
    def sta_zpx(self, a: int) -> None: self._op_zp(0x95, a)
    def sta_abs(self, a: int | str) -> None: self._op_abs(0x8D, a)
    def sta_absx(self, a: int | str) -> None: self._op_abs(0x9D, a)
    def stx_zp(self, a: int) -> None: self._op_zp(0x86, a)
    def stx_abs(self, a: int | str) -> None: self._op_abs(0x8E, a)
    def sty_zp(self, a: int) -> None: self._op_zp(0x84, a)
    def sty_abs(self, a: int | str) -> None: self._op_abs(0x8C, a)

    # transfers / stack
    def tax(self) -> None: self._emit(0xAA)
    def tay(self) -> None: self._emit(0xA8)
    def txa(self) -> None: self._emit(0x8A)
    def tya(self) -> None: self._emit(0x98)
    def txs(self) -> None: self._emit(0x9A)
    def pha(self) -> None: self._emit(0x48)
    def pla(self) -> None: self._emit(0x68)

    # arithmetic / logic
    def and_imm(self, v: int) -> None: self._op_imm(0x29, v)
    def and_zp(self, a: int) -> None: self._op_zp(0x25, a)
    def ora_imm(self, v: int) -> None: self._op_imm(0x09, v)
    def ora_zp(self, a: int) -> None: self._op_zp(0x05, a)
    def eor_imm(self, v: int) -> None: self._op_imm(0x49, v)
    def adc_imm(self, v: int) -> None: self._op_imm(0x69, v)
    def adc_zp(self, a: int) -> None: self._op_zp(0x65, a)
    def sbc_imm(self, v: int) -> None: self._op_imm(0xE9, v)
    def cmp_imm(self, v: int) -> None: self._op_imm(0xC9, v)
    def cmp_zp(self, a: int) -> None: self._op_zp(0xC5, a)
    def cpx_imm(self, v: int) -> None: self._op_imm(0xE0, v)
    def cpy_imm(self, v: int) -> None: self._op_imm(0xC0, v)
    def inc_zp(self, a: int) -> None: self._op_zp(0xE6, a)
    def dec_zp(self, a: int) -> None: self._op_zp(0xC6, a)
    def inx(self) -> None: self._emit(0xE8)
    def iny(self) -> None: self._emit(0xC8)
    def dex(self) -> None: self._emit(0xCA)
    def dey(self) -> None: self._emit(0x88)
    def asl_a(self) -> None: self._emit(0x0A)
    def lsr_a(self) -> None: self._emit(0x4A)
    def rol_a(self) -> None: self._emit(0x2A)
    def rol_zp(self, a: int) -> None: self._op_zp(0x26, a)
    def ror_a(self) -> None: self._emit(0x6A)

    # flags
    def sei(self) -> None: self._emit(0x78)
    def cli(self) -> None: self._emit(0x58)
    def cld(self) -> None: self._emit(0xD8)
    def clc(self) -> None: self._emit(0x18)
    def sec(self) -> None: self._emit(0x38)

    # control flow
    def jmp(self, target: int | str) -> None: self._op_target(0x4C, target, "abs")
    def jsr(self, target: int | str) -> None: self._op_target(0x20, target, "abs")
    def rts(self) -> None: self._emit(0x60)
    def rti(self) -> None: self._emit(0x40)
    def nop(self) -> None: self._emit(0xEA)
    def bpl(self, t: str) -> None: self._op_target(0x10, t, "rel")
    def bmi(self, t: str) -> None: self._op_target(0x30, t, "rel")
    def bne(self, t: str) -> None: self._op_target(0xD0, t, "rel")
    def beq(self, t: str) -> None: self._op_target(0xF0, t, "rel")
    def bcc(self, t: str) -> None: self._op_target(0x90, t, "rel")
    def bcs(self, t: str) -> None: self._op_target(0xB0, t, "rel")

    # raw data
    def db(self, *bs: int) -> None: self._emit(*bs)

    # ------------------------------------------------------------- assembly
    def resolve(self) -> bytes:
        """Pass 2: patch every recorded fixup with its label's address."""
        out = bytearray(self.code)
        for off, name, kind in self.fixups:
            if name not in self.labels:
                raise ValueError(f"undefined label: {name}")
            target = self.labels[name]
            if kind == "abs":
                out[off] = target & 0xFF
                out[off + 1] = (target >> 8) & 0xFF
            else:
                # A branch is relative to the address *after* the operand byte.
                delta = target - (self.base + off + 1)
                if not -128 <= delta <= 127:
                    raise ValueError(f"branch to {name} out of range ({delta})")
                out[off] = delta & 0xFF
        return bytes(out)

    def assemble(
        self,
        size: int,
        base: int,
        reset: str,
        nmi: str | None = None,
        irq: str | None = None,
        fill: int = 0xFF,
    ) -> bytes:
        """Resolve, pad to `size`, and stamp the three vectors at the tail.

        `base` is the CPU address the bank maps to, so the vectors carry real
        addresses regardless of where in the file the bank sits.
        """
        body = self.resolve()
        if len(body) > size - 6:
            raise ValueError(f"code is {len(body)} bytes, does not fit in {size - 6}")
        bank = bytearray([fill]) * size
        bank[: len(body)] = body

        def addr_of(name: str | None) -> int:
            if name is None:
                return base  # a vector must never be garbage; aim it at the start
            return self.labels[name]

        struct.pack_into(
            "<HHH", bank, size - 6, addr_of(nmi), addr_of(reset), addr_of(irq)
        )
        return bytes(bank)


def ines_header(prg_16k: int, chr_8k: int, mapper: int = 0, vertical: bool = False) -> bytes:
    """16-byte iNES header. Bytes 8-15 stay zero, which is what keeps a header
    valid for both iNES and the NES 2.0 sniff (byte 7 bits 2-3 = 00)."""
    flags6 = (mapper & 0x0F) << 4 | (1 if vertical else 0)
    flags7 = mapper & 0xF0
    return bytes([0x4E, 0x45, 0x53, 0x1A, prg_16k, chr_8k, flags6, flags7]) + bytes(8)

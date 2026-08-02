#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Build exptest.nes: a background-colour flasher that makes expansion-port key
noise visible on screen.

The emulator injects the DA-15 key rattle by ORing bits into the $4016/$4017 pad
reads ($4016 D1 = pin 13, $4017 D0-D4 = pins 8,7,6,5,4). Nothing on screen shows
that unless a game happens to look, so this ROM does the looking: every frame it
runs a standard eight-read pad poll, accumulates the noise-carrying bits, and
paints the result into the backdrop colour.

    black ($0F)   nothing
    white ($30)   $4016 D1 saw a 1
    red   ($16)   $4017 D0-D4 saw a 1
    green ($2A)   both

Hand-assembled: the byte stream is emitted by a tiny two-pass assembler built out
of Python lambdas below, so the script has no dependency on ca65/asm6 and runs
anywhere python3 does.

Usage:  python3 tools/make_exptest_rom.py [-o web/exptest.nes]
"""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from nesasm6502 import Asm, ines_header  # noqa: E402

PRG_BASE = 0xC000  # 16KB PRG mirrored into $8000 and $C000 by NROM

# Backdrop colours, indexed by (noiseB << 1) | noiseA.
COL_NONE = 0x0F
COL_A = 0x30
COL_B = 0x16
COL_BOTH = 0x2A

# Zero page
Z_A = 0x00  # accumulated $4016 D1
Z_B = 0x01  # accumulated $4017 D0-D4
Z_COL = 0x02  # backdrop colour for this frame


def build_prg() -> bytes:
    a = Asm(PRG_BASE)

    # ---------------------------------------------------------------- reset
    a.label("reset")
    a.sei()
    a.cld()
    a.ldx_imm(0xFF)
    a.txs()
    a.inx()  # X = 0
    a.stx_abs(0x2000)  # NMI off. The vector is valid anyway, but a ROM that
    a.stx_abs(0x2001)  # never asks for NMIs is one less thing to get wrong.
    a.stx_abs(0x4015)  # APU silent
    a.lda_imm(0x40)
    a.sta_abs(0x4017)  # inhibit the frame IRQ; pin-3 noise can still fire one,
    # which is why the IRQ vector points at an RTI.

    # Two vblanks before touching the PPU, per the standard warm-up.
    a.label("warm1")
    a.lda_abs(0x2002)
    a.bpl("warm1")
    a.label("warm2")
    a.lda_abs(0x2002)
    a.bpl("warm2")

    # Paint the whole palette black so nothing but $3F00 can ever be visible.
    a.lda_abs(0x2002)  # reset the $2006 latch
    a.lda_imm(0x3F)
    a.sta_abs(0x2006)
    a.lda_imm(0x00)
    a.sta_abs(0x2006)
    a.ldx_imm(0x20)
    a.lda_imm(COL_NONE)
    a.label("palfill")
    a.sta_abs(0x2007)
    a.dex()
    a.bne("palfill")

    # ----------------------------------------------------------- main loop
    a.label("main")

    # 1. wait for vblank
    a.label("vblank")
    a.lda_abs(0x2002)
    a.bpl("vblank")

    # 2. strobe 1 -> 0
    a.lda_imm(0x01)
    a.sta_abs(0x4016)
    a.lda_imm(0x00)
    a.sta_abs(0x4016)

    # 3. eight reads of $4016, OR-accumulating D1 (the only expansion bit there)
    a.sta_zp(Z_A)  # A is 0 here
    a.sta_zp(Z_B)
    a.ldx_imm(0x08)
    a.label("poll0")
    a.lda_abs(0x4016)
    a.and_imm(0x02)
    a.ora_zp(Z_A)
    a.sta_zp(Z_A)
    a.dex()
    a.bne("poll0")

    # 4. eight reads of $4017, OR-accumulating D0-D4.
    #
    # $4017 is re-strobed rather than sharing the $4016 strobe: writing $4016
    # latches both ports, but reading $4016 eight times has already shifted
    # port 0 only, so port 1 is still lined up. Reading it straight after is
    # exactly what a two-player poll does.
    a.ldx_imm(0x08)
    a.label("poll1")
    a.lda_abs(0x4017)
    a.and_imm(0x1F)
    a.ora_zp(Z_B)
    a.sta_zp(Z_B)
    a.dex()
    a.bne("poll1")

    # 5. pick the colour
    a.lda_imm(COL_NONE)
    a.sta_zp(Z_COL)
    a.lda_zp(Z_A)
    a.beq("chk_b")
    a.lda_imm(COL_A)
    a.sta_zp(Z_COL)
    a.label("chk_b")
    a.lda_zp(Z_B)
    a.beq("paint")
    a.lda_zp(Z_A)
    a.beq("only_b")
    a.lda_imm(COL_BOTH)
    a.sta_zp(Z_COL)
    a.jmp("paint")
    a.label("only_b")
    a.lda_imm(COL_B)
    a.sta_zp(Z_COL)

    # 6. write $3F00 and restore the address latch.
    #
    # Why the trailing $2006 pair matters even with rendering off: with
    # rendering disabled the PPU paints the backdrop from `v & 0x1F` whenever v
    # points into $3F00-$3FFF, so leaving v parked at $3F01 after the write
    # would show palette entry 1 instead of the backdrop. Pointing v at $0000
    # puts it back on entry 0. The $2005 writes keep the scroll registers sane
    # for anyone who turns rendering on.
    a.label("paint")
    a.lda_abs(0x2002)
    a.lda_imm(0x3F)
    a.sta_abs(0x2006)
    a.lda_imm(0x00)
    a.sta_abs(0x2006)
    a.lda_zp(Z_COL)
    a.sta_abs(0x2007)
    a.lda_imm(0x00)
    a.sta_abs(0x2005)
    a.sta_abs(0x2005)
    a.sta_abs(0x2006)
    a.sta_abs(0x2006)

    a.jmp("main")

    # --------------------------------------------------------------- vectors
    # Both NMI and IRQ land on a plain RTI. The IRQ one is load-bearing: the key
    # rattle can short pin 3 and pull /IRQ low at random, and without a valid
    # handler the CPU would vector through $FFFF garbage and wander off.
    a.label("irq")
    a.rti()

    return a.assemble(size=0x4000, base=PRG_BASE, nmi="irq", reset="reset", irq="irq")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("-o", "--out", default="web/exptest.nes", help="output .nes path")
    args = p.parse_args()

    prg = build_prg()
    chr_rom = bytes(0x2000)  # rendering stays off; the pattern tables go unread
    rom = ines_header(prg_16k=1, chr_8k=1, mapper=0) + prg + chr_rom

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(rom)
    print(f"wrote {out} ({len(rom)} bytes: 16 header + {len(prg)} PRG + {len(chr_rom)} CHR)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

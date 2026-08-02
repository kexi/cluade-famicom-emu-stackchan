#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Build exppitch.nes: a one-pitch baseball demo that turns expansion-port key
noise into a breaking ball.

This is the 『ベースボール』 trick reproduced in a cartridge small enough to read.
The ball falls from the top of the screen at a constant rate; its horizontal drift
comes entirely from the Left/Right bits of a *standard Nintendo pad poll*. The poll
is the load-bearing part: after strobing $4016, each of the eight reads is masked
with `AND #$03` and the two bits are folded to one with an OR before being shifted
into the result byte. That fold is precisely why the key rattle reaches the game —
the emulator ORs the shorted expansion lines into $4016 D1 and $4017 D0-D4, and a
game that reads D0|D1 cannot tell a key from a D-pad.

    hands off the controller -> the ball drops straight (a fastball)
    key rattling in the port  -> Left/Right thrash and the ball weaves (a curve)

The backdrop keeps the exptest.nes colour code running at the same time, so the
noise is visible as a flashing border colour as well as in the ball's path:

    black ($0F) none / white ($30) $4016 D1 / red ($16) $4017 D0-D4 / green ($2A) both

Hand-assembled through the same dependency-free emitter as make_exptest_rom.py.

Usage:  python3 tools/make_exppitch_rom.py [-o web/exppitch.nes]
"""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from nesasm6502 import Asm, ines_header  # noqa: E402

PRG_BASE = 0xC000

COL_NONE, COL_A, COL_B, COL_BOTH = 0x0F, 0x30, 0x16, 0x2A

# Zero page
Z_A = 0x00  # $4016 D1 noise accumulator
Z_B = 0x01  # $4017 D0-D4 noise accumulator
Z_COL = 0x02  # backdrop colour this frame
Z_PAD0 = 0x03  # port 0 poll result (standard NES button order)
Z_PAD1 = 0x04  # port 1 poll result
Z_BTN = 0x05  # pad0 | pad1
Z_BALLX = 0x06
Z_BALLY = 0x07
Z_TMP = 0x08

OAM = 0x0200  # OAM staging page, DMA'd through $4014

# Standard pad bit order as produced by an eight-read shift-left poll:
# A,B,Select,Start,Up,Down,Left,Right -> bit7..bit0
BIT_LEFT = 0x02
BIT_RIGHT = 0x01

BALL_TILE = 0x01
BALL_START_X = 124
BALL_START_Y = 24
BALL_END_Y = 200
BALL_DY = 2


def build_chr() -> bytes:
    """8KB CHR: tile 1 is the ball, tile 2 a solid block for the frame, the rest
    blank. Two planes per tile, 16 bytes each, low plane then high plane."""
    chr_rom = bytearray(0x2000)

    def put(index: int, rows: list[int], plane1: list[int] | None = None) -> None:
        for pattern_table in (0x0000, 0x1000):
            base = pattern_table + index * 16
            for y in range(8):
                chr_rom[base + y] = rows[y]
                chr_rom[base + 8 + y] = (plane1[y] if plane1 else 0)

    # tile 1: a round ball (colour 1)
    ball = [
        0b00111100,
        0b01111110,
        0b11111111,
        0b11111111,
        0b11111111,
        0b11111111,
        0b01111110,
        0b00111100,
    ]
    put(BALL_TILE, ball)

    # tile 2: solid, used to draw the strike-zone frame and the ground line
    put(0x02, [0xFF] * 8)

    return bytes(chr_rom)


def build_prg() -> bytes:
    a = Asm(PRG_BASE)

    a.label("reset")
    a.sei()
    a.cld()
    a.ldx_imm(0xFF)
    a.txs()
    a.inx()
    a.stx_abs(0x2000)
    a.stx_abs(0x2001)
    a.stx_abs(0x4015)
    a.lda_imm(0x40)
    a.sta_abs(0x4017)  # frame IRQ off; pin-3 noise can still fire one -> RTI

    a.label("warm1")
    a.lda_abs(0x2002)
    a.bpl("warm1")
    a.label("warm2")
    a.lda_abs(0x2002)
    a.bpl("warm2")

    # ---- palette: backdrop + white for both the ball and the frame ----
    a.lda_abs(0x2002)
    a.lda_imm(0x3F)
    a.sta_abs(0x2006)
    a.lda_imm(0x00)
    a.sta_abs(0x2006)
    a.ldx_imm(0x00)
    a.label("palloop")
    a.lda_absx("paldata")
    a.sta_abs(0x2007)
    a.inx()
    a.cpx_imm(0x20)
    a.bne("palloop")

    # ---- nametable: clear to tile 0, then draw a strike-zone frame ----
    a.lda_abs(0x2002)
    a.lda_imm(0x20)
    a.sta_abs(0x2006)
    a.lda_imm(0x00)
    a.sta_abs(0x2006)
    # 960 tiles + 64 attribute bytes = 1024 writes, done as 4 x 256 so the inner
    # counter stays in one register.
    a.ldy_imm(0x04)
    a.lda_imm(0x00)
    a.label("ntclear")
    a.ldx_imm(0x00)
    a.label("ntclear_in")
    a.sta_abs(0x2007)
    a.inx()
    a.bne("ntclear_in")
    a.dey()
    a.bne("ntclear")

    # Strike-zone frame: a horizontal run of tile 2 at nametable rows 3 and 24,
    # columns 10..21. Rows are 32 tiles apart, so the addresses are $2000 + row*32
    # + col. Drawn as two straight runs rather than a general rect routine — the
    # ROM only ever needs these two.
    for row in (3, 24):
        addr = 0x2000 + row * 32 + 10
        a.lda_abs(0x2002)
        a.lda_imm((addr >> 8) & 0xFF)
        a.sta_abs(0x2006)
        a.lda_imm(addr & 0xFF)
        a.sta_abs(0x2006)
        a.ldx_imm(12)
        a.lda_imm(0x02)
        a.label(f"row{row}")
        a.sta_abs(0x2007)
        a.dex()
        a.bne(f"row{row}")

    # ---- OAM staging: one sprite, the rest parked off-screen ----
    # y = $FF parks a sprite below the visible area, so a blanket $FF fill hides
    # all 64 entries and only sprite 0 is ever revived below.
    a.ldx_imm(0x00)
    a.lda_imm(0xFF)
    a.label("oamclr")
    a.sta_absx(OAM)
    a.inx()
    a.bne("oamclr")

    a.lda_imm(BALL_START_X)
    a.sta_zp(Z_BALLX)
    a.lda_imm(BALL_START_Y)
    a.sta_zp(Z_BALLY)

    # ---- turn rendering on ----
    a.lda_imm(0x1E)  # show background + sprites, no left-edge clipping
    a.sta_abs(0x2001)

    # ================================================================ main
    a.label("main")
    a.label("vblank")
    a.lda_abs(0x2002)
    a.bpl("vblank")

    # ---- 1. OAM DMA ----
    a.lda_imm(0x00)
    a.sta_abs(0x2003)
    a.lda_imm(OAM >> 8)
    a.sta_abs(0x4014)

    # ---- 2. standard eight-read pad poll, D0|D1 folded ----
    a.lda_imm(0x01)
    a.sta_abs(0x4016)
    a.lda_imm(0x00)
    a.sta_abs(0x4016)
    a.sta_zp(Z_A)
    a.sta_zp(Z_B)

    # port 0. Each read: AND #$03 keeps D0 (pad) and D1 (expansion), the two are
    # folded to a single bit, and that bit is rolled into Z_PAD0 from the bottom.
    # Carry is the courier: LSR of the folded bit puts it in C, then ROL shifts it
    # into the accumulator.
    a.ldx_imm(0x08)
    a.label("poll0")
    a.lda_abs(0x4016)
    a.and_imm(0x03)
    # accumulate the raw D1 for the backdrop colour before folding it away
    a.tay()
    a.and_imm(0x02)
    a.ora_zp(Z_A)
    a.sta_zp(Z_A)
    a.tya()
    a.and_imm(0x03)
    # fold D0|D1 -> C. A nonzero A means "pressed"; CMP #$01 sets C for A>=1.
    a.cmp_imm(0x01)
    a.rol_zp(Z_PAD0)
    a.dex()
    a.bne("poll0")

    # port 1: same fold, but $4017 carries five expansion lines so the noise mask
    # for the colour code is $1F.
    a.ldx_imm(0x08)
    a.label("poll1")
    a.lda_abs(0x4017)
    a.tay()
    a.and_imm(0x1F)
    a.ora_zp(Z_B)
    a.sta_zp(Z_B)
    a.tya()
    a.and_imm(0x03)
    a.cmp_imm(0x01)
    a.rol_zp(Z_PAD1)
    a.dex()
    a.bne("poll1")

    a.lda_zp(Z_PAD0)
    a.ora_zp(Z_PAD1)
    a.sta_zp(Z_BTN)

    # ---- 3. move the ball ----
    a.lda_zp(Z_BALLY)
    a.clc()
    a.adc_imm(BALL_DY)
    a.sta_zp(Z_BALLY)

    # Left/Right steer x. Both at once cancels out, which is what a real D-pad
    # would do and keeps the ball from drifting when the key shorts everything.
    a.lda_zp(Z_BTN)
    a.and_imm(BIT_LEFT)
    a.beq("no_left")
    a.dec_zp(Z_BALLX)
    a.dec_zp(Z_BALLX)
    a.label("no_left")
    a.lda_zp(Z_BTN)
    a.and_imm(BIT_RIGHT)
    a.beq("no_right")
    a.inc_zp(Z_BALLX)
    a.inc_zp(Z_BALLX)
    a.label("no_right")

    # ---- 4. reached the bottom? next pitch ----
    a.lda_zp(Z_BALLY)
    a.cmp_imm(BALL_END_Y)
    a.bcc("no_reset")
    a.lda_imm(BALL_START_Y)
    a.sta_zp(Z_BALLY)
    a.lda_imm(BALL_START_X)
    a.sta_zp(Z_BALLX)
    a.label("no_reset")

    # write sprite 0 into the staging page for next frame's DMA
    a.lda_zp(Z_BALLY)
    a.sta_abs(OAM + 0)
    a.lda_imm(BALL_TILE)
    a.sta_abs(OAM + 1)
    a.lda_imm(0x00)  # palette 0, in front
    a.sta_abs(OAM + 2)
    a.lda_zp(Z_BALLX)
    a.sta_abs(OAM + 3)

    # ---- 5. backdrop colour code (same as exptest.nes) ----
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

    a.label("paint")
    a.lda_abs(0x2002)
    a.lda_imm(0x3F)
    a.sta_abs(0x2006)
    a.lda_imm(0x00)
    a.sta_abs(0x2006)
    a.lda_zp(Z_COL)
    a.sta_abs(0x2007)
    # Restore the address latch and scroll. With rendering ON this matters more
    # than in exptest: leaving v inside $3F00 would corrupt the next frame's
    # fetch addresses, and the two $2005 writes reset the fine/coarse scroll that
    # the $2006 pair has just clobbered.
    a.lda_abs(0x2002)
    a.lda_imm(0x00)
    a.sta_abs(0x2005)
    a.sta_abs(0x2005)
    a.sta_abs(0x2006)
    a.sta_abs(0x2006)

    a.jmp("main")

    # ---- data ----
    a.label("paldata")
    # backdrop, then three entries per background palette; sprite palette 0 gets
    # white in slot 1, which is the colour the ball's pattern selects.
    bg = [COL_NONE, 0x30, 0x10, 0x00]
    sp = [COL_NONE, 0x30, 0x16, 0x00]
    a.db(*(bg * 4), *(sp * 4))

    a.label("irq")
    a.rti()

    return a.assemble(size=0x4000, base=PRG_BASE, nmi="irq", reset="reset", irq="irq")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("-o", "--out", default="web/exppitch.nes")
    args = p.parse_args()

    prg = build_prg()
    chr_rom = build_chr()
    rom = ines_header(prg_16k=1, chr_8k=1, mapper=0) + prg + chr_rom

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(rom)
    print(f"wrote {out} ({len(rom)} bytes: 16 header + {len(prg)} PRG + {len(chr_rom)} CHR)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

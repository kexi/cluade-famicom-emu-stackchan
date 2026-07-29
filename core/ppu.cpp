#include "nes.h"

namespace nes {

// Standard NES palette (2C02), RGBA in little-endian ABGR words
const uint32_t NES_PALETTE[64] = {
    0xFF666666, 0xFF882A00, 0xFFA71214, 0xFFA4003B, 0xFF7E005C, 0xFF40006E, 0xFF00066C, 0xFF001D56,
    0xFF003533, 0xFF00480B, 0xFF005200, 0xFF084F00, 0xFF4D4000, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFADADAD, 0xFFD95F15, 0xFFFF4042, 0xFFFE2775, 0xFFCC1AA0, 0xFF7B1EB7, 0xFF2031B5, 0xFF004E99,
    0xFF006D6B, 0xFF008738, 0xFF00930C, 0xFF328F00, 0xFF8D7C00, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFFFFB064, 0xFFFF9092, 0xFFFF76C6, 0xFFFF6AF3, 0xFFCC6EFE, 0xFF7081FE, 0xFF229EEA,
    0xFF00BEBC, 0xFF00D888, 0xFF30E45C, 0xFF82E045, 0xFFDECD48, 0xFF4F4F4F, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFFFFDFC0, 0xFFFFD2D3, 0xFFFFC8E8, 0xFFFFC2FB, 0xFFEAC4FE, 0xFFC5CCFE, 0xFFA5D8F7,
    0xFF94E5E4, 0xFF96EFCF, 0xFFA6EDB7, 0xFFCCEBA9, 0xFFF4E9A8, 0xFFB8B8B8, 0xFF000000, 0xFF000000,
};

// Framebuffer-format palette, derived from NES_PALETTE once at reset. Rebuilding
// unconditionally (rather than a run-once guard) keeps repeated reset() calls cheap
// and correct without any static-init order concerns.
static Pixel palLut[64];

static void buildPalLut() {
    for (int i = 0; i < 64; i++) {
        uint32_t c = NES_PALETTE[i];
#ifdef NES_EMBEDDED
        uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        const uint16_t rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        // Stored byte-swapped because the LCD panel takes RGB565 big-endian while
        // the ESP32 is little-endian. Doing it here costs nothing (64 entries at
        // reset) and lets the framebuffer be handed to pushImageDMA as-is; the
        // alternative, M5GFX's setSwapBytes(true), makes the driver stage and
        // byte-swap every line into a bounce buffer, which both burns CPU and
        // rules out the zero-copy DMA the frontend needs to overlap transfers.
        palLut[i] = (Pixel)__builtin_bswap16(rgb565);
#else
        palLut[i] = c;
#endif
    }
}

#ifdef NES_EMBEDDED
static void buildBitSpread();
#endif

void PPU::reset() {
    buildPalLut();
#ifdef NES_EMBEDDED
    buildBitSpread();
#endif
    ctrl_ = mask_ = status_ = oamAddr_ = 0;
    v_ = t_ = 0; fineX_ = 0; w_ = false;
    readBuffer_ = 0;
    scanline_ = 261; dot_ = 0;
    oddFrame_ = false;
    frameReady = false;
    frameCount = 0;
#ifdef NES_EMBEDDED
    invalidateSpriteCandidates();   // OAM survives reset; the list must not go stale
#endif
}

uint16_t PPU::ntMirror(uint16_t addr) {
    // CIRAM A10 is derived by the cart from PPU A10/A11 — apply connector faults
    // to the table-select bits only (CIRAM A0-A9 run directly on the motherboard).
    // The mask is only consulted while a pin is open so a clean cart keeps the
    // plain shift it always had.
    int table = nes_.pinsFaulty_ ? (((addr & nes_.chrAddrAnd) & 0x0FFF) / 0x400)
                                 : ((addr & 0x0FFF) / 0x400);
    int off = addr & 0x3FF;
    uint16_t r;
    switch (nes_.mapper->mirroring()) {
    case Mirroring::Vertical:   r = ((table & 1) * 0x400) + off; break;
    case Mirroring::Horizontal: r = ((table >> 1) * 0x400) + off; break;
    case Mirroring::SingleLow:  r = off; break;
    case Mirroring::SingleHigh: r = 0x400 + off; break;
    default:                    r = ((table & 1) * 0x400) + off; break; // 4-screen fallback
    }
    if (nes_.pinsFaulty_ && !nes_.ciramA10Ok) r &= ~0x400;   // CIRAM A10 broken: bit floats low
#ifndef NES_EMBEDDED
    nes_.lastCiramA10 = (r & 0x400) != 0;
#endif
    return r;
}

void PPU::refreshChrWindow() {
#ifdef NES_EMBEDDED
    // The window is a raw pointer straight into CHR, which bypasses the address
    // and data masking entirely — so it must be given up while the connector is
    // faulty, or pattern fetches would silently ignore a broken CHR line.
    const bool canUseWindow = nes_.mapper && !nes_.pinsFaulty_;
    chrWindow_ = canUseWindow ? nes_.mapper->chrWindow() : nullptr;
#endif
}

// CHR/nametable access with connector faults applied. Shared by both builds and
// kept out of line: cold while the cart is seated properly.
uint8_t PPU::vramReadFaulty(uint16_t addr) {
    if (addr < 0x2000) {
        if (!nes_.powerOk || !nes_.ppuRdOk) return addr & 0xFF;   // bus floats
        uint8_t v = nes_.mapper->ppuRead(addr & nes_.chrAddrAnd & 0x1FFF);
        v = (v & nes_.chrDataAnd) | ((addr & 0xFF) & ~nes_.chrDataAnd);
#ifndef NES_EMBEDDED
        nes_.lastPpuData = v;
#endif
        return v;
    }
    if (!nes_.ciramCeOk) return addr & 0xFF;   // nametable RAM not selected
    uint8_t v = vram_[ntMirror(addr)];
#ifndef NES_EMBEDDED
    nes_.lastPpuData = v;
#endif
    return v;
}

void PPU::vramWriteFaulty(uint16_t addr, uint8_t v) {
    if (addr < 0x2000) {
        if (!nes_.powerOk || !nes_.ppuWrOk) return;
        nes_.mapper->ppuWrite(addr & nes_.chrAddrAnd & 0x1FFF,
                              (v & nes_.chrDataAnd) | ((addr & 0xFF) & ~nes_.chrDataAnd));
        return;
    }
    if (!nes_.ciramCeOk) return;
    vram_[ntMirror(addr)] = v;
}

uint8_t PPU::vramRead(uint16_t addr) {
    addr &= 0x3FFF;
#ifndef NES_EMBEDDED
    if (addr < 0x3F00) { nes_.lastPpuAddr = addr; nes_.ppuRdPulse = true; }
#endif
    if (addr < 0x3F00) {
        if (nes_.pinsFaulty_) return vramReadFaulty(addr);
        if (addr < 0x2000) {
            if (chrWindow_) return chrWindow_[addr & 0x1FFF];
            return nes_.mapper->ppuRead(addr & 0x1FFF);
        }
        return vram_[ntMirror(addr)];
    }
    addr &= 0x1F;
    if (addr >= 0x10 && (addr & 3) == 0) addr &= 0x0F;
    return palette_[addr];
}

void PPU::vramWrite(uint16_t addr, uint8_t v) {
    addr &= 0x3FFF;
#ifndef NES_EMBEDDED
    if (addr < 0x3F00) { nes_.lastPpuAddr = addr; nes_.lastPpuData = v; nes_.ppuWrPulse = true; }
#endif
    if (addr < 0x3F00) {
        if (nes_.pinsFaulty_) { vramWriteFaulty(addr, v); return; }
        if (addr < 0x2000) { nes_.mapper->ppuWrite(addr & 0x1FFF, v); return; }
        vram_[ntMirror(addr)] = v;
        return;
    }
    addr &= 0x1F;
    if (addr >= 0x10 && (addr & 3) == 0) addr &= 0x0F;
    palette_[addr] = v;
}

uint8_t NES_HOT PPU::readReg(uint16_t addr) {
    switch (addr & 7) {
    case 2: {
        uint8_t r = (status_ & 0xE0) | (openBus_ & 0x1F);
        status_ &= ~0x80;   // clear vblank
        w_ = false;
        openBus_ = r;
        return r;
    }
    case 4:
        openBus_ = oam_[oamAddr_];
        return openBus_;
    case 7: {
        uint8_t r;
        if ((v_ & 0x3FFF) >= 0x3F00) {
            r = vramRead(v_);
            readBuffer_ = vramRead(v_ - 0x1000);  // underlying nametable
        } else {
            r = readBuffer_;
            readBuffer_ = vramRead(v_);
        }
        v_ += (ctrl_ & 0x04) ? 32 : 1;
        openBus_ = r;
        return r;
    }
    default:
        return openBus_;
    }
}

void NES_HOT PPU::writeReg(uint16_t addr, uint8_t val) {
    openBus_ = val;
    switch (addr & 7) {
    case 0: {
        bool wasNmi = ctrl_ & 0x80;
        ctrl_ = val;
        t_ = (t_ & 0xF3FF) | ((val & 3) << 10);
        // NMI edge if vblank set and NMI newly enabled
        if (!wasNmi && (ctrl_ & 0x80) && (status_ & 0x80)) nes_.cpu.nmi();
        break;
    }
    case 1: mask_ = val; break;
    case 3: oamAddr_ = val; break;
    case 4:
        oam_[oamAddr_++] = val;
#ifdef NES_EMBEDDED
        invalidateSpriteCandidates();
#endif
        break;
    case 5:
        if (!w_) {
            t_ = (t_ & 0xFFE0) | (val >> 3);
            fineX_ = val & 7;
        } else {
            t_ = (t_ & 0x8C1F) | ((val & 0xF8) << 2) | ((val & 7) << 12);
        }
        w_ = !w_;
        break;
    case 6:
        if (!w_) {
            t_ = (t_ & 0x00FF) | ((val & 0x3F) << 8);
        } else {
            t_ = (t_ & 0xFF00) | val;
            v_ = t_;
        }
        w_ = !w_;
        break;
    case 7:
        vramWrite(v_, val);
        v_ += (ctrl_ & 0x04) ? 32 : 1;
        break;
    }
}

void PPU::writeOamDma(uint8_t, const uint8_t* page) {
    for (int i = 0; i < 256; i++) oam_[(oamAddr_ + i) & 0xFF] = page[i];
#ifdef NES_EMBEDDED
    invalidateSpriteCandidates();
#endif
}

void PPU::incHoriz() {
    if ((v_ & 0x1F) == 31) { v_ &= ~0x1F; v_ ^= 0x0400; }
    else v_++;
}

void PPU::incVert() {
    if ((v_ & 0x7000) != 0x7000) { v_ += 0x1000; return; }
    v_ &= ~0x7000;
    int y = (v_ >> 5) & 0x1F;
    if (y == 29) { y = 0; v_ ^= 0x0800; }
    else if (y == 31) y = 0;
    else y++;
    v_ = (v_ & ~0x03E0) | (y << 5);
}

void NES_HOT PPU::fetchBg() {
    switch (dot_ & 7) {
    case 1:
        // reload shifters
        bgPatLo_ = (bgPatLo_ & 0xFF00) | patLo_;
        bgPatHi_ = (bgPatHi_ & 0xFF00) | patHi_;
        bgAttrLo_ = (bgAttrLo_ & 0xFF00) | ((atByte_ & 1) ? 0xFF : 0);
        bgAttrHi_ = (bgAttrHi_ & 0xFF00) | ((atByte_ & 2) ? 0xFF : 0);
        ntByte_ = vramRead(0x2000 | (v_ & 0x0FFF));
        break;
    case 3: {
        uint8_t at = vramRead(0x23C0 | (v_ & 0x0C00) | ((v_ >> 4) & 0x38) | ((v_ >> 2) & 0x07));
        int shift = ((v_ >> 4) & 4) | (v_ & 2);
        atByte_ = (at >> shift) & 3;
        break;
    }
    case 5:
        patLo_ = vramRead(((ctrl_ & 0x10) << 8) + ntByte_ * 16 + ((v_ >> 12) & 7));
        break;
    case 7:
        patHi_ = vramRead(((ctrl_ & 0x10) << 8) + ntByte_ * 16 + ((v_ >> 12) & 7) + 8);
        break;
    case 0:
        incHoriz();
        break;
    }
}

// `line` is the scanline the sprites are *evaluated against*, which is one less
// than the line they are drawn on (the hardware's OAM Y byte is top-1). The
// caller supplies it because the two render paths evaluate at different dots.
#ifdef NES_EMBEDDED
// Collect the OAM entries whose Y byte can reach a visible line.
//
// The bound is on Y alone, so one list serves every scanline and both sprite
// heights: an entry is kept when some line in 0..239 could satisfy
// 0 <= line - Y < height. With the largest height (16) that is Y > 239 - 16,
// i.e. Y >= 240 is unreachable — and Y is a uint8_t, so the test is just a
// compare against 240 with no dependence on the current line or ctrl_ bit 5.
// Deliberately not tightened per height or per line: a list that depended on
// either would have to be rebuilt when they changed, and the whole point is that
// this survives untouched across all 240 evaluations of a frame.
void PPU::rebuildSpriteCandidates() {
    spriteCandidateCount_ = 0;
    for (int i = 0; i < 64; i++) {
        const bool reachable = oam_[i * 4] < 240;
        if (reachable) spriteCandidates_[spriteCandidateCount_++] = (uint8_t)i;
    }
    spriteCandidatesValid_ = true;
}
#endif

void PPU::evalSprites(int line) {
    spriteCount_ = 0;
    int height = (ctrl_ & 0x20) ? 16 : 8;
    bool overflow = false;
    // The loop source differs between builds so that the reference path keeps the
    // exact plain scan it always had (its object file is byte-compared against the
    // pre-optimisation build); only the embedded one walks the narrowed list. The
    // body below is shared and sees the same entries in the same order either way.
#ifdef NES_EMBEDDED
    if (!spriteCandidatesValid_) rebuildSpriteCandidates();
    const int candidateCount = spriteCandidateCount_;
    for (int c = 0; c < candidateCount; c++) {
        const int i = spriteCandidates_[c];
#else
    for (int i = 0; i < 64; i++) {
#endif
        int sy = oam_[i * 4];
        int row = line - sy;
        if (row < 0 || row >= height) continue;
        if (spriteCount_ == 8) { overflow = true; break; }
        uint8_t tile = oam_[i * 4 + 1];
        uint8_t attr = oam_[i * 4 + 2];
        int sx = oam_[i * 4 + 3];
        if (attr & 0x80) row = height - 1 - row;    // vertical flip
        uint16_t patAddr;
        if (height == 16) {
            uint16_t bank = (tile & 1) << 12;
            uint8_t t = tile & 0xFE;
            if (row >= 8) { t++; row -= 8; }
            patAddr = bank + t * 16 + row;
        } else {
            patAddr = ((ctrl_ & 0x08) << 9) + tile * 16 + row;
        }
        Sprite& s = sprites_[spriteCount_++];
        s.patLo = vramRead(patAddr);
        s.patHi = vramRead(patAddr + 8);
        s.attr = attr;
        s.x = sx;
        s.sprite0 = (i == 0);
    }
    if (overflow) status_ |= 0x20;
}

void NES_HOT PPU::renderDot() {
    int x = dot_ - 1;
    int y = scanline_;

    // background pixel
    int bgPixel = 0, bgPal = 0;
    if ((mask_ & 0x08) && (x >= 8 || (mask_ & 0x02))) {
        int bit = 15 - fineX_;
        bgPixel = ((bgPatLo_ >> bit) & 1) | (((bgPatHi_ >> bit) & 1) << 1);
        bgPal = ((bgAttrLo_ >> bit) & 1) | (((bgAttrHi_ >> bit) & 1) << 1);
    }

    // sprite pixel
    int spPixel = 0, spPal = 0;
    bool spBehind = false, spZero = false;
    if ((mask_ & 0x10) && (x >= 8 || (mask_ & 0x04))) {
        for (int i = 0; i < spriteCount_; i++) {
            Sprite& s = sprites_[i];
            int off = x - s.x;
            if (off < 0 || off > 7) continue;
            int bit = (s.attr & 0x40) ? off : 7 - off;   // horizontal flip
            int px = ((s.patLo >> bit) & 1) | (((s.patHi >> bit) & 1) << 1);
            if (px == 0) continue;
            spPixel = px;
            spPal = s.attr & 3;
            spBehind = s.attr & 0x20;
            spZero = s.sprite0;
            break;
        }
    }

    // sprite 0 hit
    if (spZero && spPixel && bgPixel && x < 255) status_ |= 0x40;

    int palIndex;
    if (bgPixel == 0 && spPixel == 0) palIndex = 0;
    else if (bgPixel == 0) palIndex = 0x10 + spPal * 4 + spPixel;
    else if (spPixel == 0) palIndex = bgPal * 4 + bgPixel;
    else palIndex = spBehind ? (bgPal * 4 + bgPixel) : (0x10 + spPal * 4 + spPixel);

    uint8_t colorIdx = palette_[palIndex] & 0x3F;
    if (mask_ & 0x01) colorIdx &= 0x30;   // greyscale
    framebuffer[y * 256 + x] = palLut[colorIdx];
}

#ifdef NES_EMBEDDED
// 1bpp byte -> 8 interleaved 2-bit lanes: bit n of the input lands on bit 2n of
// the output. `spread[lo] | spread[hi] << 1` then yields the whole tile row's
// 2-bit pattern values in one 16-bit word, replacing eight shift/mask pairs.
static uint16_t bitSpread[256];
static bool bitSpreadReady = false;

static void buildBitSpread() {
    if (bitSpreadReady) return;
    for (int b = 0; b < 256; b++) {
        uint16_t w = 0;
        for (int i = 0; i < 8; i++) if (b & (1 << i)) w |= (uint16_t)1 << (i * 2);
        bitSpread[b] = w;
    }
    bitSpreadReady = true;
}

// Render one visible scanline in a single pass.
//
// The dot-accurate path costs ~305 CPU cycles per dot on a 240MHz ESP32-S3
// against a ~45 cycle budget for 60fps, because the per-dot pipeline re-derives
// tile state 256 times per line. This walks the line tile by tile instead:
// nametable/attribute/pattern bytes are fetched once per 8 pixels, and the
// sprite pass writes only the sprites that actually overlap the line.
//
// The visible difference from the dot pipeline is that mid-scanline writes to
// $2000/$2001/$2005/$2006 no longer split a line. Sprite 0 hit is still
// evaluated per pixel (games poll it for raster effects), and the loopy
// v/t updates below run on the same dots as before.
//
// Draw=false is the display-skip path: it keeps every CPU-observable effect
// (sprite 0 hit above all) but writes no pixels, so the frontend can leave a
// DMA transfer of the previous frame running across it.
template <bool Draw>
void NES_HOT PPU::renderScanline() {
    const int y = scanline_;
    Pixel* const out = framebuffer + y * 256;

    const bool showBg = mask_ & 0x08;
    const bool showSp = mask_ & 0x10;
    const bool greyscale = mask_ & 0x01;

    // Sprite 0 hit is the only side effect of the sprite pass, so once it is
    // latched a non-drawing line has nothing left to do at all.
    const bool needSprite0 = !Draw && showSp && showBg && !(status_ & 0x40) && hasSprite0();
    if (!Draw && !needSprite0) return;

    // Line-invariant fetch shortcuts, resolved once instead of per tile byte.
    //
    // The two nametable pages a line can touch are pinned here rather than
    // re-derived per fetch: ntMirror() reruns the mirroring switch and the
    // pinsFaulty_ test on all four of a tile's VRAM reads, and only bit 10 of v
    // can change within a line (the tile-31 wrap flips it; bit 11 is fixed until
    // incVert()). Resolved by calling ntMirror() rather than by reimplementing
    // the mirroring, so the two paths cannot drift apart.
    //
    // chrWindow_ is already null on a faulty connector, but a mapper that banks
    // CHR finer than 8KB leaves it null on a clean one too, so the direct path is
    // gated on the pointer and not on the fault flag alone.
    const bool fetchFast = chrWindow_ && !nes_.pinsFaulty_;
    const uint8_t* ntFast[2] = {nullptr, nullptr};
    if (fetchFast) {
        const uint16_t page = v_ & 0x0800;
        ntFast[0] = vram_ + ntMirror(0x2000 | page);
        ntFast[1] = vram_ + ntMirror(0x2400 | page);
    }

    // --- background: walk tiles, expanding 8 pixels at a time ---
    uint8_t bgPix[256];      // 2-bit pattern value per pixel (0 = transparent)
    if (showBg) {
        // v_ currently points at the first tile of this line.
        uint16_t v = v_;
        const int fineY = (v >> 12) & 7;
        const uint16_t patBase = (ctrl_ & 0x10) << 8;
        uint8_t bdIdx = palette_[0] & 0x3F;
        if (greyscale) bdIdx &= 0x30;
        const Pixel bdColor = palLut[bdIdx];

        // All four background subpalettes in framebuffer format, indexed
        // [attribute][pattern value]. Entry 0 of each is the backdrop, so the
        // pixel loop indexes straight through instead of branching on p != 0 and
        // then selecting among three.
        //
        // Resolved per line rather than per tile: the batched renderer draws the
        // line in one pass, so palette RAM cannot change part-way through it and
        // the 32 tiles were re-deriving the same four rows. Per frame would be
        // cheaper still, but $3F00 writes between lines are how games cycle
        // colours, and a line is already short enough for this to disappear.
        Pixel bgPalette[4][4];
        if (Draw) {
            for (int pi = 0; pi < 4; pi++) {
                const uint8_t* const p = palette_ + pi * 4;
                bgPalette[pi][0] = bdColor;
                for (int ci = 1; ci < 4; ci++) {
                    uint8_t idx = p[ci] & 0x3F;
                    if (greyscale) idx &= 0x30;
                    bgPalette[pi][ci] = palLut[idx];
                }
            }
        }

        // Per-tile state the inner loop needs, filled by fetchTile().
        uint16_t pat = 0;        // 8 lanes of 2-bit pattern, lane i = pixel i
        const Pixel* tileColor = bgPalette[0];   // this tile's row of the table
        const uint8_t* const chr = chrWindow_;
        auto fetchTile = [&]() {
            uint8_t nt, at, lo, hi;
            if (fetchFast) {
                // ntMirror() passes CIRAM A0-A9 straight through, so the low 10
                // bits of the address index the resolved page directly.
                const uint8_t* const page = ntFast[(v >> 10) & 1];
                nt = page[v & 0x03FF];
                at = page[0x03C0 | ((v >> 4) & 0x38) | ((v >> 2) & 0x07)];
                const uint16_t addr = patBase + nt * 16 + fineY;
                lo = chr[addr & 0x1FFF];
                hi = chr[(addr + 8) & 0x1FFF];
            } else {
                nt = vramRead(0x2000 | (v & 0x0FFF));
                at = vramRead(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07));
                const uint16_t addr = patBase + nt * 16 + fineY;
                lo = vramRead(addr);
                hi = vramRead(addr + 8);
            }
            const int atShift = ((v >> 4) & 4) | (v & 2);
            const uint8_t pal = (at >> atShift) & 3;
            // lane i must hold pixel i, i.e. bit 7-i of the pattern bytes, so the
            // spread words are bit-reversed by walking the tile right to left.
            pat = (uint16_t)(bitSpread[lo] | (uint16_t)(bitSpread[hi] << 1));
            if (Draw) tileColor = bgPalette[pal];
            // advance to the next tile exactly as incHoriz() would
            if ((v & 0x1F) == 31) { v &= ~0x1F; v ^= 0x0400; }
            else v++;
        };
        // The lanes are consumed left to right, so each pixel is the top of a
        // running shift register rather than a variable shift by (7-n)*2 — the
        // Xtensa has no barrel shift by a register operand without setting up
        // SAR, which this loop was doing 256 times a line.
        auto emit = [&](uint16_t& lanes, int at) {
            const uint8_t p = (uint8_t)(lanes >> 14);
            lanes = (uint16_t)(lanes << 2);
            bgPix[at] = p;
            if (Draw) out[at] = tileColor[p];
        };

        int px = 0;
        const int fx = fineX_;
        // Head: the first tile is shifted left by fine X, so only its last 8-fx
        // columns are on screen. Peeled out so the main loop below needs no
        // per-pixel bounds test.
        fetchTile();
        uint16_t lanes = (uint16_t)(pat << (fx * 2));
        for (int n = fx; n < 8; n++, px++) emit(lanes, px);
        // Body: whole tiles, entirely on screen.
        while (px + 8 <= 256) {
            fetchTile();
            lanes = pat;
            emit(lanes, px + 0); emit(lanes, px + 1);
            emit(lanes, px + 2); emit(lanes, px + 3);
            emit(lanes, px + 4); emit(lanes, px + 5);
            emit(lanes, px + 6); emit(lanes, px + 7);
            px += 8;
        }
        // Tail: the fine-X shift leaves fx columns of one more tile visible.
        if (px < 256) {
            fetchTile();
            lanes = pat;
            for (; px < 256; px++) emit(lanes, px);
        }

        // left-edge clipping
        if (!(mask_ & 0x02)) {
            for (int i = 0; i < 8; i++) {
                bgPix[i] = 0;
                if (Draw) out[i] = bdColor;
            }
        }
    } else {
        memset(bgPix, 0, sizeof(bgPix));
        if (Draw) {
            uint8_t bdIdx = palette_[0] & 0x3F;
            if (greyscale) bdIdx &= 0x30;
            const Pixel bdColor = palLut[bdIdx];
            for (int px = 0; px < 256; px++) out[px] = bdColor;
        }
    }

    if (!showSp) return;

    // --- sprites: front-to-back, so the first writer of a pixel wins ---
    //
    // Pixel ownership is a bitmask rather than a bool[256]: clearing it is four
    // stores instead of a 256-byte memset, and eight sprites can never justify
    // that much zeroing. Tested and set bit by bit — the spans are 8 pixels at an
    // arbitrary offset, so a word-at-a-time merge would need shifts and a
    // carry-in across the boundary for no gain at this width.
    uint64_t written[4] = {0, 0, 0, 0};
    const int clip = (mask_ & 0x04) ? 0 : 8;
    for (int i = 0; i < spriteCount_; i++) {
        const Sprite& s = sprites_[i];
        // Skipping non-sprite-0 entries outright is only safe when nothing is
        // being drawn — with Draw they still own pixels via `written`.
        if (!Draw && !s.sprite0) continue;
        // A row with no opaque pixel writes nothing and cannot raise sprite 0
        // hit (which needs p != 0), so it is skipped whole rather than being
        // walked eight times to reach the same conclusion.
        const bool rowTransparent = (s.patLo | s.patHi) == 0;
        if (rowTransparent) continue;

        // Clip the column span once instead of testing every pixel.
        int c0 = clip - s.x; if (c0 < 0) c0 = 0;
        int cEnd = 256 - s.x; if (cEnd > 8) cEnd = 8;
        for (int c = c0; c < cEnd; c++) {
            const int px = s.x + c;
            const int bit = (s.attr & 0x40) ? c : 7 - c;   // horizontal flip
            const int p = ((s.patLo >> bit) & 1) | (((s.patHi >> bit) & 1) << 1);
            if (p == 0) continue;

            // sprite 0 hit: first opaque sprite-0 pixel over opaque background
            if (s.sprite0 && bgPix[px] && px < 255 && showBg) status_ |= 0x40;
            if (!Draw) continue;

            const uint64_t ownBit = (uint64_t)1 << (px & 63);
            uint64_t& ownWord = written[px >> 6];
            if (ownWord & ownBit) continue;      // a nearer sprite already claimed it
            ownWord |= ownBit;
            const bool behind = s.attr & 0x20;
            if (behind && bgPix[px]) continue;   // background has priority

            uint8_t idx = palette_[0x10 + (s.attr & 3) * 4 + p] & 0x3F;
            if (greyscale) idx &= 0x30;
            out[px] = palLut[idx];
        }
    }
}
// NES_HOT on the template definition does not reach the instantiations, so both
// copies land in flash. Leave them there: forcing either into IRAM overflows the
// Xtensa l32r literal window ("dangerous relocation: literal placed after use"),
// because these functions are far larger than the 256KB-addressable span l32r
// reaches backwards from a literal pool. Splitting them small enough to fit is
// not worth it — measurement below put the win in the skip path, not here.
template void PPU::renderScanline<true>();
template void PPU::renderScanline<false>();
#endif // NES_EMBEDDED

#ifdef NES_EMBEDDED
// Advance `dots` PPU cycles.
//
// The batched renderer already does a whole line's pixels in one pass, so the
// only dots that carry work are the handful listed below. The rest are pure
// counter increments, and running them through step() one at a time costs far
// more than the increments themselves. This jumps straight to the next dot that
// matters and calls step() only there.
//
// It never skips onto dot 340: step() has to run on the last dot of a line so
// the scanline wrap (and dot 1 of the next line, where vblank/NMI fires) is not
// jumped over. That was the bug in the earlier attempt at this.
void NES_HOT PPU::stepMany(int dots) {
    while (dots > 0) {
        const bool rendering = renderingEnabled();
        const bool prerender = scanline_ == 261;
        const bool visible = scanline_ < 240;

        // The next dot at or after dot_ that carries work. Everything strictly
        // before it is a pure counter increment and can be coalesced.
        int next = 340;   // last dot of the line: step() must run to wrap it
        if (dot_ <= 1) {
            // Dot 1 carries work on every line regardless of rendering state:
            // scanline 241 sets vblank (which is what ends the frame) and 261
            // clears the status flags. Skipping it stalls runFrame() forever.
            next = 1;
        } else if (rendering) {
            // `<=`, not `<`: after step() has run on a work dot, dot_ already
            // sits one past it, and `<` would then select the *next* work dot
            // and skip straight over the one in between. dot 257 (the
            // horizontal v<-t copy) was lost that way on nearly every line,
            // leaving the scroll bits stale after a $2006 write.
            if (dot_ <= 256) next = 256;       // render + incVert + evalSprites
            else if (dot_ <= 257) next = 257;  // horizontal v<-t
            else if (dot_ <= 260) next = 260;  // mapper scanline IRQ
            else if (prerender && dot_ <= 304) next = dot_ < 280 ? 280 : dot_;
        }
        // Rendering off on a visible line: every dot 1..256 paints the backdrop,
        // so there is nothing to coalesce there.
        if (visible && !rendering && dot_ >= 1 && dot_ <= 256) next = dot_;
        if (next > 340) next = 340;

        // Skip up to `next` but never past it: step() tests dot_ on entry and
        // increments afterwards, so it has to run *on* the dot that carries the
        // work. When dot_ already sits on that dot the gap is 0 and step() runs
        // immediately, which is what keeps consecutive work dots (256 then 257)
        // from being skipped over.
        const int gap = next - dot_;
        if (gap > 0) {
            const int skip = (gap < dots) ? gap : dots;
            dot_ += skip;
            dots -= skip;
            if (dots == 0) return;
        }
        step();
        dots--;
    }
}
#endif // NES_EMBEDDED

void NES_HOT PPU::step() {
    bool rendering = renderingEnabled();
    bool visible = scanline_ < 240;
    bool prerender = scanline_ == 261;

    if ((visible || prerender) && rendering) {
#ifdef NES_EMBEDDED
        // Batched path: do the whole line once, at the dot where the dot-accurate
        // pipeline would have finished it, then keep only the loopy bookkeeping.
        // The dot-accurate path evaluates at dot 257 of line N-1 and draws the
        // result on line N. Drawing happens here at dot 256 of line N, so the
        // evaluation is asked for line N-1 to keep the same one-line offset;
        // evaluating line N directly would put every sprite one row too high.
        if (dot_ == 256) {
            if (visible) {
                evalSprites(scanline_ - 1);
                if (renderThisFrame) renderScanline<true>();
                else renderScanline<false>();
            }
            incVert();
        }
#else
        // background fetch pipeline
        if ((dot_ >= 1 && dot_ <= 256) || (dot_ >= 321 && dot_ <= 336)) {
            if (visible && dot_ >= 1 && dot_ <= 256) renderDot();
            fetchBg();
            // shift
            bgPatLo_ <<= 1; bgPatHi_ <<= 1; bgAttrLo_ <<= 1; bgAttrHi_ <<= 1;
        }
        if (dot_ == 256) incVert();
#endif
        if (dot_ == 257) {
            // copy horizontal bits t -> v
            v_ = (v_ & ~0x041F) | (t_ & 0x041F);
#ifndef NES_EMBEDDED
            if (visible) evalSprites(scanline_);
#endif
        }
        if (prerender && dot_ >= 280 && dot_ <= 304) {
            v_ = (v_ & ~0x7BE0) | (t_ & 0x7BE0);
        }
        if (dot_ == 260) nes_.mapper->scanline();   // MMC3 approximation
    } else if (visible && !rendering && dot_ >= 1 && dot_ <= 256) {
        // Rendering disabled: draw backdrop color. Pure framebuffer write with no
        // CPU-visible effect, so a skipped frame drops it entirely.
#ifdef NES_EMBEDDED
        if (renderThisFrame)
#endif
        {
            uint16_t bd = ((v_ & 0x3FFF) >= 0x3F00) ? (v_ & 0x1F) : 0;
            if (bd >= 0x10 && (bd & 3) == 0) bd &= 0x0F;
            framebuffer[scanline_ * 256 + dot_ - 1] = palLut[palette_[bd] & 0x3F];
        }
    }

    if (scanline_ == 241 && dot_ == 1) {
        status_ |= 0x80;
        if (ctrl_ & 0x80) nes_.cpu.nmi();
        frameReady = true;
        frameCount++;
    }
    if (prerender && dot_ == 1) {
        status_ &= ~(0x80 | 0x40 | 0x20);
    }

    dot_++;
    if (dot_ > 340) {
        dot_ = 0;
        scanline_++;
        if (scanline_ > 261) {
            scanline_ = 0;
            oddFrame_ = !oddFrame_;
            if (oddFrame_ && rendering) dot_ = 1;   // odd frame skip
        }
    }
}

} // namespace nes

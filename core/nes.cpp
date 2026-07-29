#include "nes.h"

// Cycle-accurate timing for the emulation breakdown. xthal_get_ccount() reads
// the Xtensa CCOUNT register directly — esp_timer_get_time() costs more than
// some of the batches being measured and its 1us resolution is coarser than a
// whole APU step. core-macros.h rather than esp_cpu.h because the latter is
// IDF-4.4 vintage here and pulls in esp_err_t without including esp_err.h, so it
// does not compile standalone. Kept inside the guard so the shared core still
// builds for web and host with no ESP headers on the include path.
#ifdef NES_PROFILE
#include <xtensa/core-macros.h>
#endif

namespace nes {

// Famicom 60-pin cartridge connector: recompute signal masks from pin states.
// Pinout (nesdev wiki): front 1-30 = GND, CPU A11..A0, R/W, /IRQ, GND, PPU /RD,
// CIRAM A10, PPU A6..A0, PPU D0..D3, +5V / back 31-60 = +5V, M2, CPU A12-A14,
// CPU D7..D0, /ROMSEL, sound in/out, PPU /WR, CIRAM /CE, PPU /A13, PPU A7..A13,
// PPU D7..D4.
void NES::updatePins() {
    // power rails are redundant: either GND pin (1/16) and either +5V pin (30/31) suffices
    powerOk = (pinOk[1] || pinOk[16]) && (pinOk[30] || pinOk[31]);
    // CPU address: pins 2..13 = A11..A0, 33..35 = A12..A14
    prgAddrAnd = 0;
    for (int i = 0; i < 12; i++) if (pinOk[13 - i]) prgAddrAnd |= 1 << i;      // A0-A11
    for (int i = 0; i < 3; i++)  if (pinOk[33 + i]) prgAddrAnd |= 1 << (12 + i); // A12-A14
    // CPU data: pins 43..36 = D0..D7
    prgDataAnd = 0;
    for (int i = 0; i < 8; i++) if (pinOk[43 - i]) prgDataAnd |= 1 << i;
    rwOk = pinOk[14];
    irqOk = pinOk[15];
    m2Ok = pinOk[32];
    romselOk = pinOk[44];
    soundOk = pinOk[45] && pinOk[46];
    // PPU address: pins 25..19 = A0..A6, 50..56 = A7..A13
    chrAddrAnd = 0;
    for (int i = 0; i < 7; i++) if (pinOk[25 - i]) chrAddrAnd |= 1 << i;       // A0-A6
    for (int i = 0; i < 7; i++) if (pinOk[50 + i]) chrAddrAnd |= 1 << (7 + i); // A7-A13
    // PPU data: pins 26..29 = D0..D3, 60..57 = D4..D7
    chrDataAnd = 0;
    for (int i = 0; i < 4; i++) if (pinOk[26 + i]) chrDataAnd |= 1 << i;
    for (int i = 0; i < 4; i++) if (pinOk[60 - i]) chrDataAnd |= 1 << (4 + i);
    ppuRdOk = pinOk[17];
    ppuWrOk = pinOk[47];
    ciramA10Ok = pinOk[18];
    ciramCeOk = pinOk[48] && pinOk[49];   // most carts drive CIRAM /CE from PPU /A13

    // Single gate for the hot paths. Derived from the raw pins rather than the
    // masks above so that a pin whose only effect is cosmetic still counts.
    pinsFaulty_ = false;
    for (int i = 1; i <= 60; i++) {
        if (!pinOk[i]) { pinsFaulty_ = true; break; }
    }
    // /IRQ reachability folded in here so irqLineLevel() stays a single bool test.
    mapperIrqUsable_ = mapperHasIrq_ && irqOk;
}

// bit(n-1) set = pin n making contact.
void NES::applyPinMask(uint64_t mask) {
    bool changed = false;
    for (int pin = 1; pin <= 60; pin++) {
        const bool connected = (mask >> (pin - 1)) & 1;
        if (pinOk[pin] == connected) continue;
        pinOk[pin] = connected;
        changed = true;
    }
    if (!changed) return;
    updatePins();
    // A broken CHR address/data line has to be seen by pattern fetches, so the
    // PPU's direct-CHR shortcut must be re-evaluated (refreshChrWindow drops it
    // to null while the connector is faulty).
    ppu.refreshChrWindow();
#ifdef NES_EMBEDDED
    // Same reasoning on the CPU side: the PRG windows bypass the masking too.
    refreshPrgWindows();
#endif
}

// ---- shared cartridge-connector fault paths ----
//
// Both builds route through these while pinsFaulty_ is set, so the embedded core
// masks bit-for-bit identically to the reference. Deliberately out of line: they
// are cold, and keeping them out of the callers stops the fault handling from
// bloating the hot instruction fetch/store paths.
uint8_t NES::cartReadFaulty(uint16_t addr) {
    if (!powerOk || !m2Ok) return cartOpenBus(addr);
    if (addr >= 0x8000 && !romselOk) return cartOpenBus(addr);
    const uint16_t maskedAddr = (addr & 0x8000) | (addr & prgAddrAnd);
    const uint8_t v = mapper->cpuRead(maskedAddr);
    return (v & prgDataAnd) | (cartOpenBus(addr) & ~prgDataAnd);
}

void NES::cartWriteFaulty(uint16_t addr, uint8_t v) {
    if (!powerOk || !m2Ok || !rwOk) return;
    if (addr >= 0x8000 && !romselOk) return;
    const uint16_t maskedAddr = (addr & 0x8000) | (addr & prgAddrAnd);
    mapper->cpuWrite(maskedAddr, (v & prgDataAnd) | (cartOpenBus(addr) & ~prgDataAnd));
}

bool NES::loadRom(const uint8_t* data, size_t size) {
    mapper = nes::loadRom(data, size);
    if (!mapper) return false;
    refreshMapperCaps();
    powerOn();
    ppu.refreshChrWindow();
#ifdef NES_EMBEDDED
    refreshPrgWindows();
#endif
    return true;
}

void NES::reset() {
    // like the real RESET button: work RAM survives
    ppu.reset();
    apu.reset();
    cpu.reset();
}

void NES::powerOn() {
    memset(ram, 0, sizeof(ram));
    reset();
}

void NES::runCycles(int n) {
    while (n > 0) {
#ifndef NES_EMBEDDED
        cpu.irq(apu.irqPending() || (mapper && irqOk && mapper->irqPending()));
#else
        cpu.irq(irqLineLevel());
#endif
        int cycles = cpu.step();
        for (int i = 0; i < cycles; i++) {
            apu.step();
            ppu.step();
            ppu.step();
            ppu.step();
            cycleCount++;
            if (mapper) mapper->cpuCycle();
#ifndef NES_EMBEDDED
            if (probePin) probeSample();
#endif
        }
        n -= cycles;
    }
}

uint8_t NES_HOT NES::cpuRead(uint16_t addr) {
#ifndef NES_EMBEDDED
    lastCpuAddr = addr;
    lastCpuWrite = false;
    uint8_t v = cpuReadBus(addr);
    lastCpuData = v;
    return v;
#else
    return cpuReadBus(addr);
#endif
}

uint8_t NES_HOT NES::cpuReadBus(uint16_t addr) {
    if (addr < 0x2000) return ram[addr & 0x7FF];
#ifdef NES_EMBEDDED
    // Cartridge ROM through the cached bank pointers. Placed above the register
    // decode because $8000+ is by far the most common address here (every
    // instruction fetch), and the ranges are disjoint so the order is free.
    // prgFastValid_ is false whenever a pin is broken, which is what routes the
    // faulty connector back down to cartReadFaulty() below.
    const bool prgFastPath = addr >= 0x8000 && prgFastValid_;
    if (prgFastPath) return prgWin_[(addr >> 13) & 3][addr & 0x1FFF];

    // These observe PPU/APU state, so the deferred cycles must land first.
    if (addr < 0x4000) { catchUp(); return ppu.readReg(addr); }
    if (addr == 0x4015) { catchUp(); return apu.readStatus(); }
#else
    if (addr < 0x4000) return ppu.readReg(addr);
    if (addr == 0x4015) return apu.readStatus();
#endif
    if (addr == 0x4016) return pad[0].read();
    if (addr == 0x4017) return pad[1].read();
    if (addr < 0x4020) return 0;
    if (!mapper) return 0;
    // Clean connector: straight to the mapper, exactly as before pin emulation
    // existed. Only a broken pin diverts through the masking helper.
    if (pinsFaulty_) return cartReadFaulty(addr);
    return mapper->cpuRead(addr);
}

void NES_HOT NES::cpuWrite(uint16_t addr, uint8_t v) {
#ifndef NES_EMBEDDED
    lastCpuAddr = addr;
    lastCpuData = v;
    lastCpuWrite = true;
#endif
    if (addr < 0x2000) { ram[addr & 0x7FF] = v; return; }
#ifdef NES_EMBEDDED
    if (addr < 0x4000) { catchUp(); ppu.writeReg(addr, v); return; }
#else
    if (addr < 0x4000) { ppu.writeReg(addr, v); return; }
#endif
    if (addr == 0x4014) {
        // OAM DMA
#ifdef NES_EMBEDDED
        catchUp();
#endif
        uint8_t page[256];
        uint16_t base = v << 8;
        for (int i = 0; i < 256; i++) page[i] = cpuRead(base + i);
        ppu.writeOamDma(v, page);
        cpu.addStall(513);
        return;
    }
    if (addr >= 0x4000 && addr <= 0x4017) apuRegShadow[addr - 0x4000] = v;
    if (addr == 0x4016) { pad[0].writeStrobe(v); pad[1].writeStrobe(v); return; }
#ifdef NES_EMBEDDED
    if (addr < 0x4020) { catchUp(); apu.writeReg(addr, v); return; }
#else
    if (addr < 0x4020) { apu.writeReg(addr, v); return; }
#endif
    if (!mapper) return;
    if (pinsFaulty_) {
        cartWriteFaulty(addr, v);
    } else {
        mapper->cpuWrite(addr, v);
    }
#ifdef NES_EMBEDDED
    // Bank switches can move the CHR window; re-cache it. Harmless while faulty
    // (refreshChrWindow yields null then), so it stays outside the branch.
    ppu.refreshChrWindow();
    // Likewise for PRG: this is the only path that can move a PRG bank, so
    // recomputing the four slots here is what lets the read side skip the check.
    refreshPrgWindows();
#endif
}

#ifdef NES_EMBEDDED
// Settle the PPU/APU debt accumulated while the CPU ran ahead.
void NES_HOT NES::catchUp() {
    const int cycles = pendingCpuCycles_;
    if (cycles <= 0) return;
    pendingCpuCycles_ = 0;
#ifdef NES_PROFILE
    const uint32_t profApuStart = xthal_get_ccount();
#endif
    apu.stepMany(cycles);
#ifdef NES_PROFILE
    const uint32_t profPpuStart = xthal_get_ccount();
    // Unsigned subtraction, so a CCOUNT wrap (every ~18s at 240MHz) still yields
    // the right delta as long as the batch itself is shorter than 2^32 cycles.
    profApuCycles += (uint32_t)(profPpuStart - profApuStart);
#endif
    // PPU: stepMany's dot-skipping proved unsafe (it desynchronises mid-frame
    // register timing on synth.nes), so the PPU is still stepped per dot here.
    // The win from batching is in the APU and in not re-entering runFrame's loop.
    ppu.stepMany(cycles * 3);
#ifdef NES_PROFILE
    profPpuCycles += (uint32_t)(xthal_get_ccount() - profPpuStart);
#endif
    // Only VRC-style carts drive anything off the CPU clock; for everything else
    // this loop was ~29,780 calls a frame into an empty virtual.
    if (mapperWantsCpuCycle_) {
        for (int i = 0; i < cycles; i++) mapper->cpuCycle();
    }
    cycleCount += cycles;
}

// Defer the work; flushed by catchUp() at a register access or a frame boundary.
void NES_HOT NES::runCyclesBatched(int cycles) {
    pendingCpuCycles_ += cycles;
    // Bound the debt so a long stretch without register access still renders in
    // order (and so the frame-end check below stays responsive).
    //
    // Kept at 64: raising it to 128 halves the flush count but measured no
    // faster on hardware (emuS 21.6ms either way), so the staler IRQ line it
    // buys is not worth it. The flush itself is not where the time goes.
    if (pendingCpuCycles_ >= 64) catchUp();
}
#endif

void NES_HOT NES::runFrame() {
    ppu.frameReady = false;
    while (!ppu.frameReady) {
        // IRQ line: APU frame/DMC + mapper (MMC3)
#ifndef NES_EMBEDDED
        cpu.irq(apu.irqPending() || (mapper && irqOk && mapper->irqPending()));
#else
        // The IRQ line reflects APU frame/DMC and mapper counters. Settling on
        // every instruction would defeat the batching, so the debt cap in
        // runCyclesBatched() bounds how stale this can get instead: an IRQ is
        // seen at most a few dozen CPU cycles late, which no NES title depends on.
        cpu.irq(irqLineLevel());
#endif
        int cycles = cpu.step();
#ifdef NES_EMBEDDED
        runCyclesBatched(cycles);
#else
        for (int i = 0; i < cycles; i++) {
            apu.step();
            ppu.step();
            ppu.step();
            ppu.step();
            cycleCount++;
            if (mapper) mapper->cpuCycle();
            if (probePin) probeSample();
        }
#endif
    }
#ifdef NES_EMBEDDED
    catchUp();   // do not carry debt across the frame boundary
#endif
}

#ifdef NES_EMBEDDED
uint8_t NES::debugPeek(uint16_t addr) const {
    if (addr < 0x2000) return ram[addr & 0x7FF];
    // The register window is reported as zero rather than read: touching $2002
    // clears vblank and $2007 advances the VRAM pointer, so a debugger that read
    // them would change the program it is watching.
    if (addr < 0x4020) return 0;
    return mapper ? mapper->cpuRead(addr) : 0;
}

size_t NES::buildDebugSnapshot(uint8_t* out, bool withWaves) const {
    size_t n = 0;

    // CPU registers, byte-for-byte the layout the browser already parses for the
    // local emulator, so the DEBUG panel needs no second decoder.
    out[n++] = cpu.pc & 0xFF;
    out[n++] = cpu.pc >> 8;
    out[n++] = cpu.a;
    out[n++] = cpu.x;
    out[n++] = cpu.y;
    out[n++] = cpu.sp;
    out[n++] = (cpu.fN << 7) | (cpu.fV << 6) | 0x20 | (cpu.fD << 3) |
               (cpu.fI << 2) | (cpu.fZ << 1) | (uint8_t)cpu.fC;
    out[n++] = 0;   // padding, matching nes_cpu_regs[7]
    const uint32_t f = ppu.frameCount;
    out[n++] = f & 0xFF;
    out[n++] = (f >> 8) & 0xFF;
    out[n++] = (f >> 16) & 0xFF;
    out[n++] = (f >> 24) & 0xFF;

    memcpy(out + n, apuRegShadow, sizeof(apuRegShadow));
    n += sizeof(apuRegShadow);

    // PC repeated explicitly: the code window is only meaningful relative to the
    // address it was taken from, and keeping them adjacent means the parser
    // cannot pair a window with the wrong PC.
    out[n++] = cpu.pc & 0xFF;
    out[n++] = cpu.pc >> 8;
    // 48 bytes covers the deepest the disassembler walks (12 instructions, max
    // 3 bytes each) without a second round trip.
    for (int i = 0; i < 48; i++) out[n++] = debugPeek((uint16_t)(cpu.pc + i));

    memcpy(out + n, ram, sizeof(ram));
    n += sizeof(ram);

    if (!withWaves) return n;

    // Decimate to the scope's pixel width using the same nearest-sample pick the
    // browser's drawWaves does, so the remote trace lines up with the local one
    // instead of being a differently-filtered view of the same audio.
    const int count = apu.sampleCount;
    for (int row = 0; row < DEBUG_WAVE_ROWS; row++) {
        for (int x = 0; x < DEBUG_WAVE_WIDTH; x++) {
            uint8_t v = 0;
            if (count > 0) {
                int i = (int)((int64_t)x * count / DEBUG_WAVE_WIDTH);
                if (i >= count) i = count - 1;
                if (row < 5) {
                    v = apu.chanBuf[row][i];
                } else {
                    // MIX carries a float; quantise through drawWaves' own
                    // min(1, mix*2) scaling so the row is directly comparable
                    // with the per-channel rows above it.
                    float m = apu.sampleBuf[i] * 2.0f;
                    if (m < 0.0f) m = 0.0f;
                    if (m > 1.0f) m = 1.0f;
                    v = (uint8_t)(m * 255.0f + 0.5f);
                }
            }
            out[n++] = v;
        }
    }

    return n;
}
#endif // NES_EMBEDDED

} // namespace nes

#ifndef NES_EMBEDDED
// Sample the probed pin's logic level once per CPU cycle.
// Digital levels use 30/220 so the trace reads like a real scope.
uint8_t nes::NES::probeLevelFor(int p) {
    auto dig = [](bool b) -> uint8_t { return b ? 220 : 30; };
    uint8_t v = 30;
    switch (p) {
    case 1: case 16: v = 30; break;                                 // GND
    case 30: case 31: v = 220; break;                               // +5V
    case 14: v = dig(!lastCpuWrite); break;                         // R/W (high = read)
    case 15: v = dig(!(apu.irqPending() || (mapper && irqOk && mapper->irqPending()))); break; // /IRQ
    case 32: v = dig(cycleCount & 1); break;                        // M2
    case 44: v = dig(!(lastCpuAddr >= 0x8000)); break;              // /ROMSEL
    case 45: case 46: {                                             // cart audio loop-through
        int s = soundOk ? (int)(30 + apu.mix() * 320.0f) : 30;
        v = (uint8_t)(s > 245 ? 245 : s);
        break;
    }
    case 17: v = dig(!ppuRdPulse); break;                           // PPU /RD
    case 47: v = dig(!ppuWrPulse); break;                           // PPU /WR
    case 18: v = dig(lastCiramA10); break;                          // CIRAM A10
    case 48: case 49: v = dig(!(lastPpuAddr & 0x2000)); break;      // CIRAM /CE, PPU /A13
    case 56: v = dig(lastPpuAddr & 0x2000); break;                  // PPU A13
    // internal test points (not on the connector)
    case 61: v = dig(!ppu.nmiLine()); break;                        // /NMI
    case 62: v = dig(!apu.irqPending()); break;                     // APU /IRQ
    case 63: v = dig(!(mapper && mapper->irqPending())); break;     // mapper /IRQ
    default:
        if (p >= 2 && p <= 13)       v = dig((lastCpuAddr >> (13 - p)) & 1);  // CPU A11..A0
        else if (p >= 33 && p <= 35) v = dig((lastCpuAddr >> (p - 21)) & 1);  // CPU A12..A14
        else if (p >= 36 && p <= 43) v = dig((lastCpuData >> (43 - p)) & 1);  // CPU D7..D0
        else if (p >= 19 && p <= 25) v = dig((lastPpuAddr >> (25 - p)) & 1);  // PPU A6..A0
        else if (p >= 50 && p <= 55) v = dig((lastPpuAddr >> (p - 43)) & 1);  // PPU A7..A12
        else if (p >= 26 && p <= 29) v = dig((lastPpuData >> (p - 26)) & 1);  // PPU D0..D3
        else if (p >= 57 && p <= 60) v = dig((lastPpuData >> (64 - p)) & 1);  // PPU D7..D4
        break;
    }
    return v;
}

void nes::NES::probeSample() {
    probeBuf[probePos] = probeLevelFor(probePin);
    probePos = (probePos + 1) & 2047;
    ppuRdPulse = ppuWrPulse = false;
}

// ================================================================ WASM C API
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define API EMSCRIPTEN_KEEPALIVE
#else
#define API
#endif

static nes::NES* g_nes = nullptr;
static uint8_t g_romBuf[4 * 1024 * 1024];

extern "C" {

API void nes_init(double sampleRate) {
    if (!g_nes) g_nes = new nes::NES();
    g_nes->apu.setSampleRate(sampleRate);
}

API uint8_t* nes_rom_buffer() { return g_romBuf; }

API int nes_load_rom(int size) {
    if (!g_nes || size <= 0 || (size_t)size > sizeof(g_romBuf)) return 0;
    return g_nes->loadRom(g_romBuf, (size_t)size) ? 1 : 0;
}

API void nes_reset() { if (g_nes && g_nes->mapper) g_nes->reset(); }
API void nes_power_on() { if (g_nes && g_nes->mapper) g_nes->powerOn(); }

// swap the cartridge WITHOUT any reset: CPU keeps running, RAM survives.
// Boot the new cart with the RESET button afterwards — bug techniques welcome.
API int nes_swap_rom(int size) {
    if (!g_nes || size <= 0 || (size_t)size > sizeof(g_romBuf)) return 0;
    auto m = nes::loadRom(g_romBuf, (size_t)size);
    if (!m) return 0;
    g_nes->mapper = std::move(m);
    g_nes->refreshMapperCaps();
    return 1;
}

static void muteIfCartAudioBroken() {
    // Famicom audio loops through the cartridge (pins 45/46) — a bad contact mutes it
    if (g_nes->soundOk) return;
    for (int i = 0; i < g_nes->apu.sampleCount; i++) {
        g_nes->apu.sampleBuf[i] = 0;
        g_nes->apu.sampleBufR[i] = 0;
    }
}

API void nes_frame() {
    if (!g_nes || !g_nes->mapper) return;
    g_nes->runFrame();
    muteIfCartAudioBroken();
}

API void nes_run_cycles(int n) {
    if (!g_nes || !g_nes->mapper || n <= 0) return;
    g_nes->runCycles(n);
    muteIfCartAudioBroken();
}

API void nes_set_pin(int pin, int on) {
    if (!g_nes || pin < 1 || pin > 60) return;
    g_nes->pinOk[pin] = on != 0;
    g_nes->updatePins();
}
API int nes_get_pin(int pin) {
    return (g_nes && pin >= 1 && pin <= 60) ? (g_nes->pinOk[pin] ? 1 : 0) : 1;
}
API void nes_reset_pins() {
    if (!g_nes) return;
    for (int i = 0; i < 61; i++) g_nes->pinOk[i] = true;
    g_nes->updatePins();
}

API uint32_t* nes_framebuffer() { return g_nes ? g_nes->ppu.framebuffer : nullptr; }

API void nes_set_buttons(int padIndex, int buttons) {
    if (g_nes && padIndex >= 0 && padIndex < 2)
        g_nes->pad[padIndex].setButtons((uint8_t)buttons);
}

API float* nes_audio_buffer() { return g_nes ? g_nes->apu.sampleBuf : nullptr; }
API float* nes_audio_buffer_r() { return g_nes ? g_nes->apu.sampleBufR : nullptr; }
API void nes_set_channel_volume(int ch, float v) {
    if (g_nes && ch >= 0 && ch < 8) g_nes->apu.chanVolume[ch] = v < 0 ? 0 : (v > 2 ? 2 : v);
}
API void nes_set_channel_pan(int ch, float p) {
    if (g_nes && ch >= 0 && ch < 8) g_nes->apu.chanPan[ch] = p < -1 ? -1 : (p > 1 ? 1 : p);
}
API int nes_audio_sample_count() { return g_nes ? g_nes->apu.sampleCount : 0; }
API void nes_audio_clear() { if (g_nes) g_nes->apu.sampleCount = 0; }

API uint8_t* nes_sram() {
    return (g_nes && g_nes->mapper) ? g_nes->mapper->prgRam().data() : nullptr;
}
API int nes_sram_size() {
    return (g_nes && g_nes->mapper) ? (int)g_nes->mapper->prgRam().size() : 0;
}
// CHR pattern tables rendered as a 128x256 RGBA image (table 0 on top, 1 below)
static uint32_t g_chrImage[128 * 256];

API uint32_t* nes_render_chr(int palIdx) {
    if (!g_nes || !g_nes->mapper) return nullptr;
    // colorize with the current PPU palette (palIdx 0-3: BG, 4-7: sprite)
    const uint8_t* pal = g_nes->ppu.paletteRam();
    palIdx &= 7;
    const uint32_t SHADES[4] = {
        nes::NES_PALETTE[pal[0] & 0x3F],
        nes::NES_PALETTE[pal[palIdx * 4 + 1] & 0x3F],
        nes::NES_PALETTE[pal[palIdx * 4 + 2] & 0x3F],
        nes::NES_PALETTE[pal[palIdx * 4 + 3] & 0x3F],
    };
    // read CHR through the (possibly faulty) connector, same as the PPU does
    auto chrRead = [&](uint16_t addr) -> uint8_t {
        if (!g_nes->powerOk || !g_nes->ppuRdOk) return addr & 0xFF;
        uint8_t v = g_nes->mapper->ppuRead(addr & g_nes->chrAddrAnd & 0x1FFF);
        return (v & g_nes->chrDataAnd) | ((addr & 0xFF) & ~g_nes->chrDataAnd);
    };
    for (int table = 0; table < 2; table++) {
        for (int tile = 0; tile < 256; tile++) {
            int baseX = (tile & 15) * 8;
            int baseY = table * 128 + (tile >> 4) * 8;
            uint16_t addr = table * 0x1000 + tile * 16;
            for (int y = 0; y < 8; y++) {
                uint8_t lo = chrRead(addr + y);
                uint8_t hi = chrRead(addr + y + 8);
                for (int x = 0; x < 8; x++) {
                    int px = ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1);
                    g_chrImage[(baseY + y) * 128 + baseX + x] = SHADES[px];
                }
            }
        }
    }
    return g_chrImage;
}

API uint8_t* nes_ram() { return g_nes ? g_nes->ram : nullptr; }
API uint8_t* nes_apu_regs() { return g_nes ? g_nes->apuRegShadow : nullptr; }

// side-effect-free memory read for the debugger (no PPU register touches,
// no bus-activity tracking, connector faults bypassed)
API int nes_peek(int addr) {
    if (!g_nes) return 0;
    uint16_t a = (uint16_t)addr;
    if (a < 0x2000) return g_nes->ram[a & 0x7FF];
    if (a < 0x4020) return 0;
    return g_nes->mapper ? g_nes->mapper->cpuRead(a) : 0;
}

static uint8_t g_cpuRegs[12];
API uint8_t* nes_cpu_regs() {
    if (!g_nes) return g_cpuRegs;
    const auto& c = g_nes->cpu;
    g_cpuRegs[0] = c.pc & 0xFF;
    g_cpuRegs[1] = c.pc >> 8;
    g_cpuRegs[2] = c.a;
    g_cpuRegs[3] = c.x;
    g_cpuRegs[4] = c.y;
    g_cpuRegs[5] = c.sp;
    g_cpuRegs[6] = (c.fN << 7) | (c.fV << 6) | 0x20 | (c.fD << 3) | (c.fI << 2) | (c.fZ << 1) | (uint8_t)c.fC;
    uint32_t f = g_nes->ppu.frameCount;
    g_cpuRegs[8] = f & 0xFF;
    g_cpuRegs[9] = (f >> 8) & 0xFF;
    g_cpuRegs[10] = (f >> 16) & 0xFF;
    g_cpuRegs[11] = (f >> 24) & 0xFF;
    return g_cpuRegs;
}

API void nes_set_probe(int pin) {
    if (g_nes && pin >= 0 && pin <= 63) g_nes->probePin = pin;
}
API uint8_t* nes_probe_buffer() { return g_nes ? g_nes->probeBuf : nullptr; }
API int nes_probe_level() {
    return g_nes ? g_nes->probeLevelFor(g_nes->probePin) : 0;
}
API int nes_probe_pos() { return g_nes ? g_nes->probePos : 0; }
API void nes_set_channel(int ch, int on) {
    if (!g_nes || ch < 0 || ch >= 8) return;
    g_nes->apu.chanEnable[ch] = on != 0;
    if (ch >= 5 && g_nes->mapper) g_nes->mapper->setExpansionMute(ch - 5, on == 0);
}
API int nes_has_expansion_audio() {
    return (g_nes && g_nes->mapper && g_nes->mapper->hasExpansionAudio()) ? 1 : 0;
}
API uint8_t* nes_chan_buffer(int ch) {
    return (g_nes && ch >= 0 && ch < 8) ? g_nes->apu.chanBuf[ch] : nullptr;
}

API int nes_has_battery() {
    return (g_nes && g_nes->mapper && g_nes->mapper->hasBattery()) ? 1 : 0;
}

} // extern "C"
#endif // !NES_EMBEDDED

#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include <new>

namespace nes {

#ifdef NES_EMBEDDED
using Pixel = uint16_t;   // RGB565 (LCD直転送用)
#else
using Pixel = uint32_t;   // ARGB
#endif

// IRAM_ATTR is only meaningful on ESP-IDF; keep it off the host builds so the
// same sources still compile with a plain clang/gcc.
#if defined(NES_EMBEDDED) && defined(ESP_PLATFORM)
#include "esp_attr.h"
#define NES_HOT IRAM_ATTR
// Xtensa uses the windowed ABI: every call executes entry/retw and periodically
// traps to spill a register window. PPU::step runs ~89k times per frame and
// calls three more levels down, so that overhead dominates the frame. Force the
// chain flat instead of trusting the inliner's size heuristics.
#define NES_INLINE __attribute__((always_inline)) inline
#else
#define NES_HOT
#define NES_INLINE inline
#endif

class NES;

// ROM/CHR storage. On ESP32 with PSRAM enabled the default allocator hands out
// PSRAM for allocations this size, and every opcode/pattern fetch then pays the
// external-bus latency. Force internal SRAM instead: a 48KB cartridge fits, and
// falling back to the default allocator keeps oversized ROMs loadable.
#if defined(NES_EMBEDDED) && defined(ESP_PLATFORM)
#include "esp_heap_caps.h"

template <typename T>
struct InternalRamAllocator {
    using value_type = T;
    InternalRamAllocator() = default;
    template <typename U> InternalRamAllocator(const InternalRamAllocator<U>&) {}

    T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        void* p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);   // PSRAM fallback
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) { heap_caps_free(p); }

    template <typename U> bool operator==(const InternalRamAllocator<U>&) const { return true; }
    template <typename U> bool operator!=(const InternalRamAllocator<U>&) const { return false; }
};

using RomBuffer = std::vector<uint8_t, InternalRamAllocator<uint8_t>>;
#else
using RomBuffer = std::vector<uint8_t>;
#endif

// ---------------------------------------------------------------- Cartridge
enum class Mirroring { Horizontal, Vertical, SingleLow, SingleHigh, FourScreen };

class Mapper {
public:
    Mapper(RomBuffer prg, RomBuffer chr, Mirroring m, bool battery)
        : prg_(std::move(prg)), chr_(std::move(chr)), mirroring_(m), battery_(battery) {
        chrRam_ = chr_.empty();
        if (chrRam_) chr_.resize(0x2000, 0);
        prgRam_.resize(0x2000, 0);
    }
    virtual ~Mapper() = default;

    virtual uint8_t cpuRead(uint16_t addr) = 0;
    virtual void cpuWrite(uint16_t addr, uint8_t v) = 0;
    virtual uint8_t ppuRead(uint16_t addr) = 0;   // $0000-$1FFF
    virtual void ppuWrite(uint16_t addr, uint8_t v) {
        if (chrRam_) chr_[addr & 0x1FFF] = v;
    }
    // Contiguous 8KB CHR window for $0000-$1FFF, or nullptr when the mapper
    // banks at finer granularity. The PPU reads patterns through this pointer to
    // skip a virtual call on every background/sprite fetch; mappers that switch
    // banks must keep it current (see Mapper3::cpuWrite).
    virtual const uint8_t* chrWindow() const { return nullptr; }

#ifdef NES_EMBEDDED
    // Direct pointers to the four 8KB PRG slots at $8000/$A000/$C000/$E000, the
    // CPU-side counterpart of chrWindow(). Fills win[] and returns true when the
    // mapper's $8000+ reads are a plain lookup through those pointers; false
    // leaves the caller on the virtual cpuRead() path.
    //
    // Four separate pointers rather than one base: every mapper here banks PRG at
    // 8KB or coarser, so a slot is always contiguous, but the slots need not be
    // adjacent in the ROM (MMC3 slot 1 and 3 come from unrelated banks) and a
    // 16KB cart mirrors two slots onto the same bytes.
    virtual bool prgWindows(const uint8_t* win[4]) const { (void)win; return false; }
#endif

    // Called once per scanline at PPU dot ~260 when rendering enabled (MMC3 IRQ)
    virtual void scanline() {}
    // Called once per CPU cycle (VRC-style IRQ counters, expansion audio)
    virtual void cpuCycle() {}
    // True only for mappers that actually override cpuCycle(). The catch-up path
    // would otherwise call an empty virtual ~29,780 times a frame, which on the
    // windowed-ABI Xtensa costs more than the counter it is standing in for.
    // Overridden (not derived from cpuCycle) because the base method is empty by
    // design for the majority of mappers.
    virtual bool wantsCpuCycle() const { return false; }
    // True for mappers with an IRQ source, so the per-instruction IRQ line sample
    // can skip the virtual call entirely on carts that can never assert it.
    virtual bool hasIrq() const { return false; }
    virtual bool irqPending() const { return false; }
    virtual void irqClear() {}

    // ---- cartridge expansion audio (VRC6 etc.) ----
    virtual bool hasExpansionAudio() const { return false; }
    virtual float audioOut() const { return 0.0f; }        // mixed into the APU output
    virtual float expansionGain() const { return 0.0f; }   // per-channel scale for the mixer
    virtual int expansionChannel(int) const { return 0; }  // raw level for the debug scopes
    void setExpansionMute(int ch, bool on) { if (ch >= 0 && ch < 3) expMute_[ch] = on; }

    Mirroring mirroring() const { return mirroring_; }
    bool hasBattery() const { return battery_; }
    RomBuffer& prgRam() { return prgRam_; }
    const uint8_t* prgData() const { return prg_.data(); }
    const uint8_t* chrData() const { return chr_.data(); }

protected:
#ifdef NES_EMBEDDED
    // Window derivation for the mappers whose $8000+ read is
    // prg_[(addr - 0x8000) % prg_.size()] — Mapper 0 and 3, i.e. no PRG banking.
    //
    // Requires an 8KB-aligned PRG so that a slot never straddles the wrap: at
    // 16KB the modulo mirrors $C000-$FFFF back onto the first half, which is
    // exactly slots 0 and 1 repeated, but at (say) 12KB the wrap would land
    // mid-slot and no single pointer could describe it. Non-multiples are not
    // real hardware, so they simply decline the fast path.
    bool fixedPrgWindows(const uint8_t* win[4]) const {
        const size_t size = prg_.size();
        const bool slotAligned = size >= 0x2000 && (size % 0x2000) == 0;
        if (!slotAligned) return false;
        for (int slot = 0; slot < 4; slot++) {
            win[slot] = prg_.data() + (((size_t)slot * 0x2000) % size);
        }
        return true;
    }
    // Window derivation from four already-resolved 8KB bank numbers, applying the
    // same bank % banks folding the cpuRead paths do.
    bool bankedPrgWindows(const uint8_t* win[4], const int bank8k[4]) const {
        const size_t size = prg_.size();
        const int banks = (int)(size / 0x2000);
        const bool slotAligned = banks > 0 && (size % 0x2000) == 0;
        if (!slotAligned) return false;
        for (int slot = 0; slot < 4; slot++) {
            win[slot] = prg_.data() + (size_t)(bank8k[slot] % banks) * 0x2000;
        }
        return true;
    }
#endif
    RomBuffer prg_, chr_, prgRam_;
    Mirroring mirroring_;
    bool battery_;
    bool chrRam_ = false;
    bool expMute_[3] = {false, false, false};   // UI mute for expansion channels
};

std::unique_ptr<Mapper> loadRom(const uint8_t* data, size_t size);

// ---------------------------------------------------------------- CPU (6502)
class CPU {
public:
    explicit CPU(NES& nes) : nes_(nes) {}
    void reset();
    int step();                 // execute one instruction, return cycles
    void nmi() { nmiPending_ = true; }
    void irq(bool level) { irqLine_ = level; }
    void addStall(int c) { stall_ += c; }

    uint16_t pc = 0;
    uint8_t a = 0, x = 0, y = 0, sp = 0xFD;
    // status flags
    bool fC = false, fZ = false, fI = true, fD = false, fV = false, fN = false;

private:
    NES& nes_;
    bool nmiPending_ = false;
    bool irqLine_ = false;
    int stall_ = 0;

    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);
    uint16_t read16(uint16_t addr);
    void push(uint8_t v);
    uint8_t pop();
    uint8_t status(bool brk) const;
    void setStatus(uint8_t p);
    void setZN(uint8_t v) { fZ = v == 0; fN = v & 0x80; }
    void branch(bool cond, int& cycles);

    // ---- opcode dispatch ----
    //
    // The single 16KB switch this replaces put every opcode's code in one
    // function body, so the Xtensa I-cache was thrashed by whichever opcodes the
    // ROM happened to use. Each opcode is now its own small function reached
    // through a 256-entry table, which keeps the working set to just the opcodes
    // actually executed and lets the hot ones be placed in IRAM individually.
    //
    // `crossed_` is the page-cross flag the addressing helpers set; it lived as a
    // local in the old switch and is now shared state between the addressing and
    // execute halves of one opcode. It is written and consumed within a single
    // dispatch, never across instructions.
    using OpFn = int (CPU::*)();
    static const OpFn OPS[256];
    bool crossed_ = false;

    // addressing modes (identical semantics to the old lambdas)
    uint16_t amImm() { return pc++; }
    uint16_t amZp()  { return read(pc++); }
    uint16_t amZpx() { return (read(pc++) + x) & 0xFF; }
    uint16_t amZpy() { return (read(pc++) + y) & 0xFF; }
    uint16_t amAbs() { uint16_t a = read16(pc); pc += 2; return a; }
    uint16_t amAbx() { uint16_t b = read16(pc); pc += 2; uint16_t a = b + x; crossed_ = (a & 0xFF00) != (b & 0xFF00); return a; }
    uint16_t amAby() { uint16_t b = read16(pc); pc += 2; uint16_t a = b + y; crossed_ = (a & 0xFF00) != (b & 0xFF00); return a; }
    uint16_t amIzx() { uint8_t z = read(pc++) + x; return read(z) | (read((uint8_t)(z + 1)) << 8); }
    uint16_t amIzy() {
        uint8_t z = read(pc++);
        uint16_t b = read(z) | (read((uint8_t)(z + 1)) << 8);
        uint16_t a = b + y; crossed_ = (a & 0xFF00) != (b & 0xFF00); return a;
    }

    // operations (identical semantics to the old lambdas)
    void opLda(uint16_t ad) { a = read(ad); setZN(a); }
    void opLdx(uint16_t ad) { x = read(ad); setZN(x); }
    void opLdy(uint16_t ad) { y = read(ad); setZN(y); }
    void opAdcv(uint8_t m) {
        int r = a + m + (fC ? 1 : 0);
        fV = (~(a ^ m) & (a ^ r)) & 0x80;
        fC = r > 0xFF;
        a = (uint8_t)r; setZN(a);
    }
    void opCmpv(uint8_t reg, uint8_t m) { fC = reg >= m; setZN(reg - m); }
    uint8_t opAslv(uint8_t m) { fC = m & 0x80; m <<= 1; setZN(m); return m; }
    uint8_t opLsrv(uint8_t m) { fC = m & 0x01; m >>= 1; setZN(m); return m; }
    uint8_t opRolv(uint8_t m) { bool c = fC; fC = m & 0x80; m = (m << 1) | c; setZN(m); return m; }
    uint8_t opRorv(uint8_t m) { bool c = fC; fC = m & 0x01; m = (m >> 1) | (c << 7); setZN(m); return m; }

    // Every opcode handler. Named opXX after its opcode byte so the table below
    // can be read against a 6502 opcode matrix.
#define NES_CPU_OP(hex) int op##hex();
#include "cpu_ops.inc"
#undef NES_CPU_OP
};

// ---------------------------------------------------------------- PPU
class PPU {
public:
    explicit PPU(NES& nes) : nes_(nes) {}
    void reset();
    void step();                // one PPU cycle (dot)
#ifdef NES_EMBEDDED
    void stepMany(int dots);    // advance N dots, skipping the ones with no work
#endif

    uint8_t readReg(uint16_t addr);       // $2000-$2007
    void writeReg(uint16_t addr, uint8_t v);
    void writeOamDma(uint8_t v, const uint8_t* page);

    bool frameReady = false;    // set at end of each frame; consumer clears
    uint32_t frameCount = 0;    // frames since reset/power-on
#ifdef NES_EMBEDDED
    // When false the framebuffer is left untouched for this frame while every
    // CPU-observable side effect (vblank/NMI, sprite 0 hit, sprite overflow,
    // mapper IRQ, loopy v/t) still happens. Lets the frontend run emulation at
    // real-time 60Hz and refresh the panel less often — and, because the DMA
    // engine reads the framebuffer in place, lets a transfer overlap the frames
    // that do not write to it.
    bool renderThisFrame = true;
#endif
    Pixel framebuffer[256 * 240] = {};
    const uint8_t* paletteRam() const { return palette_; }
#ifdef NES_EMBEDDED
    // Read-only views of everything the CPU can observe, for the host-side test
    // that checks a render-skipped run stays identical to a fully drawn one.
    // Const accessors only, so they cannot perturb what they measure.
    struct DbgState {
        uint8_t ctrl, mask, status, oamAddr, readBuffer, openBus;
        uint16_t v, t; uint8_t fineX; bool w;
        int scanline, dot; bool oddFrame; uint32_t frameCount;
    };
    DbgState dbgState() const {
        return {ctrl_, mask_, status_, oamAddr_, readBuffer_, openBus_,
                v_, t_, fineX_, w_, scanline_, dot_, oddFrame_, frameCount};
    }
    const uint8_t* dbgOam() const { return oam_; }
    const uint8_t* dbgVram() const { return vram_; }
#endif
    // level of the PPU→CPU NMI output (true = asserted)
    bool nmiLine() const { return (ctrl_ & 0x80) && (status_ & 0x80); }

private:
    NES& nes_;

    // registers
    uint8_t ctrl_ = 0, mask_ = 0, status_ = 0, oamAddr_ = 0;
    uint16_t v_ = 0, t_ = 0;    // loopy
    uint8_t fineX_ = 0;
    bool w_ = false;
    uint8_t readBuffer_ = 0;
    uint8_t openBus_ = 0;

    uint8_t oam_[256] = {};
    uint8_t palette_[32] = {};
    uint8_t vram_[0x800] = {};  // 2KB nametable RAM

    int scanline_ = 261, dot_ = 0;
    bool oddFrame_ = false;

#ifdef NES_EMBEDDED
    // Cached mapper->chrWindow(); refreshed whenever the cart may have switched
    // banks, so pattern fetches avoid a virtual call. Null = use the virtual.
    const uint8_t* chrWindow_ = nullptr;
#else
    static constexpr const uint8_t* chrWindow_ = nullptr;
#endif
public:
    // No-op off the embedded build; defined in ppu.cpp (NES is incomplete here).
    void refreshChrWindow();
private:

    // background shifters
    uint16_t bgPatLo_ = 0, bgPatHi_ = 0, bgAttrLo_ = 0, bgAttrHi_ = 0;
    uint8_t ntByte_ = 0, atByte_ = 0, patLo_ = 0, patHi_ = 0;

    // sprite evaluation for current scanline
    struct Sprite { uint8_t patLo, patHi, attr; int x; bool sprite0; };
    Sprite sprites_[8];
    int spriteCount_ = 0;

#ifdef NES_EMBEDDED
    // OAM indices whose Y byte can put them on a visible line, in OAM order.
    //
    // evalSprites() scans all 64 entries once per scanline, but a typical frame
    // parks most of them off-screen (Y >= 240) where they fail the row test on
    // every one of the 240 lines. This is that scan's constant part, hoisted out.
    //
    // Order is preserved, so the front-to-back priority, the 8-sprite limit and
    // the overflow flag all fall where they did: dropping an entry that can never
    // pass the row test cannot change which of the others do, nor their sequence.
    //
    // Rebuilt lazily rather than on each write: OAM DMA rewrites all 256 bytes one
    // byte at a time, so invalidating is a flag store and the cost is paid once at
    // the next scanline instead of 256 times during the transfer.
    uint8_t spriteCandidates_[64] = {};
    int spriteCandidateCount_ = 0;
    bool spriteCandidatesValid_ = false;
    void rebuildSpriteCandidates();
    // Any OAM byte write can move an entry across the parked boundary.
    //
    // Why not also invalidate on a sprite-height change ($2000 bit 5): the list is
    // built against the largest height, so it is a superset for the 8-tall case
    // too. Keeping an entry that the current height can never reach is free — the
    // row test in evalSprites() rejects it exactly as it always did — whereas
    // making the list height-dependent would put a rebuild on a register write
    // that games toggle mid-frame.
    void invalidateSpriteCandidates() { spriteCandidatesValid_ = false; }
#endif

    uint8_t vramRead(uint16_t addr);
    void vramWrite(uint16_t addr, uint8_t v);
    // Connector-fault variants, shared by both builds (see ppu.cpp).
    uint8_t vramReadFaulty(uint16_t addr);
    void vramWriteFaulty(uint16_t addr, uint8_t v);
    uint16_t ntMirror(uint16_t addr);
    void incHoriz();
    void incVert();
    void fetchBg();
    void evalSprites(int line);
    void renderDot();
#ifdef NES_EMBEDDED
    // Batched replacement for the per-dot pipeline. Draw=false keeps the
    // CPU-visible side effects (sprite 0 hit) but writes no pixels; templated
    // rather than branched on a member so the drawing path keeps the same
    // straight-line code it had before the skip mode existed.
    template <bool Draw> void renderScanline();
    bool hasSprite0() const {
        for (int i = 0; i < spriteCount_; i++) if (sprites_[i].sprite0) return true;
        return false;
    }
#endif
    bool renderingEnabled() const { return mask_ & 0x18; }
};

// ---------------------------------------------------------------- APU
class APU {
public:
    explicit APU(NES& nes) : nes_(nes) {}
    void reset();
    void step();                // one CPU cycle
#ifdef NES_EMBEDDED
    void stepMany(int cycles);  // advance N CPU cycles, batched to event bounds
#endif
    uint8_t readStatus();
    void writeReg(uint16_t addr, uint8_t v);
    bool irqPending() const { return frameIrq_ || dmcIrq_; }

    // audio output: float samples accumulated per frame (stereo)
    float sampleBuf[2048] = {};      // left
#ifndef NES_EMBEDDED
    float sampleBufR[2048] = {};     // right
#endif
    int sampleCount = 0;
    // per-channel raw levels at each sample point (debug scope): p1,p2,tri,noise,dmc
    //
    // Kept on the embedded build too so the browser's scope can show the device.
    // 16KB of internal SRAM is the price; the per-sample cost is avoided instead
    // by writing only while waveCapture is set, so a device nobody is watching
    // pays one predictable branch per sample and nothing else.
    uint8_t chanBuf[8][2048] = {};
#ifdef NES_EMBEDDED
    // Enabled by the frontend for as long as a debugger is actually asking for
    // waveforms, then dropped again. Public because the frame loop owns the
    // policy (how long to stay armed), not the APU.
    bool waveCapture = false;
#endif
    // per-channel mute switches (UI): p1,p2,tri,noise,dmc,(expansion x3)
    bool chanEnable[8] = {true, true, true, true, true, true, true, true};
    // mixer: per-channel gain (0..2, 1 = unity) and pan (-1 left .. +1 right)
    float chanVolume[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    float chanPan[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // ESP32-S3 has a single-precision FPU only: double would drop the per-CPU-cycle
    // downsample accumulator into soft-float, so store it as float there.
#ifdef NES_EMBEDDED
    using SampleTimer = float;
#else
    using SampleTimer = double;
#endif
    void setSampleRate(double rate) { cyclesPerSample_ = (SampleTimer)(1789773.0 / rate); }
#ifdef NES_EMBEDDED
    // Representative CPU-observable APU state for the render-skip equivalence
    // test: the $4015 inputs plus the frame sequencer position.
    struct DbgState {
        int p1len, p2len, triLen, noiseLen, dmcBytes;
        int frameStep, frameCycles; bool frameIrq, dmcIrq, oddCycle;
        int sampleCount;
    };
    DbgState dbgState() const {
        return {pulse1_.lengthCounter, pulse2_.lengthCounter, triangle_.lengthCounter,
                noise_.lengthCounter, dmc_.bytesRemaining, frameStep_, frameCounterCycles_,
                frameIrq_, dmcIrq_, oddCycle_, sampleCount};
    }
#endif
    float mix() const;   // mono sum; also used by the oscilloscope probe
    void mixStereo(float& l, float& r) const;
    void channelOutputs(float out[8]) const;

private:
    NES& nes_;

    struct Pulse {
        bool enabled = false;
        uint8_t duty = 0; int dutyPos = 0;
        uint16_t timer = 0; int timerCounter = 0;
        int lengthCounter = 0; bool lengthHalt = false;
        // envelope
        bool constVolume = false; uint8_t volume = 0;
        bool envStart = false; int envDivider = 0; int envDecay = 0;
        // sweep
        bool sweepEnabled = false, sweepNegate = false, sweepReload = false;
        uint8_t sweepPeriod = 0, sweepShift = 0; int sweepDivider = 0;
        bool isPulse2 = false;
        int output() const;
        void stepTimer();
#ifdef NES_EMBEDDED
        void advance(int n);   // batched equivalent of n stepTimer() calls
#endif
        void stepEnvelope();
        void stepSweep();
        bool sweepMuted() const;
    } pulse1_, pulse2_;

    struct Triangle {
        bool enabled = false;
        uint16_t timer = 0; int timerCounter = 0;
        int lengthCounter = 0; bool lengthHalt = false;
        int linearCounter = 0; uint8_t linearReload = 0; bool linearReloadFlag = false;
        int seqPos = 0;
        int output() const;
        void stepTimer();
#ifdef NES_EMBEDDED
        void advance(int n);   // batched equivalent of n stepTimer() calls
#endif
    } triangle_;

    struct Noise {
        bool enabled = false;
        bool mode = false;
        uint16_t shiftReg = 1;
        int timerPeriod = 0; int timerCounter = 0;
        int lengthCounter = 0; bool lengthHalt = false;
        bool constVolume = false; uint8_t volume = 0;
        bool envStart = false; int envDivider = 0; int envDecay = 0;
        int output() const;
        void stepTimer();
#ifdef NES_EMBEDDED
        void advance(int n);   // batched equivalent of n stepTimer() calls
#endif
        void stepEnvelope();
    } noise_;

    struct DMC {
        bool enabled = false;
        bool irqEnable = false, loop = false;
        int timerPeriod = 0; int timerCounter = 0;
        uint8_t outputLevel = 0;
        uint16_t sampleAddr = 0, currentAddr = 0;
        int sampleLength = 0, bytesRemaining = 0;
        uint8_t shiftReg = 0; int bitsRemaining = 0;
        bool bufferFilled = false; uint8_t buffer = 0;
        bool silence = true;
    } dmc_;

    int frameStep_ = 0;
    int frameCounterCycles_ = 0;
    bool fiveStep_ = false;
    bool irqInhibit_ = false;
    bool frameIrq_ = false;
    bool dmcIrq_ = false;
    bool oddCycle_ = false;

    SampleTimer cyclesPerSample_ = (SampleTimer)(1789773.0 / 44100.0);
    SampleTimer sampleTimer_ = 0;

    void quarterFrame();
    void halfFrame();
    void stepDmc();
#ifdef NES_EMBEDDED
    // Stage totals only, for the mix() path that discards the per-channel split.
    // Returns false when the mixer tables are unusable (muted/re-gained channel),
    // leaving the caller on channelOutputs().
    bool mixDirect(float& sum) const;
#endif
#ifdef NES_EMBEDDED
    // Mixer division tables. channelOutputs() ran five-plus float divides per
    // output sample and the APU measured 5.6ms/frame on hardware; every divide
    // whose input is one of the channels' small integer levels is precomputed
    // instead.
    //
    // Only the divides are tabulated — not the stage totals, and not the
    // proportional splits. Folding the splits away (mix() sums shares that add
    // back to the stage total, so pulseTable_[psum] + tnd would be algebraically
    // identical) was measured to differ from the current path by 1 ULP on 34% of
    // inputs, because splitting and re-summing rounds differently than the single
    // add. The core is bit-exact-verified against the web build, so the sum keeps
    // its exact shape and only its inputs come from tables — every downstream
    // operation then sees identical values and the result is bit-identical.
    // The tables themselves live in apu.cpp's anonymous namespace: they are
    // read only from channelOutputs() and nothing else needs the symbols.
    //
    // They are indexed by raw integer level, so they are only valid while
    // no channel is muted or re-gained. The embedded frontend never touches
    // either array, but the UDP debug protocol could grow to, so the slow path
    // stays reachable rather than silently producing wrong audio.
    bool mixerTablesUsable() const {
        for (int c = 0; c < 8; c++) {
            const bool plain = chanEnable[c] && chanVolume[c] == 1.0f;
            if (!plain) return false;
        }
        return true;
    }
#endif
#ifdef NES_EMBEDDED
    // Record one scope sample. Both downsample sites call this so the two paths
    // cannot drift apart; the branch is what keeps it free when nobody is
    // watching. NES_INLINE because it sits in the per-sample path.
    NES_INLINE void captureChannels(int at) {
        if (!waveCapture) return;
        chanBuf[0][at] = (uint8_t)pulse1_.output();
        chanBuf[1][at] = (uint8_t)pulse2_.output();
        chanBuf[2][at] = (uint8_t)triangle_.output();
        chanBuf[3][at] = (uint8_t)noise_.output();
        chanBuf[4][at] = dmc_.outputLevel;
    }
#endif
};

// ---------------------------------------------------------------- Controller
class Controller {
public:
    void setButtons(uint8_t b) { buttons_ = b; }
    void writeStrobe(uint8_t v) {
        strobe_ = v & 1;
        if (strobe_) shift_ = buttons_;
    }
    uint8_t read() {
        if (strobe_) return (buttons_ & 1) | 0x40;
        uint8_t r = (shift_ & 1) | 0x40;
        shift_ = (shift_ >> 1) | 0x80;
        return r;
    }
private:
    uint8_t buttons_ = 0, shift_ = 0;
    bool strobe_ = false;
};

// ---------------------------------------------------------------- NES
class NES {
public:
    NES() : cpu(*this), ppu(*this), apu(*this) {
        for (int i = 0; i < 61; i++) pinOk[i] = true;
    }

    bool loadRom(const uint8_t* data, size_t size);
    void reset();      // RESET button: chips reset, RAM preserved (bug techniques!)
    void powerOn();    // power cycle: RAM cleared + reset
    void runFrame();
    void runCycles(int n);   // sub-frame stepping for very low clock rates

    uint8_t cpuRead(uint16_t addr);
    void cpuWrite(uint16_t addr, uint8_t v);

    CPU cpu;
    PPU ppu;
    APU apu;
    Controller pad[2];
    std::unique_ptr<Mapper> mapper;
    uint8_t ram[0x800] = {};
    // ---- cartridge connector fault emulation (60-pin, 1-based) ----
    //
    // Shared by both builds: the embedded frontend drives these from the browser's
    // connector UI over UDP. Everything here is cold (touched only when a pin
    // changes), so it costs the hot paths nothing but the pinsFaulty_ test.
    bool pinOk[61];
    // derived signal masks/flags, recomputed by updatePins()
    uint16_t prgAddrAnd = 0x7FFF;   // CPU A0-A14 to cart
    uint8_t  prgDataAnd = 0xFF;     // CPU D0-D7
    uint16_t chrAddrAnd = 0x3FFF;   // PPU A0-A13 to cart
    uint8_t  chrDataAnd = 0xFF;     // PPU D0-D7
    bool romselOk = true, m2Ok = true, rwOk = true, irqOk = true;
    bool ppuRdOk = true, ppuWrOk = true, ciramCeOk = true, ciramA10Ok = true;
    bool soundOk = true, powerOk = true;
    // True when any of pins 1-60 is open. The single gate every hot path tests to
    // decide whether it can take the fast direct-to-mapper route.
    bool pinsFaulty_ = false;
    void updatePins();
    uint8_t cartOpenBus(uint16_t addr) const { return addr >> 8; }
    // bit(n-1) set = pin n making contact. The one entry point for external pin
    // sources (browser UI today, an on-device IMU later), so callers never have
    // to know about updatePins()/refreshChrWindow() ordering.
    void applyPinMask(uint64_t mask);

    // Cartridge-connector fault paths, shared by both builds so the embedded and
    // reference cores mask identically. Out of line and never called while the
    // connector is clean.
    uint8_t cartReadFaulty(uint16_t addr);
    void cartWriteFaulty(uint16_t addr, uint8_t v);

    // Last value written to $4000-$4017 (debug view). Shared by both builds so
    // the device can report it to the browser's DEBUG panel: 24 bytes and one
    // store per APU register write, which is not measurable against the work the
    // write itself does.
    uint8_t apuRegShadow[0x18] = {};

#ifdef NES_EMBEDDED
    // Pack a debug snapshot for the remote monitor. Layout:
    //   [12B CPU regs (same order as the WASM nes_cpu_regs)]
    //   [24B apuRegShadow]
    //   [2B PC][48B code window at PC][2048B work RAM]
    // Returns the byte count written. Side-effect free: it must be safe to call
    // between frames without perturbing what it is reporting.
    // Waveforms are decimated to the width the scope actually draws (280px), so
    // the wire carries what is displayed rather than a frame of samples the
    // browser would immediately throw away. 6 rows: P1,P2,TRI,NOI,DMC,MIX.
    static constexpr int DEBUG_WAVE_WIDTH = 280;
    static constexpr int DEBUG_WAVE_ROWS = 6;
    static constexpr size_t DEBUG_WAVE_SIZE = DEBUG_WAVE_WIDTH * DEBUG_WAVE_ROWS;
    static constexpr size_t DEBUG_SNAPSHOT_SIZE = 12 + 0x18 + 2 + 48 + 0x800;
    static constexpr size_t DEBUG_SNAPSHOT_MAX = DEBUG_SNAPSHOT_SIZE + DEBUG_WAVE_SIZE;
    // withWaves appends the decimated scope rows after the WRAM block.
    size_t buildDebugSnapshot(uint8_t* out, bool withWaves = false) const;
    // Side-effect-free read for the snapshot: no PPU/APU register touches, no
    // bus-activity tracking. $2000-$401F reads back as 0 rather than going near
    // the real registers, which would clear vblank and change what we measure.
    uint8_t debugPeek(uint16_t addr) const;
#endif

#ifndef NES_EMBEDDED
    // ---- oscilloscope probe (hover a pin in the UI) ----
    int probePin = 0;               // 1-60, 0 = no probe
    uint8_t probeBuf[2048] = {};    // one sample per CPU cycle (~1.1ms window)
    int probePos = 0;
    uint16_t lastCpuAddr = 0; uint8_t lastCpuData = 0; bool lastCpuWrite = false;
    uint16_t lastPpuAddr = 0; uint8_t lastPpuData = 0;
    bool ppuRdPulse = false, ppuWrPulse = false, lastCiramA10 = false;
    void probeSample();
    uint8_t probeLevelFor(int pin);
#endif // !NES_EMBEDDED

    uint64_t cycleCount = 0;
    uint8_t cpuReadBus(uint16_t addr);

#ifdef NES_PROFILE
    // CPU cycles spent inside catchUp()'s APU and PPU batches, so the frontend
    // can split emulation time three ways (whatever is left of the measured frame
    // is the CPU core plus the mapper). Owned by the frontend: it reads and
    // resets them, nothing here ever clears them.
    uint64_t profApuCycles = 0;
    uint64_t profPpuCycles = 0;
#endif

    // Cached mapper capabilities, refreshed on load. Both guard per-cycle and
    // per-instruction virtual calls that are pure overhead on the many carts that
    // have neither an IRQ source nor a cycle-driven counter.
    bool mapperWantsCpuCycle_ = false;
    bool mapperHasIrq_ = false;
    // mapperHasIrq_ AND the /IRQ pin actually being connected. Folding the pin
    // state in here (rather than testing irqOk on the hot path) is what makes a
    // broken /IRQ line free: the per-instruction sample reads one bool either way.
    // Kept current by both refreshMapperCaps() and updatePins().
    bool mapperIrqUsable_ = false;
    void refreshMapperCaps() {
        mapperWantsCpuCycle_ = mapper && mapper->wantsCpuCycle();
        mapperHasIrq_ = mapper && mapper->hasIrq();
        mapperIrqUsable_ = mapperHasIrq_ && irqOk;
    }
    // The IRQ line as the CPU sees it. Inlined and short-circuited so the common
    // no-IRQ-mapper case costs two predictable branches instead of a virtual call.
    bool irqLineLevel() {
        return apu.irqPending() || (mapperIrqUsable_ && mapper->irqPending());
    }

#ifdef NES_EMBEDDED
    // Cached PRG bank pointers for $8000-$FFFF, the CPU-side analogue of the
    // PPU's chrWindow_. Instruction fetch alone reads this range tens of
    // thousands of times a frame, and each one was a virtual cpuRead() that then
    // recomputed its bank arithmetic — on Mapper 3 a modulo per byte fetched.
    //
    // Refreshed rather than computed on demand: bank state only changes on a
    // write to the cartridge, which is rare enough that recomputing all four
    // slots there is free compared to testing for staleness on every read.
    const uint8_t* prgWin_[4] = {nullptr, nullptr, nullptr, nullptr};
    bool prgFastValid_ = false;
    void refreshPrgWindows() {
        // Dropped while the connector is faulty for the same reason
        // refreshChrWindow() drops its pointer: the window bypasses the address
        // and data masking, so a broken PRG line would go unnoticed.
        const bool canUseWindows = mapper && !pinsFaulty_;
        prgFastValid_ = canUseWindows && mapper->prgWindows(prgWin_);
    }

    // Catch-up scheduling.
    //
    // Stepping the PPU three times and the APU once inside the CPU's cycle loop
    // costs more in call overhead than the work itself. Instead the CPU runs
    // ahead and the debt is settled in bulk: at scanline granularity for the
    // PPU, at sample granularity for the APU. Anything that can observe their
    // state mid-instruction (a $2002/$2004/$2007 access, a $4015 read, an APU
    // register write) calls catchUp() first, so the observable behaviour is the
    // same as stepping in lockstep.
    int pendingCpuCycles_ = 0;
    void catchUp();
    void runCyclesBatched(int cycles);
#endif
};

extern const uint32_t NES_PALETTE[64];

// APU length counter table (shared)
extern const uint8_t LENGTH_TABLE[32];

} // namespace nes

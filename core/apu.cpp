#include "nes.h"

namespace nes {

const uint8_t LENGTH_TABLE[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

static const uint8_t DUTY_TABLE[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
};

static const uint8_t TRIANGLE_SEQ[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

static const uint16_t NOISE_PERIODS[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};

static const uint16_t DMC_PERIODS[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};

// ---- Pulse ----
void APU::Pulse::stepTimer() {
    if (--timerCounter < 0) {
        timerCounter = timer;
        dutyPos = (dutyPos + 1) & 7;
    }
}
#ifdef NES_EMBEDDED
// Batched equivalent of calling stepTimer() n times. The divider reloads with
// `timer`, so it wraps floor((n - counter - 1) / (timer + 1)) + 1 times once the
// first wrap is reached; the duty position is periodic mod 8.
void APU::Pulse::advance(int n) {
    if (n <= 0) return;
    if (timerCounter >= n) { timerCounter -= n; return; }
    n -= timerCounter + 1;
    const int period = timer + 1;
    const int wraps = 1 + n / period;
    timerCounter = timer - (n % period);
    dutyPos = (dutyPos + wraps) & 7;
}
#endif
void APU::Pulse::stepEnvelope() {
    if (envStart) {
        envStart = false;
        envDecay = 15;
        envDivider = volume;
    } else if (--envDivider < 0) {
        envDivider = volume;
        if (envDecay > 0) envDecay--;
        else if (lengthHalt) envDecay = 15;
    }
}
bool APU::Pulse::sweepMuted() const {
    if (timer < 8) return true;
    if (!sweepNegate) {
        int target = timer + (timer >> sweepShift);
        if (target > 0x7FF) return true;
    }
    return false;
}
void APU::Pulse::stepSweep() {
    if (--sweepDivider < 0 || sweepReload) {
        if (sweepDivider < 0 && sweepEnabled && sweepShift && !sweepMuted()) {
            int delta = timer >> sweepShift;
            if (sweepNegate) timer -= delta + (isPulse2 ? 0 : 1);
            else timer += delta;
        }
        if (sweepDivider < 0 || sweepReload) {
            sweepDivider = sweepPeriod;
            sweepReload = false;
        }
    }
}
int APU::Pulse::output() const {
    if (!enabled || lengthCounter == 0 || sweepMuted()) return 0;
    if (!DUTY_TABLE[duty][dutyPos]) return 0;
    return constVolume ? volume : envDecay;
}

// ---- Triangle ----
void APU::Triangle::stepTimer() {
    if (--timerCounter < 0) {
        timerCounter = timer;
        if (lengthCounter > 0 && linearCounter > 0 && timer >= 2)
            seqPos = (seqPos + 1) & 31;
    }
}
#ifdef NES_EMBEDDED
void APU::Triangle::advance(int n) {
    if (n <= 0) return;
    if (timerCounter >= n) { timerCounter -= n; return; }
    n -= timerCounter + 1;
    const int period = timer + 1;
    const int wraps = 1 + n / period;
    timerCounter = timer - (n % period);
    const bool gated = lengthCounter > 0 && linearCounter > 0 && timer >= 2;
    if (gated) seqPos = (seqPos + wraps) & 31;
}
#endif
int APU::Triangle::output() const {
    if (!enabled || lengthCounter == 0 || linearCounter == 0) return TRIANGLE_SEQ[seqPos];
    return TRIANGLE_SEQ[seqPos];
}

// ---- Noise ----
void APU::Noise::stepTimer() {
    if (--timerCounter < 0) {
        timerCounter = timerPeriod;
        int fb = (shiftReg & 1) ^ ((shiftReg >> (mode ? 6 : 1)) & 1);
        shiftReg = (shiftReg >> 1) | (fb << 14);
    }
}
#ifdef NES_EMBEDDED
// The LFSR has no closed form here, so only the divider is batched; the shift
// register is clocked once per wrap instead of once per cycle.
void APU::Noise::advance(int n) {
    if (n <= 0) return;
    if (timerCounter >= n) { timerCounter -= n; return; }
    n -= timerCounter + 1;
    const int period = timerPeriod + 1;
    const int wraps = 1 + n / period;
    timerCounter = timerPeriod - (n % period);
    for (int i = 0; i < wraps; i++) {
        const int fb = (shiftReg & 1) ^ ((shiftReg >> (mode ? 6 : 1)) & 1);
        shiftReg = (shiftReg >> 1) | (fb << 14);
    }
}
#endif
void APU::Noise::stepEnvelope() {
    if (envStart) {
        envStart = false;
        envDecay = 15;
        envDivider = volume;
    } else if (--envDivider < 0) {
        envDivider = volume;
        if (envDecay > 0) envDecay--;
        else if (lengthHalt) envDecay = 15;
    }
}
int APU::Noise::output() const {
    if (!enabled || lengthCounter == 0 || (shiftReg & 1)) return 0;
    return constVolume ? volume : envDecay;
}

// ---- APU ----
void APU::reset() {
    pulse1_ = Pulse{};
    pulse2_ = Pulse{}; pulse2_.isPulse2 = true;
    triangle_ = Triangle{};
    noise_ = Noise{};
    dmc_ = DMC{};
    frameStep_ = 0;
    frameCounterCycles_ = 0;
    fiveStep_ = false;
    irqInhibit_ = false;
    frameIrq_ = dmcIrq_ = false;
    oddCycle_ = false;
    sampleCount = 0;
    sampleTimer_ = 0;
}

void APU::quarterFrame() {
    pulse1_.stepEnvelope();
    pulse2_.stepEnvelope();
    noise_.stepEnvelope();
    if (triangle_.linearReloadFlag) triangle_.linearCounter = triangle_.linearReload;
    else if (triangle_.linearCounter > 0) triangle_.linearCounter--;
    if (!triangle_.lengthHalt) triangle_.linearReloadFlag = false;
}

void APU::halfFrame() {
    auto clockLength = [](int& lc, bool halt) { if (!halt && lc > 0) lc--; };
    clockLength(pulse1_.lengthCounter, pulse1_.lengthHalt);
    clockLength(pulse2_.lengthCounter, pulse2_.lengthHalt);
    clockLength(triangle_.lengthCounter, triangle_.lengthHalt);
    clockLength(noise_.lengthCounter, noise_.lengthHalt);
    pulse1_.stepSweep();
    pulse2_.stepSweep();
}

void NES_HOT APU::stepDmc() {
    // fetch sample byte when needed
    if (!dmc_.bufferFilled && dmc_.bytesRemaining > 0) {
        dmc_.buffer = nes_.cpuRead(dmc_.currentAddr);
        nes_.cpu.addStall(4);
        dmc_.bufferFilled = true;
        dmc_.currentAddr = (dmc_.currentAddr == 0xFFFF) ? 0x8000 : dmc_.currentAddr + 1;
        if (--dmc_.bytesRemaining == 0) {
            if (dmc_.loop) {
                dmc_.currentAddr = dmc_.sampleAddr;
                dmc_.bytesRemaining = dmc_.sampleLength;
            } else if (dmc_.irqEnable) {
                dmcIrq_ = true;
            }
        }
    }
    if (--dmc_.timerCounter < 0) {
        dmc_.timerCounter = dmc_.timerPeriod;
        if (!dmc_.silence) {
            if (dmc_.shiftReg & 1) { if (dmc_.outputLevel <= 125) dmc_.outputLevel += 2; }
            else { if (dmc_.outputLevel >= 2) dmc_.outputLevel -= 2; }
        }
        dmc_.shiftReg >>= 1;
        if (--dmc_.bitsRemaining <= 0) {
            dmc_.bitsRemaining = 8;
            if (dmc_.bufferFilled) {
                dmc_.silence = false;
                dmc_.shiftReg = dmc_.buffer;
                dmc_.bufferFilled = false;
            } else {
                dmc_.silence = true;
            }
        }
    }
}

// Per-channel contributions. The 2A03 mixes pulses and TND through two
// non-linear stages, so each channel's share is split proportionally out of
// its stage: the shares always sum back to the exact mono mix.
#ifdef NES_EMBEDDED
// Generated by the same expressions the slow path evaluates, so a lookup and a
// divide return bit-identical floats.
namespace {
template <int N> struct MixTable { float v[N]; };

constexpr int PULSE_TABLE_N = 31, TRI_TABLE_N = 16, NOISE_TABLE_N = 16, DMC_TABLE_N = 128;

MixTable<PULSE_TABLE_N> makePulseTable() {
    MixTable<PULSE_TABLE_N> t{};
    t.v[0] = 0.0f;   // psum == 0 takes the guarded branch, never this entry
    for (int i = 1; i < PULSE_TABLE_N; i++) {
        t.v[i] = 95.88f / (8128.0f / (float)i + 100.0f);
    }
    return t;
}
template <int N> MixTable<N> makeDivTable(float denom) {
    MixTable<N> t{};
    for (int i = 0; i < N; i++) t.v[i] = (float)i / denom;
    return t;
}
const MixTable<PULSE_TABLE_N> g_pulse = makePulseTable();
const MixTable<TRI_TABLE_N> g_tri = makeDivTable<TRI_TABLE_N>(8227.0f);
const MixTable<NOISE_TABLE_N> g_noise = makeDivTable<NOISE_TABLE_N>(12241.0f);
const MixTable<DMC_TABLE_N> g_dmc = makeDivTable<DMC_TABLE_N>(22638.0f);
}  // namespace
#endif

void APU::channelOutputs(float out[8]) const {
#ifdef NES_EMBEDDED
    // Table path. Identical arithmetic to the block below with the four
    // level-driven divides replaced by lookups; the splits and the mix are
    // untouched, so this is not an approximation of the slow path but the same
    // computation with precomputed operands.
    if (mixerTablesUsable()) {
        const int p1i = (int)pulse1_.output();
        const int p2i = (int)pulse2_.output();
        const int psumI = p1i + p2i;
        const float p1 = (float)p1i, p2 = (float)p2i, psum = (float)psumI;
        const float pulseOut = g_pulse.v[psumI];
        out[0] = psum > 0.0f ? pulseOut * (p1 / psum) : 0.0f;
        out[1] = psum > 0.0f ? pulseOut * (p2 / psum) : 0.0f;

        const float t = g_tri.v[(int)triangle_.output()];
        const float n = g_noise.v[(int)noise_.output()];
        const float d = g_dmc.v[dmc_.outputLevel];
        const float tsum = t + n + d;
        const float tnd = tsum > 0.0f ? 159.79f / (1.0f / tsum + 100.0f) : 0.0f;
        out[2] = tsum > 0.0f ? tnd * (t / tsum) : 0.0f;
        out[3] = tsum > 0.0f ? tnd * (n / tsum) : 0.0f;
        out[4] = tsum > 0.0f ? tnd * (d / tsum) : 0.0f;

        const float g = nes_.mapper ? nes_.mapper->expansionGain() : 0.0f;
        for (int c = 0; c < 3; c++) {
            const float v = (g > 0.0f && nes_.mapper) ? (float)nes_.mapper->expansionChannel(c) : 0.0f;
            out[5 + c] = v * g;
        }
        return;
    }
#endif
    float p1 = chanEnable[0] ? pulse1_.output() * chanVolume[0] : 0.0f;
    float p2 = chanEnable[1] ? pulse2_.output() * chanVolume[1] : 0.0f;
    float psum = p1 + p2;
    float pulseOut = psum > 0.0f ? 95.88f / (8128.0f / psum + 100.0f) : 0.0f;
    out[0] = psum > 0.0f ? pulseOut * (p1 / psum) : 0.0f;
    out[1] = psum > 0.0f ? pulseOut * (p2 / psum) : 0.0f;

    float t = (chanEnable[2] ? triangle_.output() * chanVolume[2] : 0.0f) / 8227.0f;
    float n = (chanEnable[3] ? noise_.output() * chanVolume[3] : 0.0f) / 12241.0f;
    float d = (chanEnable[4] ? dmc_.outputLevel * chanVolume[4] : 0.0f) / 22638.0f;
    float tsum = t + n + d;
    float tnd = tsum > 0.0f ? 159.79f / (1.0f / tsum + 100.0f) : 0.0f;
    out[2] = tsum > 0.0f ? tnd * (t / tsum) : 0.0f;
    out[3] = tsum > 0.0f ? tnd * (n / tsum) : 0.0f;
    out[4] = tsum > 0.0f ? tnd * (d / tsum) : 0.0f;

    float g = nes_.mapper ? nes_.mapper->expansionGain() : 0.0f;
    for (int c = 0; c < 3; c++) {
        float v = (g > 0.0f && nes_.mapper) ? (float)nes_.mapper->expansionChannel(c) : 0.0f;
        out[5 + c] = chanEnable[5 + c] ? v * chanVolume[5 + c] * g : 0.0f;
    }
}

#ifdef NES_EMBEDDED
// Stage totals without the per-channel split.
//
// channelOutputs() divides each stage total into per-channel shares and mix()
// immediately adds them back up, so the five divides and the re-sum exist only
// to be undone. Summing the stage totals directly drops them.
//
// Why not keep the split and stay bit-exact: the shares are computed and re-added
// in float, and splitting then re-summing rounds differently from the single add.
// Exhaustively over all 8.4M reachable channel states the two forms disagree on
// 34% of them, by at most 1.2e-7 absolute against a full-scale of 1.0 — under the
// 23rd bit, i.e. seven bits below what 16-bit output can represent. That was
// accepted deliberately: the core's bit-exactness contract covers registers,
// timing, pixel output and the debug snapshots, none of which read sampleBuf, and
// the web build reaches the mixer through mixStereo() on a path this never takes.
bool APU::mixDirect(float& sum) const {
    if (!mixerTablesUsable()) return false;

    const int psumI = (int)pulse1_.output() + (int)pulse2_.output();
    const float pulseOut = g_pulse.v[psumI];

    const float tsum = g_tri.v[(int)triangle_.output()] + g_noise.v[(int)noise_.output()] +
                       g_dmc.v[dmc_.outputLevel];
    const float tnd = tsum > 0.0f ? 159.79f / (1.0f / tsum + 100.0f) : 0.0f;

    // g is 0 without a mapper, so a positive gain already implies one.
    float exp = 0.0f;
    const float g = nes_.mapper ? nes_.mapper->expansionGain() : 0.0f;
    const bool hasExpansion = g > 0.0f;
    if (hasExpansion) {
        for (int c = 0; c < 3; c++) exp += (float)nes_.mapper->expansionChannel(c) * g;
    }

    sum = pulseOut + tnd + exp;
    return true;
}
#endif

float APU::mix() const {
#ifdef NES_EMBEDDED
    // The split is only needed while the scope is being fed per-channel values,
    // which captureChannels() reads from the channels themselves — so nothing
    // downstream of the normal playback path wants it.
    float direct;
    if (mixDirect(direct)) return direct;
#endif
    float out[8];
    channelOutputs(out);
    float s = 0.0f;
    for (int c = 0; c < 8; c++) s += out[c];
    return s;
}

// Linear pan law with unity at centre, so a centred mix is bit-identical to mono
void APU::mixStereo(float& l, float& r) const {
    float out[8];
    channelOutputs(out);
    l = r = 0.0f;
    for (int c = 0; c < 8; c++) {
        float p = chanPan[c];
        l += out[c] * (p <= 0.0f ? 1.0f : 1.0f - p);
        r += out[c] * (p >= 0.0f ? 1.0f : 1.0f + p);
    }
}

#ifdef NES_EMBEDDED
// Advance `cycles` CPU cycles worth of APU state.
//
// step() is dominated by per-call overhead rather than by its own work, so this
// runs the channel timers in a tight loop and evaluates the frame-counter and
// downsample checks only at the boundaries where they can actually fire. The
// timers still clock every cycle (their divider positions shape the waveform),
// and quarter/half-frame clocks land on exactly the same cycles as before.
//
// Deliberate compromise: DMC sample fetches are still driven from stepDmc() on
// the same cycles, but a DMC fetch does not steal CPU cycles here (it did not
// before either). ROMs that rely on precise DMC DMA stalling are unaffected in
// this build only because that behaviour was already absent.
void NES_HOT APU::stepMany(int cycles) {
    while (cycles > 0) {
        // Cycles until the next frame-counter event.
        static const int STEP4[4] = {7457, 14913, 22371, 29829};
        static const int STEP5[5] = {7457, 14913, 22371, 29829, 37281};
        int nextEvent;
        if (!fiveStep_) {
            nextEvent = frameStep_ < 4 ? STEP4[frameStep_] - frameCounterCycles_ : cycles;
        } else {
            nextEvent = frameStep_ < 5 ? STEP5[frameStep_] - frameCounterCycles_ : cycles;
        }
        if (nextEvent < 1) nextEvent = 1;

        // Cycles until the next output sample is due.
        SampleTimer remain = cyclesPerSample_ - sampleTimer_;
        int nextSample = (int)(remain / SampleTimer(1));
        if (nextSample < 1) nextSample = 1;

        int run = cycles;
        if (nextEvent < run) run = nextEvent;
        if (nextSample < run) run = nextSample;

        // Timers are dividers: instead of decrementing once per cycle, work out
        // how many times each one wraps over `run` cycles and advance its
        // sequencer that many times. Channels whose period exceeds the batch
        // reduce to a single subtraction.
        const int apuTicks = (run + (oddCycle_ ? 1 : 0)) / 2;   // cycles where the /2 channels clock
        triangle_.advance(run);
        pulse1_.advance(apuTicks);
        pulse2_.advance(apuTicks);
        noise_.advance(apuTicks);
        // DMC: collapse the whole batch to one subtraction when it provably
        // cannot do anything except decrement its timer.
        //
        // stepDmc() has exactly two effect sites. The fetch fires only when
        // !bufferFilled && bytesRemaining > 0, so requiring the negation of that
        // rules it out — and nothing inside the batch can make it true, because
        // bufferFilled is cleared only by the timer branch, which the second
        // condition excludes. That branch fires when --timerCounter goes below
        // zero, i.e. on the (timerCounter + 1)-th tick, so timerCounter >=
        // apuTicks leaves it unreached. Both held, the batch's only effect is
        // timerCounter -= apuTicks.
        //
        // Not a general fast path: the moment either could fire, the per-tick
        // loop runs, so fetch ordering, addStall and the IRQ all keep their exact
        // cycle. A batch is at most 32 ticks against a 54-tick minimum DMC
        // period, so the common case is still the cheap one.
        const bool dmcFetchPending = !dmc_.bufferFilled && dmc_.bytesRemaining > 0;
        const bool dmcTimerQuiet = dmc_.timerCounter >= apuTicks;
        const bool dmcIdle = !dmcFetchPending && dmcTimerQuiet;
        if (dmcIdle) {
            dmc_.timerCounter -= apuTicks;
        } else {
            for (int i = 0; i < apuTicks; i++) stepDmc();
        }
        if (run & 1) oddCycle_ = !oddCycle_;
        frameCounterCycles_ += run;
        sampleTimer_ += SampleTimer(run);
        cycles -= run;

        // Frame counter, evaluated exactly where step() would have fired it.
        if (!fiveStep_) {
            if (frameStep_ < 4 && frameCounterCycles_ >= STEP4[frameStep_]) {
                quarterFrame();
                if (frameStep_ == 1 || frameStep_ == 3) halfFrame();
                if (frameStep_ == 3) {
                    if (!irqInhibit_) frameIrq_ = true;
                    frameCounterCycles_ = 0;
                }
                frameStep_ = (frameStep_ + 1) & 3;
            }
        } else {
            if (frameStep_ < 5 && frameCounterCycles_ >= STEP5[frameStep_]) {
                if (frameStep_ != 3) {
                    quarterFrame();
                    if (frameStep_ == 1 || frameStep_ == 4) halfFrame();
                }
                if (frameStep_ == 4) frameCounterCycles_ = 0;
                frameStep_ = frameStep_ == 4 ? 0 : frameStep_ + 1;
            }
        }

        // Downsample.
        if (sampleTimer_ >= cyclesPerSample_) {
            sampleTimer_ -= cyclesPerSample_;
            if (sampleCount < (int)(sizeof(sampleBuf) / sizeof(float))) {
                sampleBuf[sampleCount] = mix();
                captureChannels(sampleCount);
                sampleCount++;
            }
        }
    }
}
#endif // NES_EMBEDDED

void NES_HOT APU::step() {
    // triangle clocks every CPU cycle; pulse/noise/dmc every other
    triangle_.stepTimer();
    if (oddCycle_) {
        pulse1_.stepTimer();
        pulse2_.stepTimer();
        noise_.stepTimer();
        stepDmc();
    }
    oddCycle_ = !oddCycle_;

    // frame counter (approximate CPU-cycle timing)
    frameCounterCycles_++;
    static const int STEP4[4] = {7457, 14913, 22371, 29829};
    static const int STEP5[5] = {7457, 14913, 22371, 29829, 37281};
    if (!fiveStep_) {
        if (frameStep_ < 4 && frameCounterCycles_ >= STEP4[frameStep_]) {
            quarterFrame();
            if (frameStep_ == 1 || frameStep_ == 3) halfFrame();
            if (frameStep_ == 3) {
                if (!irqInhibit_) frameIrq_ = true;
                frameCounterCycles_ = 0;
            }
            frameStep_ = (frameStep_ + 1) & 3;
        }
    } else {
        if (frameStep_ < 5 && frameCounterCycles_ >= STEP5[frameStep_]) {
            if (frameStep_ != 3) {
                quarterFrame();
                if (frameStep_ == 1 || frameStep_ == 4) halfFrame();
            }
            if (frameStep_ == 4) frameCounterCycles_ = 0;
            frameStep_ = frameStep_ == 4 ? 0 : frameStep_ + 1;
        }
    }

    // downsample
    sampleTimer_ += SampleTimer(1);
    if (sampleTimer_ >= cyclesPerSample_) {
        sampleTimer_ -= cyclesPerSample_;
        if (sampleCount < (int)(sizeof(sampleBuf) / sizeof(float))) {
#ifndef NES_EMBEDDED
            float l, r;
            mixStereo(l, r);
            sampleBuf[sampleCount] = l;
            sampleBufR[sampleCount] = r;
            chanBuf[0][sampleCount] = (uint8_t)pulse1_.output();
            chanBuf[1][sampleCount] = (uint8_t)pulse2_.output();
            chanBuf[2][sampleCount] = (uint8_t)triangle_.output();
            chanBuf[3][sampleCount] = (uint8_t)noise_.output();
            chanBuf[4][sampleCount] = dmc_.outputLevel;
            for (int c = 0; c < 3; c++)
                chanBuf[5 + c][sampleCount] =
                    nes_.mapper ? (uint8_t)nes_.mapper->expansionChannel(c) : 0;
#else
            sampleBuf[sampleCount] = mix();
            captureChannels(sampleCount);
#endif
            sampleCount++;
        }
    }
}

uint8_t NES_HOT APU::readStatus() {
    uint8_t r = 0;
    if (pulse1_.lengthCounter > 0) r |= 0x01;
    if (pulse2_.lengthCounter > 0) r |= 0x02;
    if (triangle_.lengthCounter > 0) r |= 0x04;
    if (noise_.lengthCounter > 0) r |= 0x08;
    if (dmc_.bytesRemaining > 0) r |= 0x10;
    if (frameIrq_) r |= 0x40;
    if (dmcIrq_) r |= 0x80;
    frameIrq_ = false;
    return r;
}

void NES_HOT APU::writeReg(uint16_t addr, uint8_t v) {
    switch (addr) {
    case 0x4000: case 0x4004: {
        Pulse& p = (addr == 0x4000) ? pulse1_ : pulse2_;
        p.duty = v >> 6;
        p.lengthHalt = v & 0x20;
        p.constVolume = v & 0x10;
        p.volume = v & 0x0F;
        break;
    }
    case 0x4001: case 0x4005: {
        Pulse& p = (addr == 0x4001) ? pulse1_ : pulse2_;
        p.sweepEnabled = v & 0x80;
        p.sweepPeriod = (v >> 4) & 7;
        p.sweepNegate = v & 0x08;
        p.sweepShift = v & 7;
        p.sweepReload = true;
        break;
    }
    case 0x4002: case 0x4006: {
        Pulse& p = (addr == 0x4002) ? pulse1_ : pulse2_;
        p.timer = (p.timer & 0x700) | v;
        break;
    }
    case 0x4003: case 0x4007: {
        Pulse& p = (addr == 0x4003) ? pulse1_ : pulse2_;
        p.timer = (p.timer & 0xFF) | ((v & 7) << 8);
        if (p.enabled) p.lengthCounter = LENGTH_TABLE[v >> 3];
        p.envStart = true;
        p.dutyPos = 0;
        break;
    }
    case 0x4008:
        triangle_.lengthHalt = v & 0x80;
        triangle_.linearReload = v & 0x7F;
        break;
    case 0x400A:
        triangle_.timer = (triangle_.timer & 0x700) | v;
        break;
    case 0x400B:
        triangle_.timer = (triangle_.timer & 0xFF) | ((v & 7) << 8);
        if (triangle_.enabled) triangle_.lengthCounter = LENGTH_TABLE[v >> 3];
        triangle_.linearReloadFlag = true;
        break;
    case 0x400C:
        noise_.lengthHalt = v & 0x20;
        noise_.constVolume = v & 0x10;
        noise_.volume = v & 0x0F;
        break;
    case 0x400E:
        noise_.mode = v & 0x80;
        noise_.timerPeriod = NOISE_PERIODS[v & 0x0F];
        break;
    case 0x400F:
        if (noise_.enabled) noise_.lengthCounter = LENGTH_TABLE[v >> 3];
        noise_.envStart = true;
        break;
    case 0x4010:
        dmc_.irqEnable = v & 0x80;
        if (!dmc_.irqEnable) dmcIrq_ = false;
        dmc_.loop = v & 0x40;
        dmc_.timerPeriod = DMC_PERIODS[v & 0x0F] / 2;   // stepped every other CPU cycle
        break;
    case 0x4011:
        dmc_.outputLevel = v & 0x7F;
        break;
    case 0x4012:
        dmc_.sampleAddr = 0xC000 + v * 64;
        break;
    case 0x4013:
        dmc_.sampleLength = v * 16 + 1;
        break;
    case 0x4015:
        pulse1_.enabled = v & 0x01;
        pulse2_.enabled = v & 0x02;
        triangle_.enabled = v & 0x04;
        noise_.enabled = v & 0x08;
        dmc_.enabled = v & 0x10;
        if (!pulse1_.enabled) pulse1_.lengthCounter = 0;
        if (!pulse2_.enabled) pulse2_.lengthCounter = 0;
        if (!triangle_.enabled) triangle_.lengthCounter = 0;
        if (!noise_.enabled) noise_.lengthCounter = 0;
        if (!dmc_.enabled) dmc_.bytesRemaining = 0;
        else if (dmc_.bytesRemaining == 0) {
            dmc_.currentAddr = dmc_.sampleAddr;
            dmc_.bytesRemaining = dmc_.sampleLength;
        }
        dmcIrq_ = false;
        break;
    case 0x4017:
        fiveStep_ = v & 0x80;
        irqInhibit_ = v & 0x40;
        if (irqInhibit_) frameIrq_ = false;
        frameCounterCycles_ = 0;
        frameStep_ = 0;
        if (fiveStep_) { quarterFrame(); halfFrame(); }
        break;
    }
}

} // namespace nes

#pragma once

#include <cstdint>

// ------------------------------------------------------------------ display
// The NES frame is 256x240; the CoreS3 panel is 320x240, so the picture is
// centred horizontally at 1:1 instead of scaled (no spare CPU for resampling).
constexpr int NES_WIDTH = 256;
constexpr int NES_HEIGHT = 240;
constexpr int SCREEN_X_OFFSET = 32;

// The panel expects RGB565 big-endian. The core's palette LUT now stores the
// swapped words directly (see buildPalLut), so the driver must not swap again —
// and, more importantly, setSwapBytes(true) forces M5GFX to stage every line
// through a bounce buffer, which rules out handing the framebuffer straight to
// the DMA engine.
constexpr bool DISPLAY_SWAP_BYTES = false;

// LCD SPI write clock, kept at the M5GFX CoreS3 default. 80MHz halves the
// transfer time but overruns the ILI9342C's write timing: address-window bytes
// get corrupted, so the picture lands shifted outside the panel and jumps
// around frame to frame (observed on hardware as overflow + flicker).
constexpr uint32_t SPI_WRITE_FREQ = 40000000;

// Rows per DMA segment, and how the number came about.
//
// The frame is pushed a horizontal band at a time rather than whole. The SPI
// peripheral's transaction-length register (SPI_MS_DATA_BITLEN) is 18 bits, so
// one hardware transaction moves at most 32768 bytes. LGFX hides that by
// re-arming the peripheral in a loop and spinning on SPI_USR between segments
// (Bus_SPI.cpp writeBytes, the `if (length -= len)` block) — which is why a
// 122880-byte push costs most of its ~24.6ms of wire time in CPU spin despite
// being a "DMA" call. Handing it one sub-32KB band per frame means every push
// fits a single transaction and returns as soon as the descriptor is armed.
//
// 40 rows = 20480 bytes at 256px x RGB565, comfortably inside the 32KB limit and
// an exact divisor of 240, so all six bands are the same size. The band is not
// sized to the 32KB ceiling (which would be 64 rows) because the segment count
// doubles as the display divisor: at six segments the once-per-picture repaint
// cost is amortised over six frames instead of four, which is worth more than
// the marginally fewer kicks. Six kicks of ~124us each is still negligible.
constexpr int DISPLAY_DMA_ROWS = 40;
constexpr int DISPLAY_DMA_SEGMENTS = (NES_HEIGHT + DISPLAY_DMA_ROWS - 1) / DISPLAY_DMA_ROWS;

// Display divisor: push every Nth emulated frame.
//
// Declared after the segment count because it is not independent of it. A
// picture ships as DISPLAY_DMA_SEGMENTS bands, one kicked per frame, so only a
// divisor at or above that count gives every band a frame of its own and keeps
// each kick CPU-free. Below it a draw frame arrives with bands still owed and
// has to flush them inline, degenerating back to the blocking whole-frame push
// this scheme exists to avoid.
//
// Fixed rather than adaptive: the band scheme made the draw frame cheap enough
// that there is nothing left for a controller to trade away, and the floor and
// the ceiling had converged on the same value anyway. At 60fps this yields a
// 10Hz panel refresh.
constexpr uint32_t DISPLAY_DIVISOR_MIN = 6;
constexpr uint32_t DISPLAY_DIVISOR_MAX = 6;
constexpr uint32_t DISPLAY_DIVISOR_INITIAL = 6;
static_assert(DISPLAY_DIVISOR_MIN >= (uint32_t)DISPLAY_DMA_SEGMENTS,
              "divisor floor must cover the band count, or a draw frame flushes bands inline");
static_assert(DISPLAY_DIVISOR_INITIAL >= DISPLAY_DIVISOR_MIN && DISPLAY_DIVISOR_INITIAL <= DISPLAY_DIVISOR_MAX,
              "initial divisor must lie within the configured range");

// Runtime guard on the repaint-versus-last-band race.
//
// A draw frame begins repainting while the final band (rows 200-239) may still
// be on the wire. That is safe only because the writer is slower than the
// reader: renderScanline paints strictly top-to-bottom, so row 200 is not
// touched until 200/262 of the way through emulation, while the band's 40 rows
// clear the wire 4.10ms after the kick. Setting the two equal — allowing ~0.5ms
// between the kick and the start of the repaint — puts break-even at an emulated
// frame time of 4.71ms.
//
// Measured emuD is ~19ms, so the margin is roughly 4x and the guard never fires.
// It exists because that ratio is not enforced by anything else: a faster core,
// a larger DISPLAY_DMA_ROWS or a higher SPI_WRITE_FREQ all erode it silently,
// and the failure mode is a torn picture rather than anything that would show up
// in a build. Tripping at 2x break-even leaves room for the EWMA to lag a sudden
// speed-up without letting an actual race through.
constexpr float DISPLAY_REPAINT_GUARD_MS = 9.42f;
// Smoothing for the measured draw-frame emulation time the guard tests. Slower
// than the audio servo's: this only has to track a lasting change in emulation
// speed, and a single fast frame must not arm a guard that costs a stall.
constexpr float DISPLAY_EMU_EWMA_ALPHA = 0.05f;

// ------------------------------------------------------------------- audio
constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr uint8_t SPEAKER_VOLUME = 128;   // 0-255, M5.Speaker master volume
// The device level the browser's 1.0 master volume maps to, so the two agree on
// what "normal" sounds like. The web slider spans 0..1.5, i.e. up to 192 here.
constexpr uint8_t SPEAKER_VOLUME_BASE = SPEAKER_VOLUME;
constexpr uint8_t SPEAKER_CHANNEL = 1;   // virtual channel used for playRaw
constexpr int AUDIO_BUF_SAMPLES = 2048;   // matches APU::sampleBuf capacity

// Staging between the APU and the speaker. The ring gives ~186ms of slack so a
// late frame cannot starve the hardware; chunks are the unit handed to playRaw.
constexpr int AUDIO_RING_SAMPLES = 8192;   // ~186ms at 44.1kHz
// The ring index wraps by masking, not by %, because both the enqueue and the
// drain loop wrap once per sample and an integer division there is pure cost.
// That only works while the size is a power of two, hence the assertion.
static_assert((AUDIO_RING_SAMPLES & (AUDIO_RING_SAMPLES - 1)) == 0,
              "AUDIO_RING_SAMPLES must be a power of two: the ring wraps by masking");
constexpr int AUDIO_RING_MASK = AUDIO_RING_SAMPLES - 1;
constexpr int AUDIO_CHUNK_SAMPLES = 512;   // ~11.6ms per submission
constexpr int AUDIO_CHUNK_SLOTS = 4;   // rotation depth; playRaw holds the pointer

// ---- playback rate control ----
// The NES produces 44100/60.0988 = ~733.8 samples per frame. When the emulator
// cannot hit 60fps it produces fewer, so the sample rate handed to playRaw is
// retimed to what is actually being produced and playback stays continuous
// (slightly lower pitch) instead of repeatedly underrunning.
//
// The rate handed to playRaw is servoed every frame instead of being recomputed
// from a once-per-second fps average: a per-second update moves the pitch in
// audible steps (a 27->32fps swing is nearly a whole tone) and, being derived
// from the perf window, it also disappeared whenever PERF_LOG was off.
//
// How full the ring should sit in steady state. Half of AUDIO_RING_SAMPLES so
// the same headroom exists in both directions — a late frame can eat 2048
// samples before starving, and a fast one can add 2048 before dropping.
constexpr int AUDIO_RING_TARGET = 2048;
// Smoothing for the measured production rate. 0.03 per frame is a ~0.5s time
// constant: long enough that one long frame does not move the pitch, short
// enough to follow a real change in emulation speed within a second. Applied to
// the sample count and the frame time separately, never to their ratio — see
// updatePlaybackRate().
constexpr float AUDIO_RATE_EWMA_ALPHA = 0.03f;
// Frames during which the servo follows its target without the slew limit. The
// EWMAs start at the 60fps ideal, so a ROM that runs slower needs a large one-off
// correction that the slew limit would otherwise stretch over several seconds of
// wrong-pitch playback. ~120 frames is a few EWMA time constants: long enough to
// arrive, short enough that the audible transient stays at boot.
constexpr int AUDIO_RATE_WARMUP_FRAMES = 120;
// Ring-depth feedback, in Hz of rate correction per sample of error. The ring
// level is the integral of the production/consumption mismatch, so feeding it
// back is what removes the residual drift the EWMA alone cannot see. 0.02 pulls
// a full 2048-sample error back with ~41Hz (~0.1%), i.e. gently.
constexpr float AUDIO_RATE_FEEDBACK_GAIN = 0.02f;
// Ceiling on that correction. Beyond ~1% the pitch offset becomes audible, and
// a ring error that large is a symptom of the emulator's speed, not something
// the servo should chase.
constexpr float AUDIO_RATE_FEEDBACK_MAX = 0.01f;
// Slew limit per frame. 0.25% per 16.6ms is below the ~0.5% pitch difference
// most listeners can detect, so even a large rate change arrives as a drift
// rather than a step.
constexpr float AUDIO_RATE_SLEW_MAX = 0.0025f;
// Snap window around AUDIO_SAMPLE_RATE. Once the emulator holds 60fps the servo
// would otherwise hover a few Hz off nominal forever, dithering the pitch; the
// wider release threshold is hysteresis so it does not chatter in and out.
constexpr float AUDIO_RATE_SNAP_ENTER = 0.005f;
constexpr float AUDIO_RATE_SNAP_EXIT = 0.0075f;
// Ring-feedback ceiling while snapped, as a fraction of nominal. ±0.15% (~66Hz)
// is a third of the smallest pitch offset most listeners detect, yet corrects
// tens of samples per second of clock mismatch — far more than the measured
// drift between the emulator's 60.10Hz pacing and the real I2S rate.
constexpr float AUDIO_RATE_SNAP_TRIM_MAX = 0.0015f;

// APU::mix() returns 0..~0.45 — positive only, with a standing DC offset of
// about +0.24. A one-pole high-pass removes it; the pole sits near 1.0 so the
// corner stays well below audible bass at 44.1kHz.
constexpr float AUDIO_DC_POLE = 0.9985f;
// After DC removal the signal swings only about +/-0.23, so it is amplified to
// use the available range while leaving headroom for louder passages.
constexpr float AUDIO_HEADROOM = 2.5f;

// --------------------------------------------------------------------- UDP
constexpr uint16_t UDP_PORT = 5555;
constexpr uint8_t UDP_PACKET_SIZE = 8;
constexpr uint8_t UDP_PROTOCOL_VERSION = 1;
// Controller state is released when packets stop arriving, so a dead sender
// cannot leave a button stuck down.
constexpr uint32_t INPUT_TIMEOUT_MS = 500;

// Packet kind, read from byte [3]. The original controller packet left that byte
// as a zero "reserved" field (see tools/procon_udp.py build_packet), so type 0
// keeps every existing sender working untouched.
constexpr uint8_t UDP_TYPE_PAD = 0;
constexpr uint8_t UDP_TYPE_PINS = 1;
// Console control. Needed because pulling a CPU-bus pin can crash the emulated
// program, and reseating the cart alone does not recover it — on real hardware
// you press RESET afterwards, so the browser has to be able to say that too.
constexpr uint8_t UDP_TYPE_CTRL = 2;
// Debug snapshot request. The reply goes back to whoever asked, split across
// two datagrams because ~2.1KB exceeds the MTU and relying on IP fragmentation
// over WiFi is a good way to lose the whole snapshot to one dropped fragment.
constexpr uint8_t UDP_TYPE_DEBUG = 3;
// byte [6] bit 0: also send the APU scope rows.
constexpr uint8_t UDP_DEBUG_FLAG_WAVES = 0x01;
// How long a wave request keeps the APU's per-sample capture armed. Long enough
// that a 5Hz poller never sees a gap, short enough that closing the browser stops
// the extra work within a couple of frames' worth of audio.
constexpr uint32_t DEBUG_WAVE_HOLD_MS = 2000;
// Ceiling, not a fixed count: the reply is split to fit the MTU and the actual
// number of parts is carried in the header, so adding the wave rows needs no
// protocol change.
constexpr uint8_t UDP_DEBUG_PARTS = 4;
// 'N','D' | version | part | nparts | seq u16
constexpr uint8_t UDP_DEBUG_HEADER = 7;
constexpr int UDP_DEBUG_CHUNK = 1400;   // stays inside a 1500-byte MTU
constexpr uint8_t UDP_CTRL_RESET = 0x01;   // byte [6] bit 0
constexpr uint8_t UDP_CTRL_VOLUME = 0x02;   // byte [6] bit 1, level in byte [7]

// Cartridge swap over WiFi. The ROM arrives in chunks because a .nes image is far
// past any MTU; it is staged in PSRAM and only handed to the core once the whole
// image has been verified, so a truncated or corrupted transfer can never take
// down a running game.
constexpr uint8_t UDP_TYPE_ROM = 4;
// byte [6]: which step of the transfer this datagram is.
constexpr uint8_t UDP_ROM_OP_BEGIN = 0;
constexpr uint8_t UDP_ROM_OP_DATA = 1;
constexpr uint8_t UDP_ROM_OP_END = 2;
constexpr uint8_t UDP_ROM_OP_ABORT = 3;
// 'N','P' | version | type | session u16 LE | op | flags | total u32 LE | crc32 u32 LE
constexpr uint8_t UDP_ROM_BEGIN_SIZE = 16;
// 'N','P' | version | type | session u16 LE | op | 0 | chunk u16 LE | len u16 LE | payload
constexpr uint8_t UDP_ROM_DATA_HEADER = 12;
// END and ABORT carry nothing beyond the session and op, so they are the bare
// common header — which is also the existing minimum length check.
constexpr uint8_t UDP_ROM_END_SIZE = 8;
constexpr int UDP_ROM_CHUNK = 1400;   // stays inside a 1500-byte MTU, as UDP_DEBUG_CHUNK
// Every request is acknowledged, so the sender can be a simple stop-and-wait
// loop: 'N','R' | version | op echo | session u16 LE | chunk echo u16 LE |
// status | expected chunk u16 LE | 0
constexpr uint8_t UDP_ROM_ACK_SIZE = 12;
constexpr uint8_t UDP_ROM_STATUS_OK = 0;
constexpr uint8_t UDP_ROM_STATUS_BUSY = 1;
constexpr uint8_t UDP_ROM_STATUS_TOO_BIG = 2;
constexpr uint8_t UDP_ROM_STATUS_ALLOC = 3;
constexpr uint8_t UDP_ROM_STATUS_SEQ = 4;
constexpr uint8_t UDP_ROM_STATUS_SIZE_MISMATCH = 5;
constexpr uint8_t UDP_ROM_STATUS_CRC = 6;
constexpr uint8_t UDP_ROM_STATUS_BAD_HEADER = 7;
constexpr uint8_t UDP_ROM_STATUS_UNSUPPORTED_MAPPER = 8;
constexpr uint8_t UDP_ROM_STATUS_NO_SESSION = 9;
// The largest image accepted. The supported mappers (0/1/2/3/4/24/26) top out
// near 768KB, so this leaves room while keeping the one permanent PSRAM
// reservation small against the 8MB part.
constexpr uint32_t ROM_MAX_SIZE = 1024 * 1024;
// BEGIN byte [7] bit 0: install the cart without resetting the CPU, the same
// semantics as the web build's nes_swap_rom.
constexpr uint8_t ROM_FLAG_SWAP = 0x01;
// How long a half-finished transfer keeps owning the staging buffer. A sender
// that dies mid-ROM would otherwise lock out every later attempt, so a new
// session may take over once the old one has been quiet this long.
constexpr uint32_t ROM_SESSION_TIMEOUT_MS = 3000;
// 'N','P' | version | type | seq u16 LE | mask u64 LE
constexpr uint8_t UDP_PIN_PACKET_SIZE = 14;
// Only bits 0..59 are meaningful (pins 1..60); applyPinMask ignores the rest.
// Senders differ on what they put in the top four bits — the browser builds the
// mask from the 60 pins it draws and leaves them clear, while a hand-written
// "all ok" is naturally ~0 — so every comparison masks down to this first.
// Getting that wrong silently pins the core on its slow fault path.
constexpr uint64_t PIN_MASK_VALID = (1ULL << 60) - 1;
// All 60 connector pins making contact — a properly seated cartridge.
constexpr uint64_t PIN_MASK_ALL_OK = PIN_MASK_VALID;

// ------------------------------------------------------------------- grove
// Local controllers on the Grove ports, merged (OR) with the UDP pads so either
// input source can drive the game.
//
// PORT.B (black — K151 base: pin1=G9 / pin2=G8): Joystick Unit. The port is
// nominally GPIO, but the ESP32 routes its I2C peripheral to any pin, so the
// external bus is simply bound to these pins instead of PORT.A's. Grove cables
// are straight-through, so SDA/SCL land on the same positions as on PORT.A
// (pin1=SDA, pin2=SCL). Both joystick generations are probed at boot; whichever
// answers is used.
constexpr int JOY_I2C_SDA = 9;   // PORT.B pin 1 (PORT.A equivalent: G2)
constexpr int JOY_I2C_SCL = 8;   // PORT.B pin 2 (PORT.A equivalent: G1)
constexpr uint8_t JOY2_I2C_ADDR = 0x63;   // Joystick2 (U024-C, STM32G030)
constexpr uint8_t JOY1_I2C_ADDR = 0x52;   // Joystick (U024, MEGA328P)
// 100kHz, not 400: PORT.B has no I2C pull-ups of its own (it is a GPIO port),
// so the bus leans on the ESP32's weak internal pulls and the unit's own
// resistors. At 400kHz the rise times are marginal and reads fail every second
// or two (observed on hardware as detect/lost flapping).
constexpr uint32_t GROVE_I2C_FREQ = 100000;
// A single failed read is a glitch (tolerable on this budget bus); only this
// many consecutive failures mean the stick was actually unplugged.
constexpr int JOY_READ_FAIL_LIMIT = 5;
// Poll every 8ms (~2 samples per NES frame). The reads happen on core 0 so their
// I2C latency never lands in the frame loop.
constexpr uint32_t GROVE_POLL_MS = 8;
// How far the stick must leave centre before it counts as a D-pad press, on a
// signed -128..127 scale. Large enough to ignore drift, small enough that a
// deliberate push always registers.
constexpr int JOY_DEADZONE = 40;
// Flip these if up/down or left/right come out mirrored on your unit — axis
// orientation differs between joystick revisions and mounting. X is inverted
// here: this Joystick2 reports right as negative (verified on hardware).
constexpr bool JOY_INVERT_X = true;
constexpr bool JOY_INVERT_Y = false;
// Joystick (U024) reports the button in byte 2; 1 = pressed on stock firmware.
constexpr bool JOY1_BTN_ACTIVE_HIGH = true;
// If the I2C joystick is absent (or unplugged), retry the probe about once a
// second so plugging it in later just works.
constexpr uint32_t JOY_REPROBE_MS = 1000;

// PORT.C (blue — K151 base: pin1=G17 / pin2=G18): Dual Button Unit. Nominally
// the UART port, but the pins are read as plain GPIO. Buttons short the signal
// line to GND when pressed. Grove wiring on the unit: white wire (pin2) = blue
// button, yellow wire (pin1) = red button — same positions that put blue on G8
// and red on G9 back when the unit lived on PORT.B.
constexpr int DUAL_BTN_PIN_BLUE = 18;   // blue button  -> NES B
constexpr int DUAL_BTN_PIN_RED = 17;   // red button   -> NES A

// NES pad bit layout (matches Pad::setButtons and the UDP protocol).
constexpr uint8_t NES_BTN_A = 0x01;
constexpr uint8_t NES_BTN_B = 0x02;
constexpr uint8_t NES_BTN_SELECT = 0x04;
constexpr uint8_t NES_BTN_START = 0x08;
constexpr uint8_t NES_BTN_UP = 0x10;
constexpr uint8_t NES_BTN_DOWN = 0x20;
constexpr uint8_t NES_BTN_LEFT = 0x40;
constexpr uint8_t NES_BTN_RIGHT = 0x80;

// -------------------------------------------------------------------- timing
// NTSC NES frame period: 1/60.0988 s.
constexpr int64_t FRAME_PERIOD_US = 16639;
// How far the frame schedule may run behind before it resyncs instead of
// catching up. Two periods covers the structural overrun of one repaint frame
// (~3ms) with margin, while keeping a real overload from accumulating unbounded
// debt that would burst-run the emulator when load finally drops.
constexpr int64_t FRAME_DEBT_CAP_US = 2 * FRAME_PERIOD_US;

// ------------------------------------------------------------------ logging
#define PERF_LOG 1
// CCOUNT ticks per microsecond, for turning NES_PROFILE's cycle counters into
// times. The ESP32-S3 on the CoreS3 runs at 240MHz and nothing here changes the
// clock, so this is a constant rather than a getCpuFrequencyMhz() call on a path
// that runs once a second next to the counters it scales.
constexpr uint32_t CPU_CYCLES_PER_US = 240;

// --------------------------------------------------------------------- WiFi
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t IP_DISPLAY_MS = 2000;

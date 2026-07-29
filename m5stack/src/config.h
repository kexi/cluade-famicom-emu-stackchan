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

// Adaptive display divisor: push every Nth emulated frame, chosen at runtime.
//
// Emulation must hold real-time 60Hz or the game and its music run slow, but a
// full 256x240 transfer costs ~27ms at 40MHz — more than a frame period. So the
// divisor floats: when the loop falls behind schedule the display refreshes less
// often (freeing whole frames of framebuffer-write and transfer time), and when
// there is slack it tightens back up.
constexpr uint32_t DISPLAY_DIVISOR_MIN = 1;
constexpr uint32_t DISPLAY_DIVISOR_MAX = 4;
constexpr uint32_t DISPLAY_DIVISOR_INITIAL = 3;

// Hysteresis for the divisor controller, in frames per adjustment window.
// Widening triggers earlier than narrowing so the loop settles instead of
// oscillating between two divisors that both sit near the threshold.
constexpr int DIVISOR_WINDOW_FRAMES = 30;      // ~0.5s at 60Hz
constexpr int DIVISOR_LATE_WIDEN = 6;          // late frames in a window -> widen
constexpr int DIVISOR_LATE_NARROW = 1;         // at or below this -> try narrowing

// ------------------------------------------------------------------- audio
constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr uint8_t SPEAKER_VOLUME = 128;      // 0-255, M5.Speaker master volume
// The device level the browser's 1.0 master volume maps to, so the two agree on
// what "normal" sounds like. The web slider spans 0..1.5, i.e. up to 192 here.
constexpr uint8_t SPEAKER_VOLUME_BASE = SPEAKER_VOLUME;
constexpr uint8_t SPEAKER_CHANNEL = 1;       // virtual channel used for playRaw
constexpr int AUDIO_BUF_SAMPLES = 2048;      // matches APU::sampleBuf capacity

// Staging between the APU and the speaker. The ring gives ~186ms of slack so a
// late frame cannot starve the hardware; chunks are the unit handed to playRaw.
constexpr int AUDIO_RING_SAMPLES = 8192;     // ~186ms at 44.1kHz
constexpr int AUDIO_CHUNK_SAMPLES = 512;     // ~11.6ms per submission
constexpr int AUDIO_CHUNK_SLOTS = 4;         // rotation depth; playRaw holds the pointer

// The NES produces 44100/60.0988 samples per frame. When the emulator cannot hit
// 60fps the sample rate is retimed to the measured rate so playback stays
// continuous (slightly lower pitch) instead of repeatedly underrunning.
constexpr double AUDIO_SAMPLES_PER_FRAME = 733.8;

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
constexpr uint8_t UDP_CTRL_RESET = 0x01;    // byte [6] bit 0
constexpr uint8_t UDP_CTRL_VOLUME = 0x02;   // byte [6] bit 1, level in byte [7]
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
constexpr int JOY_I2C_SDA = 9;    // PORT.B pin 1 (PORT.A equivalent: G2)
constexpr int JOY_I2C_SCL = 8;    // PORT.B pin 2 (PORT.A equivalent: G1)
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
constexpr int DUAL_BTN_PIN_BLUE = 18;  // blue button  -> NES B
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

// ------------------------------------------------------------------ logging
#define PERF_LOG 1

// --------------------------------------------------------------------- WiFi
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t IP_DISPLAY_MS = 2000;

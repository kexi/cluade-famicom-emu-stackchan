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

// -------------------------------------------------------------------- timing
// NTSC NES frame period: 1/60.0988 s.
constexpr int64_t FRAME_PERIOD_US = 16639;

// ------------------------------------------------------------------ logging
#define PERF_LOG 1

// --------------------------------------------------------------------- WiFi
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t IP_DISPLAY_MS = 2000;

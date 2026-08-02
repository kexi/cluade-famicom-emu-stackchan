#pragma once

#include <cstddef>
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
// 60 rows = 30720 bytes at 256px x RGB565, still inside the 32768-byte limit
// (with ~2KB to spare) and an exact divisor of 240, so all four bands are the
// same size and every one of them is a single hardware transaction — the whole
// point of banding. 64 rows would sit exactly on the ceiling with no margin and
// is not a divisor of 240; 48 would work too but gives five bands, and the
// segment count doubles as the display divisor, so the band count is what sets
// how often the panel is refreshed. Four bands means the once-per-picture
// repaint is amortised over four frames instead of six: 50% more panel updates
// for one extra kick's worth of nothing (~186us each at 60 rows).
constexpr int DISPLAY_DMA_ROWS = 60;
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
// 15Hz panel refresh; at the ~33fps this core actually manages, ~8Hz.
constexpr uint32_t DISPLAY_DIVISOR_MIN = 4;
constexpr uint32_t DISPLAY_DIVISOR_MAX = 4;
constexpr uint32_t DISPLAY_DIVISOR_INITIAL = 4;
static_assert(DISPLAY_DIVISOR_MIN >= (uint32_t)DISPLAY_DMA_SEGMENTS,
              "divisor floor must cover the band count, or a draw frame flushes bands inline");
static_assert(DISPLAY_DIVISOR_INITIAL >= DISPLAY_DIVISOR_MIN && DISPLAY_DIVISOR_INITIAL <= DISPLAY_DIVISOR_MAX,
              "initial divisor must lie within the configured range");

// Runtime guard on the repaint-versus-last-band race.
//
// A draw frame begins repainting while the final band (the last DISPLAY_DMA_ROWS
// rows, starting at NES_HEIGHT - DISPLAY_DMA_ROWS; 180-239 at the current values)
// may still be on the wire. That is safe only because the writer is slower than the
// reader: renderScanline paints strictly top-to-bottom, so the last band's first
// row is not touched until that fraction of the way through emulation, while the
// band clears the wire earlier. Setting the two equal gives break-even; the guard
// trips at a multiple of it.
//
// The derivation below used to be a comment ending in a hand-computed 16.45f,
// which meant DISPLAY_DMA_ROWS or SPI_WRITE_FREQ could move without the literal
// following. It is now the expression itself, so the constant cannot go stale.
//
// One band's wire time: the transfer is clock-bound, so it is simply the bits
// pushed divided by the SPI clock, linear in the row count as the measurements
// showed (4.10ms at 40 rows, 6.15ms at 60).
constexpr float DISPLAY_BAND_WIRE_MS =
    (float)NES_WIDTH * DISPLAY_DMA_ROWS * 2 /* bytes/px */ * 8 /* bits/byte */ * 1000.0f / (float)SPI_WRITE_FREQ;
// Slack between the kick returning and the repaint reaching row 0. Measured at
// roughly half a millisecond; subtracted from the wire time because that much of
// the transfer is already done before the writer starts moving at all.
constexpr float DISPLAY_KICK_TO_REPAINT_MS = 0.5f;
// Emulated frame time at which the writer catches the reader. The writer covers
// 262 scanlines' worth of time per frame but only has to reach the last band's
// first row, hence the ratio.
constexpr float DISPLAY_REPAINT_BREAK_EVEN_MS =
    (DISPLAY_BAND_WIRE_MS - DISPLAY_KICK_TO_REPAINT_MS) * 262.0f / (float)(NES_HEIGHT - DISPLAY_DMA_ROWS);
// Margin over break-even. 2x rather than something tighter because the quantity
// being guarded is an EWMA of a noisy measurement, and the cost of firing early
// (one join, i.e. a stall of whatever wire time is still owed) is far cheaper
// than firing late (a torn picture).
constexpr float DISPLAY_REPAINT_GUARD_MARGIN = 2.0f;
//
// At 60 rows / 40MHz this comes to wire=6.144ms, break-even=8.22ms, guard=16.4ms.
// Measured emuD is ~19ms, so the margin over the guard is only ~1.15x — far
// tighter than the ~2x this had at 40 rows. The guard is now close enough to fire
// on a genuinely fast frame, which costs a join rather than a torn picture, and
// the EWMA's slow alpha keeps a single quick frame from arming it. It exists
// because the writer/reader ratio is not enforced by anything else: a faster
// core, a larger DISPLAY_DMA_ROWS or a higher SPI_WRITE_FREQ all erode it
// silently, and the failure mode is a torn picture rather than anything a build
// would catch.
constexpr float DISPLAY_REPAINT_GUARD_MS = DISPLAY_REPAINT_BREAK_EVEN_MS * DISPLAY_REPAINT_GUARD_MARGIN;
// The derivation replaced a hand-computed 16.45f. Pin the result so a future
// change to the inputs that lands somewhere unexpected is caught at build time
// rather than becoming a torn picture on hardware.
static_assert(DISPLAY_REPAINT_GUARD_MS > 16.0f && DISPLAY_REPAINT_GUARD_MS < 17.0f,
              "repaint guard moved away from the ~16.4ms this display path was tuned for");
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

// ----------------------------------------------------------------- SD card
// The CoreS3's microSD sits on the same SPI bus as the LCD (SCK=36, MISO=35,
// MOSI=37; only the chip select differs), so a card access issued while a band
// DMA is in flight would interleave with the panel's transaction. Nothing in
// the SD driver can see that, which is why the rule is structural instead:
// every sdRom* call happens on core 1 after joinBand(), with no band
// outstanding. See sd_rom.h.
//
// 25MHz rather than the 40MHz the panel runs at: the card is reached through
// the same traces plus a socket, the SD spec's high-speed ceiling is 50MHz for
// SDHC and cheap cards routinely miss it, and nothing here is throughput-bound
// (a 1MB ROM at 25MHz is ~0.3s of wire time against the ~1-2s the whole save
// is budgeted for). Drop to 20 or 10MHz if a card proves flaky.
constexpr uint32_t SD_SPI_FREQ = 25000000;
// Where ROMs live. A fixed directory, not a configurable root, because the
// path a network peer can influence is then only ever a basename appended to
// this constant — traversal is impossible by construction rather than by
// checking for "..".
constexpr char SD_ROMS_DIR[] = "/roms";
// Suffix for a save still in progress. A ".nes" only ever appears under its
// final name after a successful rename, so an interrupted write cannot be
// mistaken for a complete image.
constexpr char SD_PART_SUFFIX[] = ".part";
// Ceiling on a directory listing. The array is a static in main.cpp
// (64 * 68B ≈ 4.4KB), and a listing that cannot fit the menu's scroll is of no
// use to anyone; extra files are skipped with a serial warning.
constexpr int SD_ROM_MAX_FILES = 64;
// Longest file name handled, including the NUL. FAT short names are 8.3 and
// VFAT allows 255, but 64 covers every realistic ROM name while keeping the
// listing array and the type 5 packet entries small.
constexpr int SD_ROM_NAME_MAX = 64;
// Slack demanded on top of the image before a save is attempted. FAT allocates
// in clusters and the directory entry itself costs space, so a save sized to
// exactly the free bytes can still fail half-written; refusing early keeps
// that case out of the .part cleanup path.
constexpr uint32_t SD_SAVE_MARGIN_BYTES = 64 * 1024;
// Unit of a file read/write. Large enough that the per-call overhead of the
// FAT layer disappears against the transfer, small enough to sit on the loop
// task's stack budget without a heap allocation.
constexpr size_t SD_IO_CHUNK = 4096;

// ------------------------------------------------------------------- menu
// The ROM picker shown at boot and on a BtnC hold. Drawn with ordinary display
// primitives rather than the band DMA path: no picture is being emulated while
// it is up, so there is nothing to hide the transfer under and the blocking
// push is both simpler and safe next to the SD accesses the menu makes.
//
// 24px rows at text size 2 (16px glyphs) leaves 8px of leading, which is what
// makes a row comfortably tappable on a 320x240 panel without a hit box that
// disagrees with what is drawn.
constexpr int MENU_ROW_H = 24;
constexpr int MENU_TOP_Y = 30;   // below the title line
constexpr int MENU_VISIBLE_ROWS = 7;
static_assert(MENU_TOP_Y + MENU_VISIBLE_ROWS * MENU_ROW_H <= 220,
              "menu rows must leave room for the footer guide at the bottom of the panel");
// D-pad auto-repeat while a direction is held: the first repeat waits, the rest
// come quickly. Same shape as every console menu, and the numbers are the usual
// ones — short enough that a 60-entry card is not a chore, long enough that a
// deliberate single step does not double-fire.
constexpr uint32_t MENU_REPEAT_DELAY_MS = 400;
constexpr uint32_t MENU_REPEAT_MS = 120;
// Poll period while the menu is up. ~60Hz, so touch and pad feel the same as in
// game, and slow enough to leave core 1 mostly idle for the UDP replies.
constexpr uint32_t MENU_TICK_MS = 16;
// Title shown for the first row (the flash-embedded image). The row reads
// "<title>[Built-in]" — the title names the game, the fixed tag names where it
// lives; the tag itself stays in menu.cpp because it never changes. Kept here,
// not in menu.cpp, because the title must change when data/game.nes does.
constexpr char MENU_BUILTIN_TITLE[] = "KARYUDO";

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
// ROM 選択メニューを開く (ゲーム中のみ意味を持つ)。プロコンの HOME など、
// NES のパッドビットに居場所のないボタンをメニュー呼び出しに使うための口。
constexpr uint8_t UDP_CTRL_MENU = 0x04;   // byte [6] bit 2

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
// BEGIN byte [7] bit 1: also write the image to the SD card under the name
// carried in the BEGIN's optional tail (see UDP_ROM_BEGIN_NAMED_SIZE). Without
// a name the flag is ignored — there would be nothing to call the file.
constexpr uint8_t ROM_FLAG_SAVE_SD = 0x02;
// BEGIN byte [7] bit 2: do not install the image, only save it. Lets the
// browser fill the card without interrupting whatever is running, which is the
// difference between "add to my library" and "play this now".
constexpr uint8_t ROM_FLAG_NO_LOAD = 0x04;
// BEGIN with a file name appended:
//   ... the 16 bytes above ... | nameLen u8 | name[nameLen]
//
// Length-discriminated rather than flag-discriminated: a sender that predates
// this sends exactly UDP_ROM_BEGIN_SIZE bytes, so anything longer is
// unambiguously the new form and anything equal is unambiguously the old one.
// A flag would have needed the old firmware to have reserved a bit for it,
// which it did not.
//
// The name is *not* trusted as a path. It is reduced to a basename and put
// through sdRomSanitizeName() before it touches the filesystem, so at most it
// determines what the file inside SD_ROMS_DIR is called. A name that sanitises
// to something different is rejected outright with SdStatus::BadName rather
// than silently saved under the mangled spelling.
constexpr uint8_t UDP_ROM_BEGIN_NAMED_SIZE = UDP_ROM_BEGIN_SIZE + 1;   // minimum with an (empty) name field
// Reported once the SD write has been attempted, in a separate datagram: the
// END ACK goes out from the UDP task the moment the CRC checks, while the save
// happens later on core 1 at a frame boundary. Making the sender wait for the
// ACK until then would hold the transfer open across a ~1-2s card write.
//   'N','S' | version | UDP_TYPE_ROM | session u16 LE | SdStatus | 0
constexpr uint8_t UDP_ROM_SAVE_EVENT_SIZE = 8;
// How long a half-finished transfer keeps owning the staging buffer. A sender
// that dies mid-ROM would otherwise lock out every later attempt, so a new
// session may take over once the old one has been quiet this long.
constexpr uint32_t ROM_SESSION_TIMEOUT_MS = 3000;
// SD card commands. Separate from type 4 because type 4 is a bulk transfer with
// its own session state machine, while these are single request/reply pairs
// against the filesystem — folding them in would mean a LIST could be rejected
// as BUSY by a half-finished ROM upload it has nothing to do with.
//
// Every request is handled on core 1 at a frame boundary, never in the UDP
// task: the card shares its SPI bus with the panel (see the SD section below),
// so an access from core 0 could land mid-band. The task therefore only latches
// the request, and the reply is sent from the frame loop once the work is done.
// One request is held at a time; a second arriving while the first is pending
// is answered immediately with SdStatus::Busy.
//
// Request layout, all ops:
//   [0..1] 'N','P'
//   [2]    version
//   [3]    UDP_TYPE_SD
//   [4..5] seq u16 LE      — echoed in every reply, so a late answer to an
//                            abandoned request is discardable
//   [6]    op
//   [7]    0 (reserved)
//   [8..]  op-specific payload, described per op below
constexpr uint8_t UDP_TYPE_SD = 5;
constexpr uint8_t UDP_SD_HEADER = 8;
// op 0 LIST: no payload.
constexpr uint8_t UDP_SD_OP_LIST = 0;
// op 1 LOAD: nameLen u8 | name[nameLen]. Reads the file into staging and
// installs it, i.e. the network equivalent of picking a row in the menu.
constexpr uint8_t UDP_SD_OP_LOAD = 1;
// op 2 DELETE: nameLen u8 | name[nameLen].
constexpr uint8_t UDP_SD_OP_DELETE = 2;
// op 3 RENAME: fromLen u8 | from[fromLen] | toLen u8 | to[toLen]. Refuses to
// overwrite an existing target (SdStatus::Exists) — the confirmation belongs in
// the UI that has a user to ask.
constexpr uint8_t UDP_SD_OP_RENAME = 3;

// Reply to LOAD / DELETE / RENAME, one datagram:
//   [0..1] 'N','S'
//   [2]    version
//   [3]    op echo
//   [4..5] seq echo u16 LE
//   [6]    SdStatus
//   [7]    0
constexpr uint8_t UDP_SD_ACK_SIZE = 8;

// Reply to LIST, split across as many datagrams as the entries need:
//   [0..1]   'N','S'
//   [2]      version
//   [3]      UDP_SD_OP_LIST
//   [4..5]   seq echo u16 LE
//   [6]      SdStatus       — anything but Ok means the rest is absent
//   [7]      0
//   [8]      part u8        — 0-based index of this datagram
//   [9]      nparts u8      — total datagrams in this reply
//   [10..11] total u16 LE   — entries in the whole listing, not in this part
//   [12..13] count u16 LE   — entries carried by this datagram
//   [14..21] totalBytes u64 LE  — card capacity
//   [22..29] freeBytes  u64 LE  — card free space
//   [30..]   count entries, each: size u32 LE | nameLen u8 | name[nameLen]
//
// The capacity fields repeat in every part rather than riding only on part 0,
// so a receiver that lost part 0 to a dropped datagram still has them once it
// has retried. nparts is always at least 1: an empty card answers with a single
// part carrying count=0, which is how "mounted but no ROMs" is told apart from
// "no reply at all".
constexpr uint8_t UDP_SD_LIST_HEADER = 30;
// Ceiling on one LIST datagram, matching the other split replies here.
constexpr int UDP_SD_CHUNK = 1400;
// Worst-case bytes one entry occupies: size u32 + nameLen u8 + the longest name
// the firmware will list.
constexpr int UDP_SD_ENTRY_MAX = 4 + 1 + SD_ROM_NAME_MAX;
static_assert(UDP_SD_LIST_HEADER + UDP_SD_ENTRY_MAX <= UDP_SD_CHUNK,
              "a LIST datagram must hold at least one entry, or the split cannot make progress");

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

// -------------------------------------------------------------- head touch
// M5Stack 公式 StackChan の頭頂部タッチセンサー (Si12T)。3 ゾーンを前後に
// 2 回なぞる (なでなで) とゲーム中でも ROM 選択メニューに戻る。
//
// Grove と違ってポーリングは core 1 の loop から直接行う: 読むのは 1 バイトの
// レジスタ 1 つだけで、タスクと atomic を 1 組増やすほどの仕事ではない。
//
// 16ms 周期 (毎フレーム相当)。当初は 33ms に絞っていたが、速くゴシゴシした
// ときに隣接ゾーンが同じポーリングで同時に立ってしまい、移動 (距離) として
// 数えられず取りこぼした (実機ログで確認)。1 バイト読み ~0.3ms/フレームは
// フレーム予算 16.6ms に対して誤差の範囲。
constexpr uint32_t HEAD_TOUCH_POLL_MS = 16;
// メニューを開くのに要求する「撫でた距離」。単位はゾーン境界のまたぎ回数で、
// 頭の端から端までの片道が 2、往復で 4 になる。回数 (ストローク) 数えでは
// なく距離にしたのは、指を離さず連続でゴシゴシする自然ななでなでを 1 回と
// 数え損ねないため。1 で開くと抱え上げたときの偶発的な一触れで即ゲームが
// 中断する。4 (往復) は実機で「反応が悪い」となったので、片道 + 1 またぎに
// 緩めてある — 偶発的な接触は 1 またぎ止まりで、これでも誤爆しない。
constexpr int HEAD_TOUCH_TRAVEL_TO_MENU = 3;
// 動き (新しいゾーンへの移動) がこれだけ途絶えたら距離を数え直す。指を
// 置いたまま考えている・単発の一撫でを、後から来る撫でと合算しないため。
constexpr uint32_t HEAD_TOUCH_TRAVEL_RESET_MS = 1000;

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

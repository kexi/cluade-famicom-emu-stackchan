// M5Stack CoreS3 frontend for the NES core.
//
// Core 1 (Arduino loop) runs emulation, audio and display; core 0 runs a
// blocking UDP receive task that only publishes controller bits. Splitting
// this way keeps the network stack's latency out of the frame loop.

#include <M5Unified.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <esp_timer.h>
#include <atomic>

#include "../../core/nes.h"
#include "config.h"
#include "secrets.h"

// Statically allocated in internal SRAM (not PSRAM): ppu.framebuffer is handed
// to pushImageDMA, and the LCD DMA engine cannot read from PSRAM reliably.
static nes::NES g_nes;

extern const uint8_t rom_start[] asm("_binary_data_game_nes_start");
extern const uint8_t rom_end[] asm("_binary_data_game_nes_end");

static std::atomic<uint8_t> g_padBits[2] = {};
static std::atomic<uint32_t> g_lastRxMs{0};

// Cartridge connector state from the browser's pin UI. Deliberately NOT subject
// to INPUT_TIMEOUT_MS: a pulled pin is a physical condition that persists until
// the user reseats the cart, unlike a held button which must be released if the
// sender dies. Recovery is an explicit all-ones mask.
static std::atomic<uint64_t> g_pinMask{PIN_MASK_ALL_OK};
// Set by the UDP task, consumed once at a frame boundary by the emulation loop.
static std::atomic<bool> g_resetRequested{false};
// Master volume the browser asked for, or -1 while it has never said anything —
// in which case the compile-time SPEAKER_VOLUME stands and is left untouched.
static std::atomic<int> g_volume{-1};

// Audio staging: APU output lands in the ring, and fixed-size chunks are handed
// to the speaker from there. Both live in internal SRAM.
static int16_t g_ring[AUDIO_RING_SAMPLES];
static int g_ringWrite = 0, g_ringRead = 0;
static uint32_t g_ringDropped = 0;
static int16_t g_chunk[AUDIO_CHUNK_SLOTS][AUDIO_CHUNK_SAMPLES];
static int g_chunkIndex = 0;

static bool g_wifiConnected = false;

// ---------------------------------------------------------------- UDP task

static void udpTask(void*) {
    int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const bool socketFailed = sock < 0;
    if (socketFailed) {
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(UDP_PORT);

    const bool bindFailed = ::bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0;
    if (bindFailed) {
        ::close(sock);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t packet[64];
    for (;;) {
        const int received = ::recvfrom(sock, packet, sizeof(packet), 0, nullptr, nullptr);
        const bool tooShort = received < UDP_PACKET_SIZE;
        if (tooShort) continue;

        const bool magicOk = packet[0] == 'N' && packet[1] == 'P';
        const bool versionOk = packet[2] == UDP_PROTOCOL_VERSION;
        if (!magicOk || !versionOk) continue;

        const uint8_t type = packet[3];
        if (type == UDP_TYPE_PINS) {
            // A pin packet is longer than the pad packet, so re-check the length
            // rather than reading past what actually arrived.
            const bool pinPacketShort = received < UDP_PIN_PACKET_SIZE;
            if (pinPacketShort) continue;
            uint64_t mask = 0;
            for (int i = 0; i < 8; i++) mask |= (uint64_t)packet[6 + i] << (i * 8);
            // Normalise here so every later comparison against PIN_MASK_ALL_OK
            // works regardless of what the sender left in the unused top bits.
            g_pinMask.store(mask & PIN_MASK_VALID, std::memory_order_relaxed);
            continue;   // not controller input: leave g_lastRxMs alone
        }
        if (type == UDP_TYPE_CTRL) {
            const uint8_t cmd = packet[6];
            // Latch rather than act here: this runs on core 0 while the emulator
            // is mid-frame on core 1, so the work happens at a frame boundary.
            if (cmd & UDP_CTRL_RESET) g_resetRequested.store(true, std::memory_order_relaxed);
            if (cmd & UDP_CTRL_VOLUME) g_volume.store(packet[7], std::memory_order_relaxed);
            continue;
        }

        // type 0 (or a legacy sender's zero "reserved" byte): controller state.
        g_padBits[0].store(packet[6], std::memory_order_relaxed);
        g_padBits[1].store(packet[7], std::memory_order_relaxed);
        g_lastRxMs.store(millis(), std::memory_order_relaxed);
    }
}

// ------------------------------------------------------------------- boot

static void showMessage(const char* text, int y, int size) {
    M5.Display.setTextSize(size);
    M5.Display.setCursor(4, y);
    M5.Display.print(text);
}

static bool connectWifi() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    showMessage("Connecting to " WIFI_SSID "...", 8, 2);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    const uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
    while (WiFi.status() != WL_CONNECTED) {
        const bool timedOut = millis() > deadline;
        if (timedOut) {
            Serial.printf("WIFI: timeout ssid=%s status=%d\n", WIFI_SSID, (int)WiFi.status());
            return false;
        }
        delay(200);
    }
    Serial.printf("WIFI: connected ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    return true;
}

static void haltWithError(const char* text) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    showMessage(text, 8, 2);
    for (;;) delay(1000);
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);

    M5.Display.setRotation(1);
    M5.Display.setSwapBytes(DISPLAY_SWAP_BYTES);

    // M5GFX defaults the CoreS3 panel to 40MHz, which alone costs 24.6ms for a
    // 256x240 RGB565 frame — more than a whole 60fps budget. The ILI9342C is
    // rated well past this and the CoreS3 traces are short, so push the write
    // clock up; reads stay at the conservative default.
    if (auto* bus = M5.Display.getPanel()->getBus()) bus->setClock(SPI_WRITE_FREQ);

    M5.Display.fillScreen(TFT_BLACK);

    // config は begin 前でないと反映されない
    auto speakerCfg = M5.Speaker.config();
    speakerCfg.sample_rate = AUDIO_SAMPLE_RATE;
    M5.Speaker.config(speakerCfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(SPEAKER_VOLUME);

    g_wifiConnected = connectWifi();
    if (g_wifiConnected) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
        showMessage(WiFi.localIP().toString().c_str(), 8, 3);
        showMessage("UDP port 5555", 48, 2);
        delay(IP_DISPLAY_MS);
        xTaskCreatePinnedToCore(udpTask, "udp", 4096, nullptr, 5, nullptr, 0);
    }

    const size_t romSize = (size_t)(rom_end - rom_start);
    const bool romLoaded = g_nes.loadRom(rom_start, romSize);
    if (!romLoaded) haltWithError("ROM load failed");

    g_nes.apu.setSampleRate(AUDIO_SAMPLE_RATE);
    g_nes.powerOn();

    // Prime the speaker queue with silence so the first real chunks arrive with
    // margin instead of racing an already-empty hardware buffer.
    for (int i = 0; i < 2; i++) {
        memset(g_chunk[g_chunkIndex], 0, sizeof(g_chunk[0]));
        M5.Speaker.playRaw(g_chunk[g_chunkIndex], AUDIO_CHUNK_SAMPLES,
                           AUDIO_SAMPLE_RATE, false, 1, SPEAKER_CHANNEL);
        g_chunkIndex = (g_chunkIndex + 1) % AUDIO_CHUNK_SLOTS;
    }

    // NB: no startWrite() here. Holding the bus open across the whole run leaves
    // the panel's address window owned by whatever ran last, so pushes land at
    // the wrong offset; pushImageDMA sets the window itself per call.
    Serial.printf("DISPLAY: %dx%d rot=%d push=(%d,%d,%d,%d)\n",
                  M5.Display.width(), M5.Display.height(), M5.Display.getRotation(),
                  SCREEN_X_OFFSET, 0, NES_WIDTH, NES_HEIGHT);

    M5.Display.fillScreen(TFT_BLACK);
    if (!g_wifiConnected) {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        showMessage("WiFi: failed", 228, 1);
    }
}

// ------------------------------------------------------------------- loop

static void applyInput() {
    const uint32_t sinceRx = millis() - g_lastRxMs.load(std::memory_order_relaxed);
    const bool inputStale = sinceRx > INPUT_TIMEOUT_MS;
    if (inputStale) {
        g_nes.pad[0].setButtons(0);
        g_nes.pad[1].setButtons(0);
        return;
    }
    g_nes.pad[0].setButtons(g_padBits[0].load(std::memory_order_relaxed));
    g_nes.pad[1].setButtons(g_padBits[1].load(std::memory_order_relaxed));
}

// Push connector state into the core, but only on an actual change: applyPinMask
// recomputes the derived masks and drops the PPU's CHR shortcut, so calling it
// every frame would throw away the fast path for nothing.
static void applyPinChanges() {
    static uint64_t applied = PIN_MASK_ALL_OK;
    const uint64_t wanted = g_pinMask.load(std::memory_order_relaxed);
    const bool unchanged = wanted == applied;
    if (unchanged) return;
    applied = wanted;
    g_nes.applyPinMask(wanted);
    Serial.printf("PINS: %016llx\n", (unsigned long long)wanted);
}

// The RESET button, as the browser's connector UI presses it. Runs after the pin
// state is applied so a "reseat and reset" arrives in the same order it happens
// on a real console: contacts restored first, then the reset vector fetched.
// NES::reset() keeps work RAM, which is what the physical button does.
static void applyResetRequest() {
    const bool requested = g_resetRequested.exchange(false, std::memory_order_relaxed);
    if (!requested) return;
    g_nes.reset();
    Serial.println("RESET: console reset");
}

// Mirror the browser's master volume onto the speaker. M5Unified recomputes its
// gain from _master_volume on every DMA block in the output task, so this takes
// effect on audio that has already been queued — no need to drain or restart.
static void applyVolumeRequest() {
    static int applied = -1;
    const int wanted = g_volume.load(std::memory_order_relaxed);
    const bool unset = wanted < 0;
    if (unset || wanted == applied) return;
    applied = wanted;
    M5.Speaker.setVolume((uint8_t)wanted);
    Serial.printf("VOL: %d\n", wanted);
}

// Decouple APU production from Speaker consumption.
//
// The speaker's per-channel queue holds only two entries, so submitting one
// frame-sized block per loop leaves no slack: a single late frame underruns and
// clicks. Instead the APU's output is accumulated into a ring and drained in
// fixed chunks, keeping several chunks queued ahead of the hardware at all times.
static void enqueueAudio() {
    const int produced = g_nes.apu.sampleCount;
    g_nes.apu.sampleCount = 0;
    // Famicom audio loops out through the cartridge (pins 45/46), so a bad
    // contact there silences the console — same semantics as the web build's
    // muteIfCartAudioBroken. Zero-fill rather than skip so the ring keeps being
    // fed and playback stays continuous instead of underrunning into clicks.
    const bool cartAudioBroken = !g_nes.soundOk;
    // APU::mix() swings 0..~0.45, i.e. entirely positive with a large standing DC
    // component. Feeding that to the speaker wastes half the headroom and turns
    // every queue start/stop into an audible step, so block DC first.
    static float dcX1 = 0.0f, dcY1 = 0.0f;
    for (int i = 0; i < produced; i++) {
        const float x = cartAudioBroken ? 0.0f : g_nes.apu.sampleBuf[i];
        const float y = x - dcX1 + AUDIO_DC_POLE * dcY1;
        dcX1 = x;
        dcY1 = y;

        float s = y * AUDIO_HEADROOM;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        g_ring[g_ringWrite] = (int16_t)(s * 32767.0f);
        g_ringWrite = (g_ringWrite + 1) % AUDIO_RING_SAMPLES;
        const bool ringFull = g_ringWrite == g_ringRead;
        if (ringFull) {   // drop the oldest sample rather than the newest
            g_ringRead = (g_ringRead + 1) % AUDIO_RING_SAMPLES;
            g_ringDropped++;
        }
    }
}

static int ringAvailable() {
    return (g_ringWrite - g_ringRead + AUDIO_RING_SAMPLES) % AUDIO_RING_SAMPLES;
}

// Submit whole chunks, but never more than the speaker can accept without
// blocking. playRaw keeps referencing the buffer it was given, so a chunk slot
// is only reused once it has cycled all the way around the rotation.
//
// M5Unified gives each channel two wav slots, and playRaw() (via _set_next_wav)
// *busy-waits* on a 1-tick semaphore until one frees up. Because the queue only
// drains in real time, handing it a third chunk parks the emulation loop for the
// remainder of a chunk — measured at 5.0ms average, 29ms worst case, i.e. ~200ms
// of stall per second. Worse, it self-amplifies: a lower frame rate retimes
// playbackRate down, which makes each queued chunk last *longer* in wall-clock
// time, which makes the next playRaw block for longer still. Pinning the rate to
// 44.1kHz breaks the loop (measured 27.4 -> 32.4fps) but then consumes ~44k
// samples/s against ~24k produced, so the ring runs dry and the audio gaps.
//
// Checking isPlaying() first keeps both properties: the rate stays matched to
// what the emulator actually produces, and the loop never blocks. Samples that
// do not fit simply stay in the ring, which is what the ring is for.
static void drainAudio(uint32_t rate) {
    while (ringAvailable() >= AUDIO_CHUNK_SAMPLES) {
        // Both slots busy: submitting now would block until one drains.
        const bool queueFull = M5.Speaker.isPlaying(SPEAKER_CHANNEL) >= 2;
        if (queueFull) return;
        int16_t* chunk = g_chunk[g_chunkIndex];
        for (int i = 0; i < AUDIO_CHUNK_SAMPLES; i++) {
            chunk[i] = g_ring[g_ringRead];
            g_ringRead = (g_ringRead + 1) % AUDIO_RING_SAMPLES;
        }
        const bool queued = M5.Speaker.playRaw(chunk, AUDIO_CHUNK_SAMPLES, rate,
                                               false, 1, SPEAKER_CHANNEL);
        if (!queued) {
            // Queue is full: rewind so the samples are not lost, and stop.
            g_ringRead = (g_ringRead - AUDIO_CHUNK_SAMPLES + AUDIO_RING_SAMPLES)
                         % AUDIO_RING_SAMPLES;
            return;
        }
        g_chunkIndex = (g_chunkIndex + 1) % AUDIO_CHUNK_SLOTS;
    }
}

// Display refresh divisor, adjusted from how often the loop misses its deadline.
//
// Raising it does two things at once: the PPU stops writing pixels on the
// skipped frames (see PPU::renderThisFrame), and — because those frames never
// touch the framebuffer — the DMA transfer of the last drawn frame is free to
// run underneath them. Emulation therefore keeps its real-time 60Hz and only the
// panel's refresh rate gives way.
static uint32_t g_divisor = DISPLAY_DIVISOR_INITIAL;

static void adjustDivisor(bool wasLate) {
    static int windowFrames = 0, lateFrames = 0;
    windowFrames++;
    if (wasLate) lateFrames++;
    const bool windowOpen = windowFrames < DIVISOR_WINDOW_FRAMES;
    if (windowOpen) return;

    const bool tooSlow = lateFrames >= DIVISOR_LATE_WIDEN;
    const bool hasSlack = lateFrames <= DIVISOR_LATE_NARROW;
    if (tooSlow && g_divisor < DISPLAY_DIVISOR_MAX) g_divisor++;
    else if (hasSlack && g_divisor > DISPLAY_DIVISOR_MIN) g_divisor--;

    windowFrames = 0;
    lateFrames = 0;
}

void loop() {
    static int64_t nextFrameUs = esp_timer_get_time();
#if PERF_LOG
    static int64_t perfWindowUs = esp_timer_get_time();
    static uint32_t perfFrames = 0, perfDrawn = 0;
    static int64_t perfEmuUs = 0, perfPushUs = 0;
    // Split by drawn/skipped: the gap between them is the cost of the draw path,
    // which is what the display divisor actually buys back.
    static int64_t perfEmuDrawUs = 0, perfEmuSkipUs = 0, perfJoinUs = 0;
    static int perfRingMin = AUDIO_RING_SAMPLES, perfRingMax = 0;
#endif

    // Retimed from the measured frame rate: while the emulator runs below 60fps
    // it also produces samples below 44.1kHz, so playing them at the nominal rate
    // would drain the ring faster than it fills.
    static uint32_t playbackRate = AUDIO_SAMPLE_RATE;

    M5.update();
    applyInput();
    applyPinChanges();
    applyResetRequest();
    applyVolumeRequest();

    static uint32_t frameIndex = 0;
    const bool drawThisFrame = (frameIndex++ % g_divisor) == 0;
    g_nes.ppu.renderThisFrame = drawThisFrame;

    const int64_t emuStartUs = esp_timer_get_time();
    // The DMA engine reads ppu.framebuffer in place — a second copy would need
    // another 120KB of internal SRAM, which is not available. So the transfer is
    // only joined here, immediately before the frame that is about to overwrite
    // the buffer. Skipped frames leave it untouched, which is exactly what lets
    // the ~27ms transfer hide under them.
    //
    // endWrite() closes the transaction opened after the last push, and
    // Panel_LCD::end_transaction() waits on the bus — so this both joins the
    // transfer and releases the SPI lock. waitDMA() alone would leave the
    // transaction open and the lock held.
    static bool pushOutstanding = false;
    if (drawThisFrame && pushOutstanding) {
        M5.Display.endWrite();
        pushOutstanding = false;
    }
    const int64_t joinEndUs = esp_timer_get_time();
    g_nes.runFrame();
    const int64_t emuEndUs = esp_timer_get_time();

    enqueueAudio();
    drainAudio(playbackRate);

    // Fire and forget. pushImageDMA queues the transfer and returns, but it wraps
    // itself in startWrite()/endWrite() and Panel_LCD's end_transaction() calls
    // _bus->wait() — so left alone it blocks for the full ~27ms and the overlap
    // never happens. IPanel refcounts the nesting, so an outer startWrite() here
    // demotes that inner endWrite() to a decrement and leaves the transfer in
    // flight. The matching endWrite() is the one before the next runFrame().
    //
    // Nothing else may touch M5.Display while this is outstanding; the boot-time
    // showMessage() calls are all done by then.
    if (drawThisFrame) {
        M5.Display.startWrite();
        M5.Display.pushImageDMA(SCREEN_X_OFFSET, 0, NES_WIDTH, NES_HEIGHT,
                                g_nes.ppu.framebuffer);
        pushOutstanding = true;
    }

    drainAudio(playbackRate);
    const int64_t pushEndUs = esp_timer_get_time();

#if PERF_LOG
    perfEmuUs += emuEndUs - emuStartUs;
    perfPushUs += pushEndUs - emuEndUs;
    perfJoinUs += joinEndUs - emuStartUs;
    if (drawThisFrame) perfEmuDrawUs += emuEndUs - joinEndUs;
    else perfEmuSkipUs += emuEndUs - joinEndUs;
    perfFrames++;
    if (drawThisFrame) perfDrawn++;
    const int avail = ringAvailable();
    if (avail < perfRingMin) perfRingMin = avail;
    if (avail > perfRingMax) perfRingMax = avail;
    const int64_t windowUs = pushEndUs - perfWindowUs;
    const bool perfWindowElapsed = windowUs >= 1000000;
    if (perfWindowElapsed) {
        const double fps = perfFrames * 1000000.0 / (double)windowUs;
        const double drawHz = perfDrawn * 1000000.0 / (double)windowUs;
        const uint32_t skipped = perfFrames - perfDrawn;
        Serial.printf("fps=%.1f div=%lu draw=%.1fHz emu=%lldus push=%lldus "
                      "join=%lldus emuD=%lldus emuS=%lldus "
                      "ring=%d..%d drop=%lu rate=%lu\n",
                      fps, (unsigned long)g_divisor, drawHz,
                      (long long)(perfEmuUs / perfFrames),
                      (long long)(perfPushUs / perfFrames),
                      (long long)(perfJoinUs / perfFrames),
                      (long long)(perfDrawn ? perfEmuDrawUs / perfDrawn : 0),
                      (long long)(skipped ? perfEmuSkipUs / skipped : 0),
                      perfRingMin, perfRingMax,
                      (unsigned long)g_ringDropped,
                      (unsigned long)playbackRate);
        // Only when something is actually unplugged, so the normal log stays as
        // it was and a fault is impossible to miss in a capture.
        const uint64_t pins = g_pinMask.load(std::memory_order_relaxed);
        const bool cartSeated = pins == PIN_MASK_ALL_OK;
        if (!cartSeated) Serial.printf("  pins=%016llx\n", (unsigned long long)pins);
        playbackRate = (uint32_t)(AUDIO_SAMPLES_PER_FRAME * fps);
        perfWindowUs = pushEndUs;
        perfFrames = 0;
        perfDrawn = 0;
        perfEmuUs = 0;
        perfPushUs = 0;
        perfJoinUs = 0;
        perfEmuDrawUs = 0;
        perfEmuSkipUs = 0;
        perfRingMin = AUDIO_RING_SAMPLES;
        perfRingMax = 0;
        g_ringDropped = 0;
    }
#endif

    nextFrameUs += FRAME_PERIOD_US;
    const int64_t remainingUs = nextFrameUs - esp_timer_get_time();
    const bool behindSchedule = remainingUs <= 0;
    adjustDivisor(behindSchedule);
    if (behindSchedule) {
        // Do not try to catch up: resync so a slow frame cannot cascade.
        nextFrameUs = esp_timer_get_time();
        return;
    }
    // delay() yields to the idle task (needed for the watchdog); the residual
    // sub-millisecond wait is spun out for frame-time accuracy.
    const int64_t delayMs = remainingUs / 1000;
    if (delayMs > 1) delay((uint32_t)(delayMs - 1));
    while (esp_timer_get_time() < nextFrameUs) {
    }
}

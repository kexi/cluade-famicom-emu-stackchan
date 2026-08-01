// M5Stack CoreS3 frontend for the NES core.
//
// Core 1 (Arduino loop) runs emulation, audio and display; core 0 runs a
// blocking UDP receive task that only publishes controller bits. Splitting
// this way keeps the network stack's latency out of the frame loop.

#include <M5Unified.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <atomic>
#include <new>

#include "../../core/nes.h"
#include "config.h"
#include "grove_input.h"
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
// Debug snapshot request: the flag plus where to send the answer. Latched the
// same way as the other controls so the snapshot is taken between frames, when
// the CPU state is coherent, rather than mid-instruction from the UDP task.
static std::atomic<bool> g_debugRequested{false};
static std::atomic<uint32_t> g_debugReplyIp{0};
static std::atomic<uint16_t> g_debugReplyPort{0};
static std::atomic<uint16_t> g_debugSeq{0};
static std::atomic<bool> g_debugWantWaves{false};
// millis() of the last wave request; the frame loop disarms capture once this
// goes stale. 0 = never asked.
static std::atomic<uint32_t> g_debugWaveAskedMs{0};
// A ROM received over UDP, staged in PSRAM by the UDP task and installed by the
// emulation loop at a frame boundary.
//
// Unlike the other latches this one guards a whole buffer rather than a single
// value, so it is release/acquire and not relaxed: the release store publishes
// every byte written into g_romBuf (plus g_romSize/g_romFlags) to whichever core
// observes the flag with an acquire load. Core 0 must not touch the buffer again
// until core 1 has cleared the flag, which is why core 1 clears it only after the
// install has completely finished.
static uint8_t* g_romBuf = nullptr;
static uint32_t g_romSize = 0;
static uint8_t g_romFlags = 0;
static std::atomic<bool> g_romApplyRequested{false};
// The UDP socket, shared so loop() can answer directly. lwIP's sendto is
// thread-safe, and replying from the emulation core avoids handing the snapshot
// buffer across tasks while it is being filled.
static int g_udpSock = -1;
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

// ---------------------------------------------------------------- ROM receive

// Transfer state, owned entirely by the UDP task. Kept at file scope only so the
// packet handler can be split out of udpTask's loop for readability.
static bool g_romActive = false;   // a BEGIN has been accepted and not yet finished
static uint16_t g_romSession = 0;
static uint32_t g_romExpectedSize = 0;
static uint32_t g_romExpectedCrc = 0;
static uint8_t g_romPendingFlags = 0;
static uint16_t g_romNextChunk = 0;   // the chunk index a DATA packet must carry
static uint32_t g_romReceived = 0;   // bytes staged so far
static uint32_t g_romLastRxMs = 0;   // for the stale-session takeover

static void sendRomAck(int sock, const sockaddr_in& to, uint8_t op, uint16_t session, uint16_t chunk, uint8_t status) {
    uint8_t ack[UDP_ROM_ACK_SIZE] = {};
    ack[0] = 'N';
    ack[1] = 'R';
    ack[2] = UDP_PROTOCOL_VERSION;
    ack[3] = op;
    ack[4] = session & 0xFF;
    ack[5] = session >> 8;
    ack[6] = chunk & 0xFF;
    ack[7] = chunk >> 8;
    ack[8] = status;
    // Where the sender should resume. Meaningful on a SEQ rejection; harmless
    // elsewhere, and always filling it keeps the layout fixed.
    ack[9] = g_romNextChunk & 0xFF;
    ack[10] = g_romNextChunk >> 8;
    ::sendto(sock, ack, sizeof(ack), 0, (const sockaddr*)&to, sizeof(to));
}

// Validate the staged image the same way nes::loadRom will, so a cart that cannot
// possibly load is rejected while the sender is still listening — rather than
// failing on core 1 where the only report would be a serial line.
static uint8_t checkStagedRom() {
    const bool magicOk = g_romBuf[0] == 'N' && g_romBuf[1] == 'E' && g_romBuf[2] == 'S' && g_romBuf[3] == 0x1A;
    if (!magicOk) return UDP_ROM_STATUS_BAD_HEADER;

    // Archaic iNES: bytes 12-15 should be zero, and when they are not (e.g.
    // "DiskDude!" garbage) flags7's upper nibble is not a mapper number. Mirrors
    // cartridge.cpp's dirtyHeader rule exactly — disagreeing would let a ROM pass
    // here and then fail to load.
    const bool dirtyHeader = g_romBuf[12] || g_romBuf[13] || g_romBuf[14] || g_romBuf[15];
    const int mapperNum = (g_romBuf[6] >> 4) | (dirtyHeader ? 0 : (g_romBuf[7] & 0xF0));
    const bool mapperSupported = mapperNum == 0 || mapperNum == 1 || mapperNum == 2 || mapperNum == 3 ||
                                 mapperNum == 4 || mapperNum == 24 || mapperNum == 26;
    if (!mapperSupported) return UDP_ROM_STATUS_UNSUPPORTED_MAPPER;
    return UDP_ROM_STATUS_OK;
}

static void handleRomPacket(int sock, const sockaddr_in& from, const uint8_t* packet, int received) {
    const uint16_t session = (uint16_t)(packet[4] | (packet[5] << 8));
    const uint8_t op = packet[6];

    // The staging buffer belongs to core 1 until it has installed the ROM. Taking
    // a new transfer now would overwrite the image out from under it.
    const bool applyPending = g_romApplyRequested.load(std::memory_order_acquire);
    if (applyPending) {
        sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_BUSY);
        return;
    }

    if (op == UDP_ROM_OP_BEGIN) {
        const bool beginShort = received < UDP_ROM_BEGIN_SIZE;
        if (beginShort) return;
        const uint32_t total = (uint32_t)packet[8] | ((uint32_t)packet[9] << 8) | ((uint32_t)packet[10] << 16) |
                               ((uint32_t)packet[11] << 24);

        // A retransmitted BEGIN (its ACK was lost) must not restart a transfer
        // that is already making progress, so only re-acknowledge it.
        const bool sameSession = g_romActive && session == g_romSession;
        if (sameSession) {
            g_romLastRxMs = millis();
            sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_OK);
            return;
        }
        const bool otherSessionAlive = g_romActive && (millis() - g_romLastRxMs) <= ROM_SESSION_TIMEOUT_MS;
        if (otherSessionAlive) {
            sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_BUSY);
            return;
        }
        const bool tooBig = total == 0 || total > ROM_MAX_SIZE;
        if (tooBig) {
            sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_TOO_BIG);
            return;
        }
        // Reserved once and never released: a buffer that comes and goes would
        // race core 1 and fragment PSRAM for nothing. 1MB against 8MB is cheap.
        const bool needBuffer = g_romBuf == nullptr;
        if (needBuffer) {
            g_romBuf = (uint8_t*)heap_caps_malloc(ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
        }
        if (!g_romBuf) {
            sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_ALLOC);
            return;
        }

        g_romActive = true;
        g_romSession = session;
        g_romExpectedSize = total;
        g_romExpectedCrc = (uint32_t)packet[12] | ((uint32_t)packet[13] << 8) | ((uint32_t)packet[14] << 16) |
                           ((uint32_t)packet[15] << 24);
        g_romPendingFlags = packet[7];
        g_romNextChunk = 0;
        g_romReceived = 0;
        g_romLastRxMs = millis();
        sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_OK);
        return;
    }

    const bool noSession = !g_romActive || session != g_romSession;
    if (noSession) {
        sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_NO_SESSION);
        return;
    }

    if (op == UDP_ROM_OP_ABORT) {
        g_romActive = false;
        sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_OK);
        return;
    }

    if (op == UDP_ROM_OP_DATA) {
        const bool dataShort = received < UDP_ROM_DATA_HEADER;
        if (dataShort) return;
        const uint16_t chunk = (uint16_t)(packet[8] | (packet[9] << 8));
        const uint16_t len = (uint16_t)(packet[10] | (packet[11] << 8));
        // Trust the datagram's own length over the claimed one, or a lying header
        // would read past what actually arrived.
        const bool lengthLies = (int)len != received - UDP_ROM_DATA_HEADER;
        if (lengthLies) return;

        // The ACK for the previous chunk was lost and the sender repeated it. The
        // bytes are already staged, so re-acknowledge without copying — copying
        // would advance nothing but could only ever corrupt.
        const bool duplicate = g_romNextChunk > 0 && chunk == (uint16_t)(g_romNextChunk - 1);
        if (duplicate) {
            g_romLastRxMs = millis();
            sendRomAck(sock, from, op, session, chunk, UDP_ROM_STATUS_OK);
            return;
        }
        const bool outOfOrder = chunk != g_romNextChunk;
        if (outOfOrder) {
            g_romLastRxMs = millis();
            sendRomAck(sock, from, op, session, chunk, UDP_ROM_STATUS_SEQ);
            return;
        }
        const bool overflows = g_romReceived + len > g_romExpectedSize;
        if (overflows) {
            g_romActive = false;
            sendRomAck(sock, from, op, session, chunk, UDP_ROM_STATUS_SIZE_MISMATCH);
            return;
        }

        memcpy(g_romBuf + g_romReceived, packet + UDP_ROM_DATA_HEADER, len);
        g_romReceived += len;
        g_romNextChunk++;
        g_romLastRxMs = millis();
        sendRomAck(sock, from, op, session, chunk, UDP_ROM_STATUS_OK);
        return;
    }

    if (op == UDP_ROM_OP_END) {
        const bool sizeMismatch = g_romReceived != g_romExpectedSize;
        if (sizeMismatch) {
            g_romActive = false;
            sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_SIZE_MISMATCH);
            return;
        }
        // esp_rom_crc32_le brackets its own computation with '~' (see
        // esp_rom_crc.h), so seeding it with 0 yields exactly zlib's CRC-32 —
        // init 0xFFFFFFFF, reflected, final xor 0xFFFFFFFF. Verified equal to
        // Python's zlib.crc32, which is what the sender uses.
        const uint32_t crc = esp_rom_crc32_le(0, g_romBuf, g_romReceived);
        const bool crcMismatch = crc != g_romExpectedCrc;
        if (crcMismatch) {
            g_romActive = false;
            sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_CRC);
            return;
        }
        const uint8_t headerStatus = checkStagedRom();
        const bool unloadable = headerStatus != UDP_ROM_STATUS_OK;
        if (unloadable) {
            g_romActive = false;
            sendRomAck(sock, from, op, session, 0, headerStatus);
            return;
        }

        g_romSize = g_romReceived;
        g_romFlags = g_romPendingFlags;
        g_romActive = false;
        // Publishes the buffer and the two plain globals above to core 1.
        g_romApplyRequested.store(true, std::memory_order_release);
        sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_OK);
        return;
    }
}

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

    g_udpSock = sock;

    // Static, not on the stack: a ROM chunk needs 1412 bytes and this task only
    // gets 4096 in total.
    static uint8_t packet[UDP_ROM_DATA_HEADER + UDP_ROM_CHUNK];
    for (;;) {
        // The sender address is captured so a debug query can be answered; the
        // other packet types ignore it.
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        const int received = ::recvfrom(sock, packet, sizeof(packet), 0, (sockaddr*)&from, &fromLen);
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
        if (type == UDP_TYPE_DEBUG) {
            g_debugReplyIp.store(from.sin_addr.s_addr, std::memory_order_relaxed);
            g_debugReplyPort.store(from.sin_port, std::memory_order_relaxed);
            g_debugSeq.store((uint16_t)(packet[4] | (packet[5] << 8)), std::memory_order_relaxed);
            const bool wantWaves = packet[6] & UDP_DEBUG_FLAG_WAVES;
            g_debugWantWaves.store(wantWaves, std::memory_order_relaxed);
            if (wantWaves) g_debugWaveAskedMs.store(millis(), std::memory_order_relaxed);
            g_debugRequested.store(true, std::memory_order_relaxed);
            continue;
        }
        if (type == UDP_TYPE_ROM) {
            handleRomPacket(sock, from, packet, received);
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
    Serial.printf("WIFI: connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    return true;
}

static void joinBand();

static void haltWithError(const char* text) {
    // A band may still be in flight: applyRomRequest() can reach here mid-frame,
    // with a startWrite() open and DMA armed against the framebuffer. Drawing on
    // top of that would corrupt the error screen and leave the panel parked on an
    // open transaction, i.e. the one message the user needs would be the one
    // message they cannot read.
    joinBand();
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
    // M5Unified leaves the speaker task unpinned at priority 2. Unpinned it is
    // free to land on core 1, where it outranks the Arduino loop task (priority
    // 1) and preempts emulation mid-frame; raising its priority alone would only
    // make that worse. Pinning it to core 0 is what actually separates the two,
    // and priority 4 there sits above the Grove poller (3) so an I2C read cannot
    // delay the I2S refill, and below the UDP task (5) which blocks on recv and
    // therefore never holds the core.
    speakerCfg.task_pinned_core = 0;
    speakerCfg.task_priority = 4;
    M5.Speaker.config(speakerCfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(SPEAKER_VOLUME);

    // Before WiFi: the Grove controllers work regardless of network state.
    groveInputInit();

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
        M5.Speaker.playRaw(g_chunk[g_chunkIndex], AUDIO_CHUNK_SAMPLES, AUDIO_SAMPLE_RATE, false, 1, SPEAKER_CHANNEL);
        g_chunkIndex = (g_chunkIndex + 1) % AUDIO_CHUNK_SLOTS;
    }

    // NB: no startWrite() here. Holding the bus open across the whole run leaves
    // the panel's address window owned by whatever ran last, so pushes land at
    // the wrong offset; pushImageDMA sets the window itself per call.
    Serial.printf("DISPLAY: %dx%d rot=%d push=(%d,%d,%d,%d)\n", M5.Display.width(), M5.Display.height(),
                  M5.Display.getRotation(), SCREEN_X_OFFSET, 0, NES_WIDTH, NES_HEIGHT);

    M5.Display.fillScreen(TFT_BLACK);
    if (!g_wifiConnected) {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        showMessage("WiFi: failed", 228, 1);
    }
}

// ------------------------------------------------------------------- loop

// Buttons that live on the CoreS3 itself: the three touch zones below the
// screen. Start/Select have no home on the Grove units (the joystick's centre
// press doubles as Start, but Select needs somewhere), and a long-press on the
// right zone is the RESET button for standalone play.
static uint8_t touchButtonBits() {
    uint8_t bits = 0;
    if (M5.BtnA.isPressed()) bits |= NES_BTN_SELECT;
    if (M5.BtnB.isPressed()) bits |= NES_BTN_START;
    if (M5.BtnC.wasHold()) g_resetRequested.store(true, std::memory_order_relaxed);
    return bits;
}

// Pad 1 is the OR of every local source (Grove units, touch zones) and the UDP
// sender. Only the UDP bits are subject to the staleness timeout — a physical
// button that is held down should stay down.
static void applyInput() {
    const uint32_t sinceRx = millis() - g_lastRxMs.load(std::memory_order_relaxed);
    const bool udpStale = sinceRx > INPUT_TIMEOUT_MS;
    const uint8_t udp0 = udpStale ? 0 : g_padBits[0].load(std::memory_order_relaxed);
    const uint8_t udp1 = udpStale ? 0 : g_padBits[1].load(std::memory_order_relaxed);
    g_nes.pad[0].setButtons(udp0 | groveInputBits() | touchButtonBits());
    g_nes.pad[1].setButtons(udp1);
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

// Install a ROM that arrived over UDP, if one is waiting.
//
// Runs at a frame boundary for the same reason as the reset above: the UDP task
// stages the image mid-frame, and swapping the mapper out from under a running
// instruction would fault. The acquire load pairs with the UDP task's release
// store, so every staged byte is visible here.
static void applyRomRequest() {
    const bool requested = g_romApplyRequested.load(std::memory_order_acquire);
    if (!requested) return;

    const bool wantSwap = (g_romFlags & ROM_FLAG_SWAP) != 0;
    bool ok = false;
    // The core allocates PRG/CHR through InternalRamAllocator, which throws when
    // internal SRAM runs out. A ROM the device cannot fit must leave the current
    // game running, not kill the console.
    try {
        if (wantSwap) {
            // No reset: keep the old cart until the new one is fully built, so a
            // failed load leaves the running game untouched. The two therefore
            // coexist briefly and the new PRG may land in PSRAM — accepted, since
            // continuing to play matters more than that cart's speed.
            auto m = nes::loadRom(g_romBuf, g_romSize);
            if (m) {
                g_nes.mapper = std::move(m);
                g_nes.refreshMapperCaps();
                // Not optional here, though the web build's nes_swap_rom omits it:
                // there chrWindow_ is a constexpr nullptr, while this build caches
                // a real pointer into the old cart's CHR — which std::move just
                // freed. Skipping this leaves the PPU reading dangling memory.
                g_nes.ppu.refreshChrWindow();
                // The PRG windows point into the old cart the same way, and the
                // CPU reads them far more often than the PPU reads CHR.
                g_nes.refreshPrgWindows();
                ok = true;
            }
        } else {
            // Drop the old cart first so its PRG/CHR return to internal SRAM before
            // the new one asks for any. NES::loadRom assigns (mapper = loadRom(...)),
            // which builds the replacement while the old one is still held, and on
            // this part that peak is enough to exhaust SRAM on a large ROM.
            g_nes.mapper.reset();
            ok = g_nes.loadRom(g_romBuf, g_romSize);   // powerOn + refreshChrWindow included
        }
    } catch (const std::bad_alloc&) {
        ok = false;
    }

    // The powerOn path already discarded the old cart, so a failure here leaves
    // the console with no mapper at all — fall back to the image built into the
    // firmware rather than run on nothing.
    const bool cartMissing = !g_nes.mapper;
    if (cartMissing) {
        const size_t embeddedSize = (size_t)(rom_end - rom_start);
        bool restored = false;
        try {
            restored = g_nes.loadRom(rom_start, embeddedSize);
        } catch (const std::bad_alloc&) {
            restored = false;
        }
        if (!restored) haltWithError("ROM load failed");
    }

    if (ok) Serial.printf("ROM: applied %u bytes%s\n", (unsigned)g_romSize, wantSwap ? " (no reset)" : "");
    else Serial.printf("ROM: failed %u bytes\n", (unsigned)g_romSize);

    // Cleared last: until this store the UDP task treats the buffer as ours and
    // refuses new transfers. Clearing it earlier would let a BEGIN overwrite the
    // image we are still reading.
    g_romApplyRequested.store(false, std::memory_order_release);
}

// Answer a debug snapshot request, if one is pending.
//
// Runs at a frame boundary so the CPU state it reports is coherent — sampling
// from the UDP task would catch the emulator mid-instruction. The reply is sent
// from here rather than handed back to that task so the buffer is never shared
// while it is being filled.
// Arm or disarm the APU's per-sample scope capture.
//
// Evaluated every frame, not only when a request arrives, or it would never turn
// back off. Split from the reply below because arming must happen *before*
// runFrame() (so the frame produces samples) while the reply must happen after
// it (so sampleCount still describes those samples — enqueueAudio zeroes it).
static void updateWaveCapture() {
    const uint32_t asked = g_debugWaveAskedMs.load(std::memory_order_relaxed);
    const bool watching = asked != 0 && (millis() - asked) < DEBUG_WAVE_HOLD_MS;
    if (g_nes.apu.waveCapture == watching) return;
    g_nes.apu.waveCapture = watching;
    Serial.printf("DBG: wave capture %s\n", watching ? "on" : "off");
}

static void applyDebugRequest() {
    const bool requested = g_debugRequested.exchange(false, std::memory_order_relaxed);
    if (!requested) return;
    if (g_udpSock < 0) return;

    // Static, not stack: ~3.8KB would be a large chunk of the Arduino loop task's
    // stack, and it is only touched here.
    static uint8_t snapshot[nes::NES::DEBUG_SNAPSHOT_MAX];
    // Only include waves once capture has actually been running: the first reply
    // after arming would otherwise carry a stale or empty buffer.
    const bool withWaves = g_debugWantWaves.load(std::memory_order_relaxed) && g_nes.apu.waveCapture;
    const size_t total = g_nes.buildDebugSnapshot(snapshot, withWaves);

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = g_debugReplyIp.load(std::memory_order_relaxed);
    to.sin_port = g_debugReplyPort.load(std::memory_order_relaxed);
    const uint16_t seq = g_debugSeq.load(std::memory_order_relaxed);

    // Derived from the payload actually built, so a wave-bearing reply simply
    // uses more parts; the receiver reads the count out of the header.
    const uint8_t nparts = (uint8_t)((total + UDP_DEBUG_CHUNK - 1) / UDP_DEBUG_CHUNK);

    uint8_t datagram[UDP_DEBUG_HEADER + UDP_DEBUG_CHUNK];
    for (int part = 0; part < nparts; part++) {
        const size_t offset = (size_t)part * UDP_DEBUG_CHUNK;
        if (offset >= total) break;
        size_t len = total - offset;
        if (len > UDP_DEBUG_CHUNK) len = UDP_DEBUG_CHUNK;
        datagram[0] = 'N';
        datagram[1] = 'D';
        datagram[2] = UDP_PROTOCOL_VERSION;
        datagram[3] = (uint8_t)part;
        datagram[4] = nparts;
        datagram[5] = seq & 0xFF;
        datagram[6] = seq >> 8;
        memcpy(datagram + UDP_DEBUG_HEADER, snapshot + offset, len);
        ::sendto(g_udpSock, datagram, UDP_DEBUG_HEADER + len, 0, (sockaddr*)&to, sizeof(to));
    }

    // Once only: at 5Hz this would otherwise bury every other serial line.
    static bool announced = false;
    if (!announced) {
        announced = true;
        Serial.printf("DBG: snapshot to %s (%u bytes)\n", IPAddress(to.sin_addr.s_addr).toString().c_str(),
                      (unsigned)total);
    }
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
//
// Returns how many samples were written, which is what the rate servo in loop()
// measures the production rate from.
// DC blocker state, carried across calls. APU::mix() swings 0..~0.45, i.e.
// entirely positive with a large standing DC component; feeding that to the
// speaker wastes half the headroom and turns every queue start/stop into an
// audible step.
static float g_dcX1 = 0.0f, g_dcY1 = 0.0f;

// The filter-and-store loop. Muted is a template parameter, not a runtime test,
// so the mute check stays out of the per-sample path while both variants keep a
// single definition of the filter — the muted one still advances the state, just
// with x fixed at zero, so a cart fault does not leave a discontinuity behind.
template <bool Muted> static void enqueueSamples(const float* src, int count) {
    // Loop-carried state in locals: as globals the compiler has to reload them
    // across the store to g_ring, which it cannot prove does not alias them.
    float x1 = g_dcX1, y1 = g_dcY1;
    int w = g_ringWrite, r = g_ringRead;
    constexpr int ringMask = AUDIO_RING_MASK;
    for (int i = 0; i < count; i++) {
        const float x = Muted ? 0.0f : src[i];
        const float y = x - x1 + AUDIO_DC_POLE * y1;
        x1 = x;
        y1 = y;

        // Deliberately still two multiplies with the clamp between them. Folding
        // them into one scale factor is algebraically the same but rounds
        // differently, and was measured to shift ~0.01% of samples by 1 LSB — a
        // change to the output for no measurable gain, since the multiply is not
        // what this loop spends its time on.
        float s = y * AUDIO_HEADROOM;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        g_ring[w] = (int16_t)(s * 32767.0f);
        w = (w + 1) & ringMask;
        const bool ringFull = w == r;
        if (ringFull) {   // drop the oldest sample rather than the newest
            r = (r + 1) & ringMask;
            g_ringDropped++;
        }
    }
    g_dcX1 = x1;
    g_dcY1 = y1;
    g_ringWrite = w;
    g_ringRead = r;
}

static int enqueueAudio() {
    const int produced = g_nes.apu.sampleCount;
    g_nes.apu.sampleCount = 0;
    // Famicom audio loops out through the cartridge (pins 45/46), so a bad
    // contact there silences the console — same semantics as the web build's
    // muteIfCartAudioBroken. Still run the loop rather than skipping it, so the
    // ring keeps being fed and playback stays continuous instead of underrunning
    // into clicks.
    const bool cartAudioBroken = !g_nes.soundOk;
    if (cartAudioBroken) enqueueSamples<true>(nullptr, produced);
    else enqueueSamples<false>(g_nes.apu.sampleBuf, produced);
    return produced;
}

static int ringAvailable() { return (g_ringWrite - g_ringRead) & AUDIO_RING_MASK; }

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
        // Two memcpy-able runs at most: the chunk either sits contiguously in the
        // ring or straddles the wrap once. Copying run-wise rather than sample-wise
        // drops the per-sample index wrap entirely.
        const int firstRun = AUDIO_RING_SAMPLES - g_ringRead;
        const int headLen = firstRun < AUDIO_CHUNK_SAMPLES ? firstRun : AUDIO_CHUNK_SAMPLES;
        memcpy(chunk, g_ring + g_ringRead, (size_t)headLen * sizeof(int16_t));
        const int tailLen = AUDIO_CHUNK_SAMPLES - headLen;
        if (tailLen > 0) memcpy(chunk + headLen, g_ring, (size_t)tailLen * sizeof(int16_t));
        g_ringRead = (g_ringRead + AUDIO_CHUNK_SAMPLES) & AUDIO_RING_MASK;
        const bool queued = M5.Speaker.playRaw(chunk, AUDIO_CHUNK_SAMPLES, rate, false, 1, SPEAKER_CHANNEL);
        if (!queued) {
            // Queue is full: rewind so the samples are not lost, and stop.
            g_ringRead = (g_ringRead - AUDIO_CHUNK_SAMPLES) & AUDIO_RING_MASK;
            return;
        }
        g_chunkIndex = (g_chunkIndex + 1) % AUDIO_CHUNK_SLOTS;
    }
}

// Display refresh divisor: the PPU only paints every Nth frame (see
// PPU::renderThisFrame), and the frames in between are what the band transfers
// hide under. Emulation keeps its real-time 60Hz and only the panel's refresh
// rate gives way.
//
// A controller used to float this between MIN and MAX from how often the loop
// missed its deadline. It is gone: the segmented push made the draw frame cheap
// enough that MIN and MAX converged on the same value, leaving nothing to trade,
// and the divisor is now pinned to the band count by static_assert. Kept as a
// variable rather than folded into the constant so the PERF_LOG line still has
// something to report and a future controller has somewhere to write.
static uint32_t g_divisor = DISPLAY_DIVISOR_INITIAL;

// "A band was kicked and its transaction is still open." File scope rather than
// a local of loop(): any path that draws to the panel has to be able to close
// that transaction first, and haltWithError() is reachable from the ROM-swap
// handler in the middle of a frame.
static bool g_pushOutstanding = false;

// Close the open band transaction, if there is one, and wait for its DMA.
// Panel_LCD::end_transaction() waits on the bus, so this both joins the transfer
// and releases the SPI lock; waitDMA() alone would leave the lock held.
static void joinBand() {
    if (!g_pushOutstanding) return;
    M5.Display.endWrite();
    g_pushOutstanding = false;
}

// Kick one horizontal band of the framebuffer at the panel and return.
//
// Sized so the whole band fits a single SPI transaction: the peripheral's
// transaction-length register (SPI_MS_DATA_BITLEN, 18 bits) caps one transfer at
// 32768 bytes, and LGFX covers a longer request by re-arming the peripheral in a
// loop while spinning on SPI_USR (Bus_SPI.cpp writeBytes, the `if (length -= len)`
// block). That spin is why a whole-frame push cost most of its ~24.6ms of wire
// time in CPU time despite being a "DMA" call. A band never enters that loop, so
// the call returns as soon as the DMA descriptors are armed.
//
// The caller owns the matching endWrite(): startWrite() here leaves the
// transaction open on purpose so the transfer stays in flight (IPanel refcounts
// the nesting, demoting pushImageDMA's inner endWrite() to a decrement).
static void pushBand(int band) {
    const int bandY = band * DISPLAY_DMA_ROWS;
    const int rowsLeft = NES_HEIGHT - bandY;
    const int bandRows = rowsLeft < DISPLAY_DMA_ROWS ? rowsLeft : DISPLAY_DMA_ROWS;
    M5.Display.startWrite();
    M5.Display.pushImageDMA(SCREEN_X_OFFSET, bandY, NES_WIDTH, bandRows,
                            g_nes.ppu.framebuffer + (size_t)bandY * NES_WIDTH);
}

// Servo the playback rate to what the emulator actually produces.
//
// Two terms, because neither is sufficient alone. The smoothed production rate
// (produced samples over real elapsed time) tracks emulation speed but says
// nothing about the accumulated error, so on its own the ring slowly walks to
// empty or full. The ring-depth term is that missing integral: the level is the
// running sum of every past mismatch, so pulling it back to AUDIO_RING_TARGET
// is what keeps playback continuous across a speed change.
//
// Output is slew-limited, so what reaches the ear is a drift rather than the
// per-second steps the old fps-derived rate produced.
static uint32_t updatePlaybackRate(uint32_t current, int produced, int64_t frameUs, int ringAvail) {
    // Numerator and denominator are smoothed separately, and the ratio is taken
    // from the two averages. Smoothing the per-frame ratio produced/frameUs
    // instead reads high: E[p/t] > E[p]/E[t] whenever t varies (Jensen — the
    // harmonic-mean bias), and t varies a lot here because the display divisor
    // makes drawn frames ~34ms and skipped ones ~21ms. Measured on hardware at
    // 33fps that bias was ~18% (28.6kHz reported against a true ~24.2kHz), which
    // played the ring empty and kept it there.
    static float avgProduced = (float)(AUDIO_SAMPLE_RATE * FRAME_PERIOD_US / 1000000.0);
    static float avgFrameUs = (float)FRAME_PERIOD_US;
    static bool snapped = true;
    // Frames still to run before the slew limit engages.
    static int warmupFrames = AUDIO_RATE_WARMUP_FRAMES;

    const bool frameUsable = frameUs > 0 && produced > 0;
    if (frameUsable) {
        avgProduced += AUDIO_RATE_EWMA_ALPHA * ((float)produced - avgProduced);
        avgFrameUs += AUDIO_RATE_EWMA_ALPHA * ((float)frameUs - avgFrameUs);
    }
    const float measuredRate = avgProduced * 1000000.0f / avgFrameUs;

    float correction = (float)(ringAvail - AUDIO_RING_TARGET) * AUDIO_RATE_FEEDBACK_GAIN;
    const float correctionMax = measuredRate * AUDIO_RATE_FEEDBACK_MAX;
    if (correction > correctionMax) correction = correctionMax;
    if (correction < -correctionMax) correction = -correctionMax;
    float target = measuredRate + correction;

    // 44.1kHz snap. Hysteresis on the release side: without it the rate would
    // flip between snapped and free every time the servo grazes the threshold,
    // which is exactly the audible dither the snap exists to remove.
    const float nominal = (float)AUDIO_SAMPLE_RATE;
    const float offset = (target - nominal) / nominal;
    const float deviation = offset < 0 ? -offset : offset;
    const bool withinSnap = snapped ? deviation <= AUDIO_RATE_SNAP_EXIT : deviation <= AUDIO_RATE_SNAP_ENTER;
    snapped = withinSnap;
    if (withinSnap) {
        // Not a hard pin to nominal: the emulator paces at 60.10Hz wall clock and
        // the I2S PLL does not hit 44100 exactly, so at a hard 44100 the ring
        // drifts a few samples per second one way until it drops or starves —
        // measured +5/s on hardware, overflow in roughly a quarter hour. The ring
        // term is kept, clamped far below the snap window so what remains is a
        // slow inaudible trim around nominal rather than the free-running servo.
        const float trimMax = nominal * AUDIO_RATE_SNAP_TRIM_MAX;
        float trim = correction;
        if (trim > trimMax) trim = trimMax;
        if (trim < -trimMax) trim = -trimMax;
        target = nominal + trim;
    }

    // Warm-up: follow the target directly for the first couple of seconds. The
    // averages start at the 60fps ideal, so on a ROM that only reaches 33fps the
    // rate has to travel 44.1k -> ~24k; at 0.25% per frame that is ~7s of audible
    // fast-forward before it arrives. The slew limit exists to hide steady-state
    // corrections, and there is no steady state yet to hide.
    const bool warmingUp = warmupFrames > 0;
    if (warmingUp) {
        warmupFrames--;
        return (uint32_t)(target + 0.5f);
    }

    const float slew = (float)current * AUDIO_RATE_SLEW_MAX;
    const float low = (float)current - slew;
    const float high = (float)current + slew;
    if (target < low) target = low;
    if (target > high) target = high;
    return (uint32_t)(target + 0.5f);
}

void loop() {
    static int64_t nextFrameUs = esp_timer_get_time();
    // Wall-clock length of the previous loop iteration, measured at the top so
    // it naturally includes the pacing sleep and the early-return taken when a
    // frame runs behind schedule — both of which are part of how long the frame
    // really took, and so part of the production rate.
    static int64_t lastLoopUs = esp_timer_get_time();
    const int64_t loopStartUs = esp_timer_get_time();
    const int64_t frameUs = loopStartUs - lastLoopUs;
    lastLoopUs = loopStartUs;
#if PERF_LOG
    static int64_t perfWindowUs = esp_timer_get_time();
    static uint32_t perfFrames = 0, perfDrawn = 0;
    static int64_t perfEmuUs = 0;
    // Split by drawn/skipped: the gap between them is the cost of the draw path,
    // which is what the display divisor actually buys back.
    static int64_t perfEmuDrawUs = 0, perfEmuSkipUs = 0, perfJoinUs = 0;
    // The push window lumps audio, the DMA kick and the input poll together;
    // split them so a regression in any one of them has a name in the log.
    //
    // dma= is the cost of kicking one band (descriptor setup, no wire time) and
    // is averaged over every frame, since a band goes out on nearly all of them.
    // The wire time itself lands in join= when it is not fully hidden, and the
    // leftover-band flush at a divisor below DISPLAY_DMA_SEGMENTS lands there too.
    static int64_t perfUpdUs = 0, perfAudioUs = 0, perfDmaUs = 0, perfDrain2Us = 0;
    static int perfRingMin = AUDIO_RING_SAMPLES, perfRingMax = 0;
#endif

    // Retimed every frame by updatePlaybackRate(): while the emulator runs below
    // 60fps it also produces samples below 44.1kHz, so playing them at the
    // nominal rate would drain the ring faster than it fills.
    static uint32_t playbackRate = AUDIO_SAMPLE_RATE;

    // Counts every loop iteration; drives the display divisor below, so it is
    // incremented exactly once, here.
    static uint32_t frameIndex = 0;
    const uint32_t thisFrame = frameIndex++;

    // Every frame, deliberately. Polling this at half rate to save ~0.2ms was
    // tried and reverted: touchButtonBits() reads BtnA/BtnB with isPressed(),
    // which is a level test against state that only advances inside update(), so
    // any tap shorter than the polling interval is not delayed but lost outright
    // — and a 33ms window drops taps a player would consider perfectly normal.
    M5.update();
    applyInput();
    applyPinChanges();
    applyResetRequest();
    applyRomRequest();
    applyVolumeRequest();
    updateWaveCapture();

    // A picture is transferred as DISPLAY_DMA_SEGMENTS bands, one per loop
    // iteration — the draw frame ships band 0 and the skipped frames that follow
    // ship the rest, so a whole picture still lands within one divisor cycle and
    // the panel refresh rate is unchanged.
    //
    // Only the frame that starts a picture may repaint the framebuffer. The
    // skipped frames in between leave it untouched (renderThisFrame is false),
    // which is what keeps the rows behind the in-flight bands stable; repainting
    // on every frame would let the emulator overwrite rows the DMA engine has not
    // read yet and tear the picture.
    static int bandIndex = 0;
    const bool drawThisFrame = (thisFrame % g_divisor) == 0;
    // Bands still owed from the previous picture. A static_assert ties the
    // divisor floor to the segment count, so at the configured divisor this is
    // always zero on a draw frame; the flush below is the defence for a divisor
    // that does not satisfy that relation.
    const bool pictureInFlight = bandIndex != 0;
    g_nes.ppu.renderThisFrame = drawThisFrame;

    const int64_t emuStartUs = esp_timer_get_time();
    // A draw frame can arrive while bands are still owed. Finish the old picture
    // first: runFrame() is about to repaint, so those rows would otherwise ship
    // as a mix of two pictures.
    const bool mustFlushBeforeRepaint = drawThisFrame && pictureInFlight;
    if (mustFlushBeforeRepaint) {
        joinBand();
        while (bandIndex != 0) {
            pushBand(bandIndex);
            bandIndex = (bandIndex + 1) % DISPLAY_DMA_SEGMENTS;
            M5.Display.endWrite();
        }
    }

    // Guard the repaint-versus-last-band race. The final band is normally still
    // in flight here and that is fine, because the repaint reaches its rows far
    // later than the wire does — but only while emulation stays slower than
    // DISPLAY_REPAINT_GUARD_MS. Once it is not, the transfer has to be joined
    // before anything repaints over it.
    //
    // Tested against the smoothed draw-frame emulation time rather than the last
    // one, so a single quick frame cannot arm a stall; the EWMA is updated after
    // runFrame() below. Independent of PERF_LOG, which is a diagnostic and may be
    // compiled out.
    // Seeded from the first draw frame rather than a large constant. A large seed
    // looks conservative — the guard cannot fire until the average has decayed
    // into range — but that decay takes ~100 draw frames, i.e. ten seconds during
    // which a fast build would be running completely unguarded.
    static float emuDrawMs = 0.0f;
    static bool emuDrawSeeded = false;
    const bool repaintRacesBand = drawThisFrame && emuDrawSeeded && emuDrawMs < DISPLAY_REPAINT_GUARD_MS;
    if (repaintRacesBand) joinBand();
    const int64_t flushEndUs = esp_timer_get_time();
    g_nes.runFrame();
    const int64_t emuEndUs = esp_timer_get_time();

    // Feed the guard above. Only draw frames count: they are the ones that
    // repaint, and a skipped frame is much cheaper, so mixing the two in would
    // understate the writer's pace and arm the guard needlessly.
    if (drawThisFrame) {
        const float drawMs = (float)(emuEndUs - flushEndUs) / 1000.0f;
        // The first sample is taken whole: there is no prior average to blend it
        // into, and starting at the real value is what keeps the guard live from
        // the second picture onward instead of after a decay.
        emuDrawMs = emuDrawSeeded ? emuDrawMs + DISPLAY_EMU_EWMA_ALPHA * (drawMs - emuDrawMs) : drawMs;
        emuDrawSeeded = true;
    }

    // Answered here, not with the other applyXxx handlers: the scope rows are
    // decimated from apu.sampleCount, which describes the frame that just ran and
    // which enqueueAudio() below resets to zero.
    applyDebugRequest();

    const int produced = enqueueAudio();
    // Read before draining: the servo wants the level the emulator just left
    // behind, not what is left after the speaker has taken its chunks.
    playbackRate = updatePlaybackRate(playbackRate, produced, frameUs, ringAvailable());
    drainAudio(playbackRate);
    const int64_t audioEndUs = esp_timer_get_time();

    // One band per loop iteration, draw frame or not. At divisor 4 the draw frame
    // kicks band 0 and the three skipped frames that follow kick bands 1-3, so a
    // picture completes within one divisor cycle — but the CPU only ever pays the
    // kick.
    //
    // The previous band is joined here, immediately before the next kick, rather
    // than at the top of the loop. Joining at the top left only ~0.4ms between
    // kick and join (the input poll), so most of the band's wire time was still
    // owed and showed up as a multi-millisecond join=. Joining here instead puts a
    // whole loop iteration (~30ms at 33fps, ~16.6ms even at 60fps) between the
    // two, so the wire has long since drained and the join costs nothing.
    //
    // Why this is safe even though it leaves the last band in flight across a
    // repaint: on a draw frame the previous iteration kicked the final band (rows
    // 180-239 at DISPLAY_DMA_ROWS=60), and runFrame() starts repainting ~0.5ms
    // later. The writer and the reader are separated in both space and time.
    // renderScanline writes strictly top-to-bottom (PPU::step draws line N at dot
    // 256), so row 180 is not touched until 180/262 of the way through emulation
    // — at the measured emuD=19.7ms that is kick+13.5ms, while the band's 60 rows
    // finish on the wire at kick+6.15ms. Margin ~7.4ms.
    //
    // That margin shrinks as emulation gets faster, because the writer speeds up
    // while the reader is fixed at the SPI clock. Break-even is emuD = 8.2ms; see
    // the derivation at DISPLAY_REPAINT_GUARD_MS, which is set to twice it. This
    // is not left to a comment: the guard before runFrame() joins the band
    // outright once the smoothed draw-frame time falls under that threshold.
    //
    // Nothing else may touch M5.Display while a band is outstanding; the
    // boot-time showMessage() calls are all done by then, and haltWithError()
    // joins first.
    joinBand();
    const int64_t joinEndUs = esp_timer_get_time();

    const bool hasBandToPush = drawThisFrame || pictureInFlight;
    if (hasBandToPush) {
        pushBand(bandIndex);
        g_pushOutstanding = true;
        bandIndex = (bandIndex + 1) % DISPLAY_DMA_SEGMENTS;
    }
    const int64_t dmaEndUs = esp_timer_get_time();

    drainAudio(playbackRate);
    const int64_t pushEndUs = esp_timer_get_time();

#if PERF_LOG
    perfEmuUs += emuEndUs - emuStartUs;
    // join= is now the wait before the next kick, plus the divisor-transition
    // flush before runFrame() when that fires at all.
    perfJoinUs += (joinEndUs - audioEndUs) + (flushEndUs - emuStartUs);
    perfUpdUs += emuStartUs - loopStartUs;
    perfAudioUs += audioEndUs - emuEndUs;
    perfDmaUs += dmaEndUs - joinEndUs;
    perfDrain2Us += pushEndUs - dmaEndUs;
    // Split on whether the PPU actually painted, which is what the divisor buys
    // back — a band frame that only kicks DMA costs the same as a skipped one.
    if (drawThisFrame) perfEmuDrawUs += emuEndUs - flushEndUs;
    else perfEmuSkipUs += emuEndUs - flushEndUs;
    perfFrames++;
    // Counts pictures, not band kicks, so draw= stays the panel's refresh rate.
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
        // push= is gone: it was the sum of aud/join/dma/dr2, which are all now
        // reported individually on the second line.
        Serial.printf("fps=%.1f div=%lu draw=%.1fHz emu=%lldus "
                      "join=%lldus emuD=%lldus emuS=%lldus "
                      "ring=%d..%d drop=%lu rate=%lu\n",
                      fps, (unsigned long)g_divisor, drawHz, (long long)(perfEmuUs / perfFrames),
                      (long long)(perfJoinUs / perfFrames), (long long)(perfDrawn ? perfEmuDrawUs / perfDrawn : 0),
                      (long long)(skipped ? perfEmuSkipUs / skipped : 0), perfRingMin, perfRingMax,
                      (unsigned long)g_ringDropped, (unsigned long)playbackRate);
        Serial.printf("  upd=%lldus aud=%lldus dma=%lldus dr2=%lldus\n", (long long)(perfUpdUs / perfFrames),
                      (long long)(perfAudioUs / perfFrames), (long long)(perfDmaUs / perfFrames),
                      (long long)(perfDrain2Us / perfFrames));
        // Only when something is actually unplugged, so the normal log stays as
        // it was and a fault is impossible to miss in a capture.
        const uint64_t pins = g_pinMask.load(std::memory_order_relaxed);
        const bool cartSeated = pins == PIN_MASK_ALL_OK;
#ifdef NES_PROFILE
        // Per-frame averages, in us. cpu is the remainder rather than its own
        // counter: instrumenting the CPU core would mean a CCOUNT read per
        // instruction, which costs more than the thing being measured.
        const uint64_t apuUs = g_nes.profApuCycles / CPU_CYCLES_PER_US;
        const uint64_t ppuUs = g_nes.profPpuCycles / CPU_CYCLES_PER_US;
        const int64_t cpuUs = perfEmuUs - (int64_t)apuUs - (int64_t)ppuUs;
        Serial.printf("  apu=%lluus ppu=%lluus cpu=%lldus\n", (unsigned long long)(apuUs / perfFrames),
                      (unsigned long long)(ppuUs / perfFrames), (long long)(cpuUs / (int64_t)perfFrames));
        g_nes.profApuCycles = 0;
        g_nes.profPpuCycles = 0;
#endif
        if (!cartSeated) Serial.printf("  pins=%016llx\n", (unsigned long long)pins);
        perfWindowUs = pushEndUs;
        perfFrames = 0;
        perfDrawn = 0;
        perfEmuUs = 0;
        perfJoinUs = 0;
        perfEmuDrawUs = 0;
        perfEmuSkipUs = 0;
        perfUpdUs = 0;
        perfAudioUs = 0;
        perfDmaUs = 0;
        perfDrain2Us = 0;
        perfRingMin = AUDIO_RING_SAMPLES;
        perfRingMax = 0;
        g_ringDropped = 0;
    }
#endif

    nextFrameUs += FRAME_PERIOD_US;
    const int64_t remainingUs = nextFrameUs - esp_timer_get_time();
    const bool behindSchedule = remainingUs <= 0;
    if (behindSchedule) {
        // Carry the deficit instead of resyncing: the repaint frame structurally
        // overruns the period (~19ms vs 16.6ms) while the following skip frames
        // each finish under it, so the debt is repaid within one divisor cycle
        // and the average holds 60Hz. Resyncing here forfeited that slack and
        // capped the loop at ~58fps with the skip-frame headroom going idle.
        // The cap is what stops a genuinely overloaded loop from spiraling: once
        // the debt exceeds it, resync and let the frame rate drop honestly.
        const bool overloaded = -remainingUs > FRAME_DEBT_CAP_US;
        if (overloaded) nextFrameUs = esp_timer_get_time();
        return;
    }
    // delay() yields to the idle task (needed for the watchdog); the residual
    // sub-millisecond wait is spun out for frame-time accuracy.
    const int64_t delayMs = remainingUs / 1000;
    if (delayMs > 1) delay((uint32_t)(delayMs - 1));
    while (esp_timer_get_time() < nextFrameUs) {}
}

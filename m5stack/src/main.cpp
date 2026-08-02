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
#include <cstring>
#include <new>

#include "../../core/nes.h"
#include "config.h"
#include "grove_input.h"
#include "head_touch.h"
#include "menu.h"
#include "sd_rom.h"
#include "secrets.h"

// What the frame loop is currently doing. The two modes are mutually exclusive
// owners of the panel and the speaker: Game drives them through the band DMA
// path and the audio ring, Menu through ordinary blocking primitives with both
// idle. stopVideoAudio() / startGame() are the handover in each direction.
enum class AppMode : uint8_t { Menu, Game };
static AppMode g_mode = AppMode::Game;

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
// Set by the BtnC hold, consumed at a frame boundary. Plain rather than atomic
// would work — it is written and read on core 1 only — but it is latched the
// same way as the rest so a future sender on core 0 needs no change here.
static std::atomic<bool> g_menuRequested{false};
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
// The other half of the staging interlock, in the opposite direction: core 1
// sets this while it is filling g_romBuf from the SD card, and the UDP task's
// BEGIN refuses with BUSY for as long as it is set. g_romApplyRequested alone
// cannot cover that case — it means "core 0 has published an image", which is
// exactly not what an SD load does.
static std::atomic<bool> g_stagingBusy{false};
// The SD save a completed type 4 transfer asked for, published alongside the
// image itself. Read only when g_romApplyRequested has been observed with an
// acquire load, so it needs no ordering of its own.
static bool g_romSaveToSd = false;
static char g_romSaveName[SD_ROM_NAME_MAX] = {};
// Where to report the save's outcome, since the END ACK has already gone out by
// the time core 1 writes the card.
static sockaddr_in g_romSaveReplyTo = {};

// A type 5 request, latched by the UDP task and serviced by the frame loop.
//
// The payload is copied out of the datagram rather than pointed into it: the
// receive buffer is reused by the very next recvfrom(), so by the time core 1
// looks the name could be part of an unrelated packet. Plain (non-atomic)
// fields published by the release store on g_sdRequested, exactly as the ROM
// staging buffer is.
static std::atomic<bool> g_sdRequested{false};
static uint8_t g_sdOp = 0;
static uint16_t g_sdSeq = 0;
static char g_sdArgA[SD_ROM_NAME_MAX] = {};
static char g_sdArgB[SD_ROM_NAME_MAX] = {};
static sockaddr_in g_sdReplyTo = {};

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
// Written only by the UDP task, but read by core 1's staging claim, so it is
// atomic. Left with the default (seq_cst) operators everywhere: launchSdRom()
// needs its claim store and this load to sit in one total order, and
// acquire/release would not give that — they only order each core's own writes
// against its own reads, which is exactly not what a two-flag handshake needs.
static std::atomic<bool> g_romActive{false};   // a BEGIN has been accepted and not yet finished
static uint16_t g_romSession = 0;
static uint32_t g_romExpectedSize = 0;
static uint32_t g_romExpectedCrc = 0;
static uint8_t g_romPendingFlags = 0;
static uint16_t g_romNextChunk = 0;   // the chunk index a DATA packet must carry
static uint32_t g_romReceived = 0;   // bytes staged so far
static uint32_t g_romLastRxMs = 0;   // for the stale-session takeover
// The name from a BEGIN's optional tail, held until END publishes it. Empty
// when the sender used the old fixed-length BEGIN.
static char g_romPendingName[SD_ROM_NAME_MAX] = {};
// The BEGIN's source, so the save outcome reaches the same peer that asked for
// it even if some other host is also talking to the device.
static sockaddr_in g_romPendingFrom = {};

// Copy a length-prefixed name field out of a datagram.
//
// Returns the number of bytes consumed (the length byte plus the name), or -1
// when the field runs past what actually arrived. Every string this protocol
// carries goes through here rather than being read inline, so a lying length
// byte is rejected in one place instead of in each op's parser.
static int readNameField(const uint8_t* packet, int received, int offset, char* out, size_t cap) {
    out[0] = '\0';
    const bool noLengthByte = offset >= received;
    if (noLengthByte) return -1;
    const int len = packet[offset];
    const bool runsPastDatagram = offset + 1 + len > received;
    if (runsPastDatagram) return -1;
    // A name that does not fit is not silently truncated: a truncated name is a
    // *different* file, and acting on it would delete or overwrite the wrong
    // one. The caller sees the empty string and answers BadName.
    const bool tooLong = (size_t)len >= cap;
    if (tooLong) return -1;
    memcpy(out, packet + offset + 1, (size_t)len);
    out[len] = '\0';
    return 1 + len;
}

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

// Validate an image the same way nes::loadRom will, so a cart that cannot
// possibly load is rejected while the sender is still listening — rather than
// failing on core 1 where the only report would be a serial line.
//
// Takes the buffer rather than reading g_romBuf directly: the same question has
// to be answered for an image read off the SD card, which never passes through
// staging on the UDP task's schedule. Keeping one implementation is what stops
// the two paths from disagreeing about which mappers this build supports.
static uint8_t romHeaderStatus(const uint8_t* buf, uint32_t size) {
    // Takes the size rather than trusting the caller to have checked it: BEGIN
    // accepts any total from 1 byte up, so a 3-byte transfer reaches END and
    // would otherwise have its mapper number read out of never-written staging.
    const bool tooShortForHeader = size < 16;
    if (tooShortForHeader) return UDP_ROM_STATUS_BAD_HEADER;

    const bool magicOk = buf[0] == 'N' && buf[1] == 'E' && buf[2] == 'S' && buf[3] == 0x1A;
    if (!magicOk) return UDP_ROM_STATUS_BAD_HEADER;

    // Archaic iNES: bytes 12-15 should be zero, and when they are not (e.g.
    // "DiskDude!" garbage) flags7's upper nibble is not a mapper number. Mirrors
    // cartridge.cpp's dirtyHeader rule exactly — disagreeing would let a ROM pass
    // here and then fail to load.
    const bool dirtyHeader = buf[12] || buf[13] || buf[14] || buf[15];
    const int mapperNum = (buf[6] >> 4) | (dirtyHeader ? 0 : (buf[7] & 0xF0));
    const bool mapperSupported = mapperNum == 0 || mapperNum == 1 || mapperNum == 2 || mapperNum == 3 ||
                                 mapperNum == 4 || mapperNum == 24 || mapperNum == 26;
    if (!mapperSupported) return UDP_ROM_STATUS_UNSUPPORTED_MAPPER;
    return UDP_ROM_STATUS_OK;
}

// Reserve the PSRAM staging buffer on first use, and report whether it exists.
//
// Reserved once and never released: a buffer that comes and goes would race
// core 1 and fragment PSRAM for nothing. 1MB against 8MB is cheap.
//
// Called from both cores — the UDP task on a BEGIN, core 1 before an SD load —
// but never concurrently: core 1 only asks while holding g_stagingBusy, which
// the BEGIN path checks first, so the allocation itself needs no lock.
static bool ensureStagingBuffer() {
    const bool needBuffer = g_romBuf == nullptr;
    if (needBuffer) g_romBuf = (uint8_t*)heap_caps_malloc(ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
    return g_romBuf != nullptr;
}

static void handleRomPacket(int sock, const sockaddr_in& from, const uint8_t* packet, int received) {
    const uint16_t session = (uint16_t)(packet[4] | (packet[5] << 8));
    const uint8_t op = packet[6];

    // The staging buffer belongs to core 1 until it has installed the ROM, and
    // equally while core 1 is filling it from the SD card. Taking a new transfer
    // in either window would overwrite the image out from under it.
    const bool applyPending = g_romApplyRequested.load(std::memory_order_acquire);
    // seq_cst, unlike the acquire above: this load is the UDP half of
    // launchSdRom()'s claim handshake, and only a total order over both flags
    // stops the two cores from each deciding the buffer is theirs.
    const bool loopOwnsStaging = g_stagingBusy.load(std::memory_order_seq_cst);
    if (applyPending || loopOwnsStaging) {
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
        if (!ensureStagingBuffer()) {
            sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_ALLOC);
            return;
        }

        g_romActive = true;
        g_romSession = session;
        g_romExpectedSize = total;
        g_romExpectedCrc = (uint32_t)packet[12] | ((uint32_t)packet[13] << 8) | ((uint32_t)packet[14] << 16) |
                           ((uint32_t)packet[15] << 24);
        g_romPendingFlags = packet[7];
        // Length-discriminated, not flag-discriminated: a sender that predates
        // the SD support sends exactly UDP_ROM_BEGIN_SIZE bytes and gets the old
        // behaviour, with no bit it would have had to reserve in advance.
        g_romPendingName[0] = '\0';
        const bool hasNameField = received >= UDP_ROM_BEGIN_NAMED_SIZE;
        if (hasNameField) readNameField(packet, received, UDP_ROM_BEGIN_SIZE, g_romPendingName, SD_ROM_NAME_MAX);
        g_romPendingFrom = from;
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
        const uint8_t headerStatus = romHeaderStatus(g_romBuf, g_romReceived);
        const bool unloadable = headerStatus != UDP_ROM_STATUS_OK;
        if (unloadable) {
            g_romActive = false;
            sendRomAck(sock, from, op, session, 0, headerStatus);
            return;
        }

        g_romSize = g_romReceived;
        g_romFlags = g_romPendingFlags;
        // A save with nothing to call the file is not a save. Dropping the flag
        // rather than inventing a name keeps the card free of images the user
        // cannot recognise later.
        const bool nameGiven = g_romPendingName[0] != '\0';
        g_romSaveToSd = (g_romPendingFlags & ROM_FLAG_SAVE_SD) != 0 && nameGiven;
        memcpy(g_romSaveName, g_romPendingName, sizeof(g_romSaveName));
        g_romSaveReplyTo = g_romPendingFrom;
        g_romActive = false;
        // Publishes the buffer and the plain globals above to core 1.
        g_romApplyRequested.store(true, std::memory_order_release);
        sendRomAck(sock, from, op, session, 0, UDP_ROM_STATUS_OK);
        return;
    }
}

// ------------------------------------------------------------- SD commands

// Single-datagram reply, used by LOAD / DELETE / RENAME and by a LIST that
// failed before it had anything to list.
static void sendSdAck(int sock, const sockaddr_in& to, uint8_t op, uint16_t seq, SdStatus status) {
    uint8_t ack[UDP_SD_ACK_SIZE] = {};
    ack[0] = 'N';
    ack[1] = 'S';
    ack[2] = UDP_PROTOCOL_VERSION;
    ack[3] = op;
    ack[4] = seq & 0xFF;
    ack[5] = seq >> 8;
    ack[6] = (uint8_t)status;
    ::sendto(sock, ack, sizeof(ack), 0, (const sockaddr*)&to, sizeof(to));
}

// Latch a type 5 request for the frame loop.
//
// Nothing here touches the card: the SPI bus is shared with the panel, and this
// runs on core 0 where there is no way to know whether a band is in flight. The
// only work done on this side is validating that the datagram is self-consistent
// and copying the arguments somewhere the receive buffer's reuse cannot reach.
static void handleSdPacket(int sock, const sockaddr_in& from, const uint8_t* packet, int received) {
    const uint16_t seq = (uint16_t)(packet[4] | (packet[5] << 8));
    const uint8_t op = packet[6];

    const bool known = op == UDP_SD_OP_LIST || op == UDP_SD_OP_LOAD || op == UDP_SD_OP_DELETE || op == UDP_SD_OP_RENAME;
    if (!known) return;

    // One outstanding request at a time. Queueing would need a depth, a policy
    // for overflow and an ordering guarantee across two cores, for a protocol
    // whose sender is a stop-and-wait loop that has no reason to pipeline.
    const bool alreadyPending = g_sdRequested.load(std::memory_order_acquire);
    if (alreadyPending) {
        sendSdAck(sock, from, op, seq, SdStatus::Busy);
        return;
    }

    char argA[SD_ROM_NAME_MAX] = {};
    char argB[SD_ROM_NAME_MAX] = {};
    const bool needsName = op != UDP_SD_OP_LIST;
    if (needsName) {
        const int consumed = readNameField(packet, received, UDP_SD_HEADER, argA, sizeof(argA));
        const bool malformed = consumed < 0 || argA[0] == '\0';
        if (malformed) {
            sendSdAck(sock, from, op, seq, SdStatus::BadName);
            return;
        }
        const bool needsSecondName = op == UDP_SD_OP_RENAME;
        if (needsSecondName) {
            const int consumedB = readNameField(packet, received, UDP_SD_HEADER + consumed, argB, sizeof(argB));
            const bool malformedB = consumedB < 0 || argB[0] == '\0';
            if (malformedB) {
                sendSdAck(sock, from, op, seq, SdStatus::BadName);
                return;
            }
        }
    }

    g_sdOp = op;
    g_sdSeq = seq;
    memcpy(g_sdArgA, argA, sizeof(g_sdArgA));
    memcpy(g_sdArgB, argB, sizeof(g_sdArgB));
    g_sdReplyTo = from;
    // Publishes the fields above to core 1, the same pairing the ROM staging
    // buffer uses.
    g_sdRequested.store(true, std::memory_order_release);
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
        if (type == UDP_TYPE_SD) {
            handleSdPacket(sock, from, packet, received);
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
// Defined with the rest of the mode handover, below the audio and display state
// they touch; declared here because setup() picks the starting mode.
static void startGame();
static void enterMenu();

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

    // Mount the card here, while the display is still being driven by ordinary
    // blocking primitives. Doing it after the frame loop has started would mean
    // reaching for the shared SPI bus with a band possibly in flight; setup()
    // has no bands outstanding by construction, so this is the one place where
    // the ordering costs nothing to guarantee.
    // 内部 I2C 上のセンサーなので、外部 I2C を張り替える groveInputInit() とは
    // 独立に呼べる。StackChan ボディが無ければ検出に失敗して以降無効になるだけで、
    // 素の CoreS3 の起動には影響しない。
    headTouchInit();

    sdRomInit();
    int sdRomsFound = 0;
    if (sdRomMounted()) {
        static SdRomEntry entries[SD_ROM_MAX_FILES];
        sdRomsFound = sdRomScan(entries, SD_ROM_MAX_FILES);
        Serial.printf("SD: %d ROM(s) in %s\n", sdRomsFound, SD_ROMS_DIR);
        for (int i = 0; i < sdRomsFound; i++) {
            Serial.printf("SD:   %s (%u bytes)\n", entries[i].name, (unsigned)entries[i].size);
        }
    }

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

    // NB: no startWrite() here. Holding the bus open across the whole run leaves
    // the panel's address window owned by whatever ran last, so pushes land at
    // the wrong offset; pushImageDMA sets the window itself per call.
    Serial.printf("DISPLAY: %dx%d rot=%d push=(%d,%d,%d,%d)\n", M5.Display.width(), M5.Display.height(),
                  M5.Display.getRotation(), SCREEN_X_OFFSET, 0, NES_WIDTH, NES_HEIGHT);

    // The picker only earns the boot delay when there is something on the card
    // to pick. With no card, or an empty /roms, the only choice it could offer
    // is the built-in image that was just loaded, so a device without an SD
    // card boots exactly as it always has — straight into the game.
    const bool haveChoice = sdRomsFound > 0;
    if (haveChoice) {
        enterMenu();
        // Left up to the picker: it draws the whole panel, and a WiFi warning in
        // the footer would land under the guide line it draws there.
        return;
    }

    startGame();
    if (!g_wifiConnected) {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        showMessage("WiFi: failed", 228, 1);
    }
}

// ------------------------------------------------------------------- loop

// Buttons that live on the CoreS3 itself: the three touch zones below the
// screen. Start/Select have no home on the Grove units (the joystick's centre
// press doubles as Start, but Select needs somewhere), and a long-press on the
// right zone opens the ROM picker for standalone play.
//
// That hold used to be RESET. It was reassigned because the picker is the only
// standalone way to reach a different cart, while RESET is still reachable —
// over UDP type 2 from the browser, and from the picker itself by choosing the
// running ROM again, which is a power-on rather than a reset but gets the user
// to the same place. A device with no other button to spare has to spend the
// one it has on the thing that cannot be done another way.
static uint8_t touchButtonBits() {
    uint8_t bits = 0;
    if (M5.BtnA.isPressed()) bits |= NES_BTN_SELECT;
    if (M5.BtnB.isPressed()) bits |= NES_BTN_START;
    if (M5.BtnC.wasHold()) g_menuRequested.store(true, std::memory_order_relaxed);
    return bits;
}

// StackChan の頭を撫でる操作。BtnC 長押しと同じラッチを立てるだけで、実際の
// 遷移は loop() のフレーム境界に任せる。
//
// 呼ぶのは applyInput() の中、つまり Game モードのフレームだけ。メニュー中は
// loop() が先に return しているのでポーリング自体が走らず、パネルと SPI を
// 触っているメニューの裏で I2C が動くこともない。
static void applyHeadTouch() {
    const bool swiped = headTouchSwiped();
    if (!swiped) return;
    g_menuRequested.store(true, std::memory_order_relaxed);
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
    // パッドのビットには寄与しない (撫でるのはメニューを開く操作であって
    // ボタンではない) が、同じ「毎フレームの入力取り込み」の一部なのでここに置く。
    applyHeadTouch();
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

// Smoothed emulation time of fully-rendered draw frames, and whether it holds a
// real sample yet. Read by the repaint guard in loop() — see the long comment
// there for what it stands for and why it is filtered.
//
// At file scope rather than function-local statics inside loop() so reset and ROM
// swap can invalidate it. The evidence describes one cart running one scene; a
// different cart, or the same one restarted, is a different writer whose pace has
// not been measured yet.
static float g_emuDrawMs = 0.0f;
static bool g_emuDrawSeeded = false;

// Forget the measured emulation pace. Called whenever the thing being measured is
// replaced, so the guard falls back to its armed default until a fresh
// fully-rendered draw frame re-seeds it.
//
// Why not keep the old average across a reset: it was gathered from a game that
// had booted into steady state, and the frames right after a reset are the boot
// sequence again — fast, mostly rendering-off, and exactly the ones the guard
// exists to protect. Carrying the old evidence forward would stand the guard down
// across that window on the strength of measurements that no longer apply.
static void invalidateEmuDrawPace() {
    g_emuDrawMs = 0.0f;
    g_emuDrawSeeded = false;
}

// The RESET button, as the browser's connector UI presses it. Runs after the pin
// state is applied so a "reseat and reset" arrives in the same order it happens
// on a real console: contacts restored first, then the reset vector fetched.
// NES::reset() keeps work RAM, which is what the physical button does.
static void applyResetRequest() {
    const bool requested = g_resetRequested.exchange(false, std::memory_order_relaxed);
    if (!requested) return;
    g_nes.reset();
    invalidateEmuDrawPace();
    Serial.println("RESET: console reset");
}

// Hand an image to the core, whatever it came from.
//
// Split out of applyRomRequest() because the SD path installs the same way from
// a buffer that never went through the UDP session machinery: the mapper swap,
// the fallback to the embedded image and the pace invalidation are properties of
// *installing a cart*, not of how its bytes arrived. Returns whether the
// requested image loaded — false still leaves a playable console, either the
// previous cart (swap path) or the embedded ROM.
//
// Must be called at a frame boundary: swapping the mapper out from under a
// running instruction would fault.
static bool installRom(const uint8_t* data, uint32_t size, bool wantSwap) {
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
            auto m = nes::loadRom(data, size);
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
            ok = g_nes.loadRom(data, size);   // powerOn + refreshChrWindow included
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

    // Unconditionally, on every path that got this far. A successful load is
    // obviously a new writer; a failed one either left the old cart running (swap
    // path) or fell back to the embedded image, and in the failure case the frames
    // around it were spent allocating rather than emulating. None of those are
    // described by the average built before this point.
    invalidateEmuDrawPace();

    if (ok) Serial.printf("ROM: applied %u bytes%s\n", (unsigned)size, wantSwap ? " (no reset)" : "");
    else Serial.printf("ROM: failed %u bytes\n", (unsigned)size);
    return ok;
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

    // Saved before installing, for two reasons. The image is still exactly what
    // the sender verified — installRom() does not modify staging, but a future
    // change to it must not be able to alter what lands on the card. And a
    // NO_LOAD transfer has no install to sequence against at all.
    if (g_romSaveToSd) {
        // The panel and the card share the SPI bus, so the in-flight band has to
        // be off the wire before the write starts. This is what makes the whole
        // save legal here and illegal in the UDP task.
        joinBand();
        char clean[SD_ROM_NAME_MAX];
        sdRomSanitizeName(g_romSaveName, clean, sizeof(clean));
        // Rejected rather than saved under the cleaned spelling: the sender then
        // knows the name it will find on the card is not the one it asked for,
        // and can offer the user the corrected one instead of guessing later.
        const bool nameMangled = strcmp(clean, g_romSaveName) != 0;
        const SdStatus status = nameMangled ? SdStatus::BadName : sdRomSave(clean, g_romBuf, g_romSize);
        // A separate datagram, because the END ACK went out from the UDP task
        // the moment the CRC checked — holding that ACK until the card write
        // finished would stall the sender across a ~1-2s write.
        const bool canReply = g_udpSock >= 0;
        if (canReply) {
            uint8_t event[UDP_ROM_SAVE_EVENT_SIZE] = {};
            event[0] = 'N';
            event[1] = 'S';
            event[2] = UDP_PROTOCOL_VERSION;
            event[3] = UDP_TYPE_ROM;
            event[4] = g_romSession & 0xFF;
            event[5] = g_romSession >> 8;
            event[6] = (uint8_t)status;
            ::sendto(g_udpSock, event, sizeof(event), 0, (const sockaddr*)&g_romSaveReplyTo, sizeof(g_romSaveReplyTo));
        }
        Serial.printf("ROM: save '%s' -> %s\n", g_romSaveName, sdStatusText(status));
    }

    // "Add to my library" rather than "play this now": the running game keeps
    // going, and the picker (if it is up) picks the new file up on its next
    // scan. A failed save still skips the install — the sender asked for a file,
    // not for a cart swap, and interrupting the game would be a surprise.
    const bool installWanted = (g_romFlags & ROM_FLAG_NO_LOAD) == 0;
    if (installWanted) installRom(g_romBuf, g_romSize, (g_romFlags & ROM_FLAG_SWAP) != 0);

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

// Which band the next kick ships. File scope for the same reason as the flag
// above: leaving the frame loop for the menu has to reset it, or the first band
// pushed after coming back would land mid-picture and paint one band of the new
// game over three of the old.
static int g_bandIndex = 0;

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

// Bring the panel and the speaker back to the state setup() left them in, so
// something other than the frame loop can own them.
//
// The three steps are not independent and the order matters. The band DMA has
// to be joined first or the SPI bus stays locked and anything drawn afterwards
// interleaves with a transfer still on the wire. The band index has to be reset
// or the next picture starts from whichever band the loop was interrupted at.
// And the ring has to be emptied rather than merely stopped: it holds up to
// ~186ms of the previous game's audio, which would otherwise play out over the
// first frames of the next one.
//
// Why not simply not stop the speaker: M5.Speaker.stop() drops what is already
// queued in the hardware, and playRaw's queued buffers reference g_chunk slots
// that the next game's drain will begin overwriting.
static void stopVideoAudio() {
    joinBand();
    g_bandIndex = 0;
    M5.Speaker.stop(SPEAKER_CHANNEL);
    g_ringRead = 0;
    g_ringWrite = 0;
    // The DC blocker's state describes the signal that just stopped; carrying it
    // into silence would decay as an audible thump at the start of the next one.
    g_dcX1 = 0.0f;
    g_dcY1 = 0.0f;
}

// Hand the panel and the speaker back to the frame loop.
//
// The silence priming moved here from setup(): it is a property of *starting a
// game*, and after a menu the speaker queue is as empty as it is at boot, so
// the first real chunks would race the hardware buffer exactly the same way.
static void startGame() {
    // Prime the speaker queue with silence so the first real chunks arrive with
    // margin instead of racing an already-empty hardware buffer.
    for (int i = 0; i < 2; i++) {
        memset(g_chunk[g_chunkIndex], 0, sizeof(g_chunk[0]));
        M5.Speaker.playRaw(g_chunk[g_chunkIndex], AUDIO_CHUNK_SAMPLES, AUDIO_SAMPLE_RATE, false, 1, SPEAKER_CHANNEL);
        g_chunkIndex = (g_chunkIndex + 1) % AUDIO_CHUNK_SLOTS;
    }
    // The menu owns the whole panel, including the columns either side of the
    // 256px picture, so it has to be cleared here rather than left for the first
    // band to overwrite — which it never would.
    M5.Display.fillScreen(TFT_BLACK);
    // The frames right after a launch are a boot sequence, not the steady state
    // the guard's average was built from.
    invalidateEmuDrawPace();
    g_mode = AppMode::Game;
}

// Leave the game and put the picker up.
static void enterMenu() {
    stopVideoAudio();
    menuEnter();
    g_mode = AppMode::Menu;
}

// Read a ROM off the card into staging and install it.
//
// Staging is shared with the UDP receive path, so g_stagingBusy is held for the
// whole read-plus-install: a BEGIN that arrived between the read and the install
// would otherwise overwrite the image while installRom() is walking it.
static SdStatus launchSdRom(const char* name) {
    if (!ensureStagingBuffer()) return SdStatus::IoError;
    // Claim first, then re-check. Checking before claiming leaves a window: a
    // BEGIN that passed its own g_stagingBusy test just before the store below
    // would start filling g_romBuf while sdRomLoad() is reading into it, and the
    // installed image would be a splice of both transfers.
    //
    // Why not acquire/release: the pairing here is store-then-load on one flag
    // against store-then-load on another, and release/acquire orders neither
    // core's store ahead of its own subsequent load. Only a single total order
    // over the four operations rules out both cores concluding they won, which
    // is what seq_cst buys. g_romActive covers a transfer that is mid-flight but
    // has not published yet, which g_romApplyRequested alone would miss.
    g_stagingBusy.store(true, std::memory_order_seq_cst);
    const bool udpOwnsStaging = g_romApplyRequested.load(std::memory_order_seq_cst) || g_romActive.load();
    if (udpOwnsStaging) {
        // Reporting Busy rather than waiting keeps the menu responsive and gives
        // the PC side something it can retry on.
        g_stagingBusy.store(false, std::memory_order_release);
        return SdStatus::Busy;
    }

    size_t size = 0;
    SdStatus status = sdRomLoad(name, g_romBuf, ROM_MAX_SIZE, &size);
    if (status == SdStatus::Ok) {
        // Checked before installing so an unsupported mapper is reported as such
        // instead of surfacing as a generic load failure after the current cart
        // has already been dropped.
        const uint8_t header = romHeaderStatus(g_romBuf, (uint32_t)size);
        if (header != UDP_ROM_STATUS_OK) status = SdStatus::BadRom;
        // No swap: a ROM chosen from the picker is a fresh power-on, which is
        // what putting a different cartridge in means. ROM_FLAG_SWAP exists for
        // the browser's live cart-swap experiment, not for this.
        else if (!installRom(g_romBuf, (uint32_t)size, false)) status = SdStatus::BadRom;
    }
    g_stagingBusy.store(false, std::memory_order_release);
    Serial.printf("SD: launch %s -> %s\n", name, sdStatusText(status));
    return status;
}

// Set when a type 5 op changed what a listing would show, so the picker knows
// to rescan. Not a rescan from within the handler: the handler may run in Game
// mode, where there is no listing to refresh.
static bool g_sdListingChanged = false;

// Send a LIST reply, split across as many datagrams as the entries need.
//
// The whole listing is built once and then sliced, rather than scanning the
// card per datagram: a rescan between parts could see a different set of files
// and produce a reply whose parts do not describe one moment in time.
static void sendSdListing(const sockaddr_in& to, uint16_t seq) {
    if (g_udpSock < 0) return;

    static SdRomEntry entries[SD_ROM_MAX_FILES];
    const bool mounted = sdRomMounted();
    const int count = mounted ? sdRomScan(entries, SD_ROM_MAX_FILES) : 0;
    if (!mounted) {
        sendSdAck(g_udpSock, to, UDP_SD_OP_LIST, seq, SdStatus::NotMounted);
        return;
    }
    uint64_t totalBytes = 0, freeBytes = 0;
    sdRomSpace(&totalBytes, &freeBytes);

    // How many entries fit one datagram, worst case. Computed against the
    // longest name rather than the actual ones so the part count can be decided
    // before any packing happens, which is what lets nparts be correct in part 0.
    const int perPart = (UDP_SD_CHUNK - UDP_SD_LIST_HEADER) / UDP_SD_ENTRY_MAX;
    // At least one part even for an empty card: "mounted, no ROMs" has to be
    // distinguishable from "no reply arrived".
    const int nparts = count > 0 ? (count + perPart - 1) / perPart : 1;

    uint8_t datagram[UDP_SD_LIST_HEADER + UDP_SD_CHUNK];
    for (int part = 0; part < nparts; part++) {
        const int first = part * perPart;
        int here = count - first;
        if (here > perPart) here = perPart;
        if (here < 0) here = 0;

        datagram[0] = 'N';
        datagram[1] = 'S';
        datagram[2] = UDP_PROTOCOL_VERSION;
        datagram[3] = UDP_SD_OP_LIST;
        datagram[4] = seq & 0xFF;
        datagram[5] = seq >> 8;
        datagram[6] = (uint8_t)SdStatus::Ok;
        datagram[7] = 0;
        datagram[8] = (uint8_t)part;
        datagram[9] = (uint8_t)nparts;
        datagram[10] = (uint8_t)(count & 0xFF);
        datagram[11] = (uint8_t)(count >> 8);
        datagram[12] = (uint8_t)(here & 0xFF);
        datagram[13] = (uint8_t)(here >> 8);
        // Repeated in every part rather than riding only on part 0, so a
        // receiver that lost part 0 still has the capacity once it has retried.
        for (int i = 0; i < 8; i++) datagram[14 + i] = (uint8_t)(totalBytes >> (i * 8));
        for (int i = 0; i < 8; i++) datagram[22 + i] = (uint8_t)(freeBytes >> (i * 8));

        size_t offset = UDP_SD_LIST_HEADER;
        for (int i = 0; i < here; i++) {
            const SdRomEntry& e = entries[first + i];
            const size_t nameLen = strlen(e.name);
            datagram[offset++] = (uint8_t)(e.size & 0xFF);
            datagram[offset++] = (uint8_t)((e.size >> 8) & 0xFF);
            datagram[offset++] = (uint8_t)((e.size >> 16) & 0xFF);
            datagram[offset++] = (uint8_t)((e.size >> 24) & 0xFF);
            datagram[offset++] = (uint8_t)nameLen;
            memcpy(datagram + offset, e.name, nameLen);
            offset += nameLen;
        }
        ::sendto(g_udpSock, datagram, offset, 0, (const sockaddr*)&to, sizeof(to));
    }
    Serial.printf("SD: listed %d entries in %d part(s)\n", count, nparts);
}

// Service a latched type 5 request at a frame boundary.
//
// Every card access in this function is legal only because of the joinBand()
// at the top: the panel and the card share the SPI bus, and this is the point
// at which core 1 can guarantee nothing is on the wire.
static void applySdRequest() {
    const bool requested = g_sdRequested.load(std::memory_order_acquire);
    if (!requested) return;

    joinBand();

    const uint8_t op = g_sdOp;
    const uint16_t seq = g_sdSeq;
    if (op == UDP_SD_OP_LIST) {
        sendSdListing(g_sdReplyTo, seq);
        g_sdRequested.store(false, std::memory_order_release);
        return;
    }

    SdStatus status = SdStatus::IoError;
    bool listingChanged = false;
    if (op == UDP_SD_OP_LOAD) {
        status = launchSdRom(g_sdArgA);
        // A LOAD from the browser is a "play this now", the network equivalent
        // of picking the row. Leaving the picker up on success would show the
        // user a menu for a game that is already running.
        const bool shouldStart = status == SdStatus::Ok && g_mode == AppMode::Menu;
        if (shouldStart) startGame();
    } else if (op == UDP_SD_OP_DELETE) {
        status = sdRomDelete(g_sdArgA);
        listingChanged = status == SdStatus::Ok;
    } else if (op == UDP_SD_OP_RENAME) {
        status = sdRomRename(g_sdArgA, g_sdArgB);
        listingChanged = status == SdStatus::Ok;
    }
    if (listingChanged) g_sdListingChanged = true;

    if (g_udpSock >= 0) sendSdAck(g_udpSock, g_sdReplyTo, op, seq, status);
    // Cleared last, for the same reason the ROM latch is: until this store the
    // UDP task refuses further requests, which is what keeps g_sdArgA/B stable
    // for the duration of the work above.
    g_sdRequested.store(false, std::memory_order_release);
}

// One frame of the picker. Returns once the mode has been decided, so the
// caller's only job is to stop emulating while this is up.
static void menuLoop() {
    M5.update();
    // The Grove pad and the UDP pad both drive the picker, so a user with a
    // joystick plugged in never has to reach for the touch strip.
    const uint32_t sinceRx = millis() - g_lastRxMs.load(std::memory_order_relaxed);
    const bool udpStale = sinceRx > INPUT_TIMEOUT_MS;
    const uint8_t udpBits = udpStale ? 0 : g_padBits[0].load(std::memory_order_relaxed);
    uint8_t nav = udpBits | groveInputBits();
    if (M5.BtnB.isPressed()) nav |= NES_BTN_START;

    // A hold on BtnC while the picker is up means "never mind": go back to
    // whatever was already loaded rather than making the user pick it again.
    const bool resumeRequested = M5.BtnC.wasHold();
    if (resumeRequested) {
        g_menuRequested.store(false, std::memory_order_relaxed);
        startGame();
        return;
    }

    const MenuResult result = menuTick(nav);
    if (result.action == MenuResult::Action::None) {
        // Type 4 and type 5 keep working while the picker is up, and both stage
        // into the same buffer this mode reads from, so they are serviced on the
        // same frame boundary they would be in Game.
        const bool romPushed = g_romApplyRequested.load(std::memory_order_acquire);
        const bool pushWantsPlay = romPushed && (g_romFlags & ROM_FLAG_NO_LOAD) == 0;
        applyRomRequest();
        applySdRequest();
        applyVolumeRequest();
        // A type 5 LOAD starts the game from inside applySdRequest(), so the
        // mode may already have changed. Everything below is menu upkeep and
        // would drag the user straight back out of the game they just launched.
        const bool leftMenu = g_mode != AppMode::Menu;
        if (leftMenu) return;
        // A ROM sent with the install flag is a "play this now", so honour it
        // from the picker as well: the browser's send button should not behave
        // differently depending on whether the user happens to be browsing.
        if (pushWantsPlay) {
            startGame();
            return;
        }
        // A save-only push (or an SD delete/rename) changed what the listing
        // should show, so redraw it rather than leaving a stale row on screen.
        const bool listingStale = romPushed || g_sdListingChanged;
        if (listingStale) {
            g_sdListingChanged = false;
            menuEnter();
        }
        delay(MENU_TICK_MS);
        return;
    }

    if (result.action == MenuResult::Action::LaunchEmbedded) {
        const size_t embeddedSize = (size_t)(rom_end - rom_start);
        const bool ok = installRom(rom_start, (uint32_t)embeddedSize, false);
        if (!ok) {
            menuShowError("built-in ROM failed");
            return;
        }
        startGame();
        return;
    }

    const SdStatus status = launchSdRom(result.sdName);
    if (status != SdStatus::Ok) {
        // Stay in the picker: the previous cart is still loaded, so the user can
        // read the reason and choose again rather than being dropped into a game
        // they did not ask for.
        menuShowError(sdStatusText(status));
        return;
    }
    startGame();
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
    // Handled before anything else, and with an early return, because the rest
    // of this function is the Game mode: it emulates a frame, kicks a band and
    // paces to 60Hz, none of which mean anything while the picker owns the panel.
    const bool inMenu = g_mode == AppMode::Menu;
    if (inMenu) {
        menuLoop();
        return;
    }

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
    // Before the ROM handler and the emulation below, so the frame that opens
    // the picker does not also run a frame of the game whose audio and video
    // stopVideoAudio() has just torn down.
    const bool menuRequested = g_menuRequested.exchange(false, std::memory_order_relaxed);
    if (menuRequested) {
        enterMenu();
        return;
    }
    applyRomRequest();
    // After the ROM handler, not before: both may touch the card, and doing the
    // type 4 save first means a LIST issued right behind a save already sees the
    // new file.
    applySdRequest();
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
    const bool drawThisFrame = (thisFrame % g_divisor) == 0;
    // Bands still owed from the previous picture. A static_assert ties the
    // divisor floor to the segment count, so at the configured divisor this is
    // always zero on a draw frame; the flush below is the defence for a divisor
    // that does not satisfy that relation.
    const bool pictureInFlight = g_bandIndex != 0;
    g_nes.ppu.renderThisFrame = drawThisFrame;

    const int64_t emuStartUs = esp_timer_get_time();
    // A draw frame can arrive while bands are still owed. Finish the old picture
    // first: runFrame() is about to repaint, so those rows would otherwise ship
    // as a mix of two pictures.
    const bool mustFlushBeforeRepaint = drawThisFrame && pictureInFlight;
    if (mustFlushBeforeRepaint) {
        joinBand();
        while (g_bandIndex != 0) {
            pushBand(g_bandIndex);
            g_bandIndex = (g_bandIndex + 1) % DISPLAY_DMA_SEGMENTS;
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
    //
    // The guard is armed by default and only stood down on positive evidence.
    // That asymmetry is deliberate — skipping the join is the unsafe direction, so
    // it has to be earned, while paying it costs at most one band's wire time.
    //
    // Standing it down takes three things, because the average and the frame about
    // to run have to be talking about the same writer:
    //
    //   1. a seeded average at or above the threshold,
    //   2. the previous frame having been fully rendered, and
    //   3. rendering being on right now, as this frame starts.
    //
    // Condition 2 is what keeps the evidence in its domain. The average is built
    // only from fully-rendered frames (see the filter below), so on its own it
    // says nothing about a rendering-off frame — and those are the *fast* ones,
    // 2-3ms against 16. Without it, a game that reaches steady state and then
    // switches rendering off would carry a 16.45ms average into the first
    // rendering-off draw frame and skip the join there: evidence from the slowest
    // writer licensing the fastest one. The previous frame's flag is used as the
    // predictor for the next frame's writer profile — not exact, but rendering
    // state persists across frame boundaries far more often than it flips, and
    // being wrong costs one join rather than a torn picture.
    //
    // Condition 3 closes the one transition condition 2 cannot see. A game that
    // disables $2001 during vblank leaves frameFullyRendered() true — correctly,
    // the visible region it describes *was* drawn in full — so on the very next
    // draw frame conditions 1 and 2 both hold while the writer has already become
    // the fast backdrop fill. Reading the register at the top of the frame catches
    // exactly that case.
    //
    // Why an instantaneous read is legitimate here when an earlier revision was
    // rightly rejected for using one: direction. That revision read renderingOn()
    // and *skipped* the guard when it was false — off meant "no writer to race",
    // which was wrong twice over (ppu.cpp still fills the backdrop across all 256
    // pixels of every visible line whenever renderThisFrame is set, and does it
    // faster than anything else). Here the same read only ever *arms* the guard:
    // off means join. A one-dot sample still cannot characterise a whole frame,
    // but it does not have to — it is a conservative trigger, and the only way it
    // can be wrong is by joining when it need not have.
    //
    // What that leaves uncovered: rendering on at the frame's start and switched
    // off part-way through the visible region. The guard stands down and the frame
    // becomes a partial backdrop fill, so it runs faster than a full repaint. It
    // is still safe on static margin — the writes are paced by emulation, not by
    // the fill, so even that frame takes at least emuS (~12.6ms) and reaches the
    // first row behind the in-flight band (row 180) at ~8.6ms, against ~6.7ms of
    // wire time. Accepted rather than chased: covering it would need a mid-frame
    // hook on $2001 writes, and the margin is real without one.
    //
    // The sample filter asks the frame-level question via frameFullyRendered():
    // only a frame whose visible region was drawn in full is a representative
    // "how long does a repaint take" measurement. Why filter at all: a
    // rendering-off run lasts as long as the game leaves it off — menus, screen
    // transitions, the whole boot sequence — and every frame in it measures 2-3ms.
    // Unfiltered, at alpha 0.05 a single one pulls a 16.45ms average down by
    // ~0.7ms and a few dozen drag it through the threshold, so the average would
    // stop describing the drawing frames it is supposed to bound.
    //
    // Unseeded counts as armed. Until a representative sample exists there is no
    // evidence either way, and the honest price of that is a joinBand stall on
    // each draw frame during boot: bounded (one band's wire time per picture),
    // paid only until the first fully-rendered draw frame lands, and cheap next
    // to a torn picture. reset and ROM swap clear the seed for the same reason.
    //
    // Net behaviour:
    //
    //   boot / unseeded              join   (no evidence yet)
    //   after reset or ROM swap      join   (evidence discarded)
    //   on->off, first draw frame    join   (condition 3: caught at the register)
    //   rendering-off draw frame     join   (conditions 2 and 3 both fail)
    //   off->on, first draw frame    join   (condition 2: prev frame not full)
    //   steady, emulation slow       skip   (all three conditions met)
    //   steady, emulation fast       join   (writer may overtake the DMA)
    //   skipped (non-draw) frame     n/a    (framebuffer untouched)
    static bool prevFrameFullyRendered = false;
    // Sampled before runFrame() so it describes the frame about to run, not the
    // one after it.
    const bool renderingAtFrameStart = g_nes.ppu.renderingOn();
    const bool paceApplies = g_emuDrawSeeded && prevFrameFullyRendered && renderingAtFrameStart;
    const bool guardStoodDown = paceApplies && g_emuDrawMs >= DISPLAY_REPAINT_GUARD_MS;
    const bool repaintRacesBand = drawThisFrame && !guardStoodDown;
    if (repaintRacesBand) joinBand();
    const int64_t flushEndUs = esp_timer_get_time();
    g_nes.runFrame();
    const int64_t emuEndUs = esp_timer_get_time();

    // Feed the guard above. Only draw frames count: they are the ones that
    // repaint, and a skipped frame is much cheaper, so mixing the two in would
    // understate the writer's pace and arm the guard needlessly.
    //
    // And only frames whose visible region was drawn end to end. The EWMA is meant
    // to answer "how long does a full repaint take", so a frame that spent some or
    // all of its lines filling backdrop instead of drawing is not a slow or fast
    // instance of that — it is not an instance of it at all.
    //
    // Read after runFrame() because the flag describes the frame that just ran,
    // and it stays stable through vblank once scanline 241 has been reached.
    const bool fullyRendered = g_nes.ppu.frameFullyRendered();
    if (drawThisFrame && fullyRendered) {
        const float drawMs = (float)(emuEndUs - flushEndUs) / 1000.0f;
        // The first sample is taken whole: there is no prior average to blend it
        // into, and starting at the real value is what stands the guard down from
        // the second picture onward instead of after a decay. No floor is needed
        // now that samples are filtered — the boot frames that made the raw value
        // untrustworthy are exactly the ones that no longer reach here.
        g_emuDrawMs = g_emuDrawSeeded ? g_emuDrawMs + DISPLAY_EMU_EWMA_ALPHA * (drawMs - g_emuDrawMs) : drawMs;
        g_emuDrawSeeded = true;
    }
    // Carried to the next iteration as the predictor for that frame's writer.
    // Every frame updates it, draw or not: a skipped frame still ran the emulator
    // and still tells us what the PPU was doing, and the next draw frame is better
    // served by the most recent answer than by one from several frames back.
    prevFrameFullyRendered = fullyRendered;

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
    // repaint: on a draw frame the previous iteration kicked the final band (the
    // last DISPLAY_DMA_ROWS rows, i.e. from NES_HEIGHT - DISPLAY_DMA_ROWS to
    // NES_HEIGHT - 1 — rows 180-239 at the current values), and runFrame() starts
    // repainting ~0.5ms later. The writer and the reader are separated in both
    // space and time. renderScanline writes strictly top-to-bottom (PPU::step
    // draws line N at dot 256), so that first band row is not touched until
    // (NES_HEIGHT - DISPLAY_DMA_ROWS)/262 of the way through emulation — at the
    // measured emuD=19.7ms that is kick+13.5ms, while the band finishes on the
    // wire at kick+6.15ms. Margin ~7.4ms.
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
        pushBand(g_bandIndex);
        g_pushOutstanding = true;
        g_bandIndex = (g_bandIndex + 1) % DISPLAY_DMA_SEGMENTS;
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

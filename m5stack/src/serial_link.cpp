#include "serial_link.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstring>

#include "config.h"
#include "reply_sink.h"

// Defined in main.cpp: the one dispatch both transports share. Declared here
// rather than in a header because it is the only thing serial_link needs from
// main.cpp, and main.cpp needs nothing from this file but the two entry points
// in serial_link.h.
void dispatchPacket(const ReplySink& sink, const uint8_t* packet, int len);

namespace {

// A frame is COBS(payload | crc16) followed by 0x00. The largest payload is a
// ROM DATA packet, and COBS adds one byte per 254 plus a leading overhead byte.
constexpr size_t PAYLOAD_MAX = UDP_ROM_DATA_HEADER + UDP_ROM_CHUNK;
constexpr size_t FRAMED_MAX = PAYLOAD_MAX + SERIAL_CRC_SIZE;
constexpr size_t ENCODED_MAX = FRAMED_MAX + FRAMED_MAX / 254 + 2;

SemaphoreHandle_t g_txMutex = nullptr;

// CRC-16/CCITT-FALSE. Chosen over CRC-32 because the frame is small and the
// point here is only to notice a byte stream that lost sync, not to protect a
// megabyte — the ROM image keeps its own CRC-32 end to end.
uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            const bool topSet = crc & 0x8000;
            crc = topSet ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// Consistent Overhead Byte Stuffing. Returns bytes written to `out`, which must
// hold ENCODED_MAX. The output contains no 0x00, so the caller appends one as
// the delimiter.
size_t cobsEncode(const uint8_t* in, size_t len, uint8_t* out) {
    size_t readIdx = 0, writeIdx = 1, codeIdx = 0;
    uint8_t code = 1;
    while (readIdx < len) {
        const bool isZero = in[readIdx] == 0;
        if (isZero) {
            out[codeIdx] = code;
            codeIdx = writeIdx++;
            code = 1;
            readIdx++;
            continue;
        }
        out[writeIdx++] = in[readIdx++];
        code++;
        const bool blockFull = code == 0xFF;
        if (blockFull) {
            out[codeIdx] = code;
            codeIdx = writeIdx++;
            code = 1;
        }
    }
    out[codeIdx] = code;
    return writeIdx;
}

// Decode in place. Returns the decoded length, or -1 if the run structure is
// inconsistent — which is the normal outcome for a stray log line that happened
// to sit between two delimiters.
int cobsDecode(const uint8_t* in, size_t len, uint8_t* out, size_t cap) {
    size_t readIdx = 0, writeIdx = 0;
    while (readIdx < len) {
        const uint8_t code = in[readIdx];
        const bool zeroCode = code == 0;
        if (zeroCode) return -1;
        readIdx++;
        for (uint8_t i = 1; i < code; i++) {
            const bool runsPast = readIdx >= len;
            if (runsPast) return -1;
            const bool wouldOverflow = writeIdx >= cap;
            if (wouldOverflow) return -1;
            out[writeIdx++] = in[readIdx++];
        }
        // A full 0xFF block is a continuation, not an encoded zero.
        const bool impliedZero = code < 0xFF && readIdx < len;
        if (impliedZero) {
            const bool wouldOverflow = writeIdx >= cap;
            if (wouldOverflow) return -1;
            out[writeIdx++] = 0;
        }
    }
    return (int)writeIdx;
}

// Hand one decoded frame to the shared dispatch, after checking its CRC.
void deliver(const uint8_t* frame, int len) {
    const bool tooShort = len < (int)SERIAL_CRC_SIZE + UDP_PACKET_SIZE;
    if (tooShort) return;
    const int payloadLen = len - (int)SERIAL_CRC_SIZE;
    const uint16_t want = (uint16_t)(frame[payloadLen] | (frame[payloadLen + 1] << 8));
    const bool corrupt = want != crc16(frame, (size_t)payloadLen);
    if (corrupt) return;

    const bool magicOk = frame[0] == 'N' && frame[1] == 'P';
    const bool versionOk = frame[2] == UDP_PROTOCOL_VERSION;
    if (!magicOk || !versionOk) return;

    dispatchPacket(serialSink(), frame, payloadLen);
}

// Accumulate bytes until a 0x00 delimiter, then decode and deliver.
//
// Static buffers, not stack: a ROM DATA frame is ~1.4KB and this task is given
// 4096 bytes in total, the same reasoning udpTask's packet buffer follows.
void serialTask(void*) {
    static uint8_t encoded[ENCODED_MAX];
    static uint8_t decoded[FRAMED_MAX];
    size_t fill = 0;
    // Set when a frame overran the buffer: the rest of it must be discarded
    // rather than decoded as if it were a fresh frame.
    bool overrun = false;

    for (;;) {
        const int available = Serial.available();
        const bool idle = available <= 0;
        if (idle) {
            // The link is idle most of the time and this task must not spin on
            // core 0, where the speaker refill and the UDP task also live.
            vTaskDelay(pdMS_TO_TICKS(SERIAL_POLL_MS));
            continue;
        }

        for (int i = 0; i < available; i++) {
            const int byteIn = Serial.read();
            const bool exhausted = byteIn < 0;
            if (exhausted) break;

            const bool isDelimiter = byteIn == 0x00;
            if (isDelimiter) {
                const bool usable = !overrun && fill > 0;
                if (usable) {
                    const int len = cobsDecode(encoded, fill, decoded, sizeof(decoded));
                    const bool decoded_ok = len > 0;
                    if (decoded_ok) deliver(decoded, len);
                }
                fill = 0;
                overrun = false;
                continue;
            }

            const bool wouldOverflow = fill >= sizeof(encoded);
            if (wouldOverflow) {
                overrun = true;
                continue;
            }
            encoded[fill++] = (uint8_t)byteIn;
        }
    }
}

}   // namespace

void serialLinkSend(const uint8_t* data, size_t len) {
    const bool tooBig = len > PAYLOAD_MAX;
    if (tooBig) return;
    const bool notReady = g_txMutex == nullptr;
    if (notReady) return;

    // One writer at a time. The UDP path could rely on lwIP's sendto being
    // thread-safe (see main.cpp's g_udpSock comment), but HardwareSerial gives
    // no such guarantee, and both cores send here: core 0 answers ROM and SD
    // requests inline, while core 1 emits the deferred save event and the SD
    // listing from the frame loop. Interleaved writes would splice two frames
    // into one, which the CRC would then reject — losing both.
    xSemaphoreTake(g_txMutex, portMAX_DELAY);

    static uint8_t framed[FRAMED_MAX];
    static uint8_t encoded[ENCODED_MAX];
    memcpy(framed, data, len);
    const uint16_t crc = crc16(data, len);
    framed[len] = (uint8_t)(crc & 0xFF);
    framed[len + 1] = (uint8_t)(crc >> 8);

    const size_t n = cobsEncode(framed, len + SERIAL_CRC_SIZE, encoded);
    Serial.write(encoded, n);
    Serial.write((uint8_t)0x00);

    xSemaphoreGive(g_txMutex);
}

void serialLinkStart() {
    g_txMutex = xSemaphoreCreateMutex();
    const bool noMutex = g_txMutex == nullptr;
    if (noMutex) {
        Serial.println("SERIAL: mutex alloc failed, link disabled");
        return;
    }
    // Core 0 alongside udpTask, for the same reason: core 1 is the emulation
    // core and must not be interrupted by transport work. One below udpTask's
    // priority — a blocked recv costs nothing, whereas this task polls.
    xTaskCreatePinnedToCore(serialTask, "seriallink", 4096, nullptr, 4, nullptr, 0);
}

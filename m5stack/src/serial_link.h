#pragma once

// The same UDP protocol, carried over the USB serial link.
//
// Why this exists: the browser has no way to send UDP, so every device feature
// reachable from the web UI (cartridge pins, ROM transfer, SD management) used
// to require `just serve` — a Python relay that turns HTTP into UDP. Hosting the
// page on GitHub Pages means there is no relay to run, and USB is the only
// transport a browser can drive directly.
//
// Why COBS rather than a length prefix: this link is shared with the log. The
// firmware writes ~58 Serial sites, and PERF_LOG alone emits 2-4 lines a second
// while a transfer is in flight — ROM saves and SD listings log immediately
// after sending their reply. COBS reserves 0x00 as the frame delimiter and
// guarantees the encoded body never contains one, so an ASCII log line can
// never be mistaken for frame content, and a decoder that lost sync recovers at
// the next 0x00. A length-prefixed frame would have to resynchronise by hunting
// for a magic that plain log text can contain by accident, and the failure mode
// there is silent: main.cpp's type dispatch falls through to PAD, so a
// misframed packet presses controller buttons instead of erroring.
//
// Each frame carries a CRC-16 over the payload. UDP had a checksum underneath
// it and this protocol has no per-packet integrity of its own (only the ROM
// image as a whole is CRC-32'd), so dropping to a raw byte stream would
// otherwise remove the last defence against a corrupted header.

#include <cstddef>
#include <cstdint>

// Start the serial receive task.
//
// Unconditional, unlike udpTask which only starts once WiFi is up: a board
// fresh from the browser flasher has no credentials yet, and configuring them
// is precisely what the serial link is for.
void serialLinkStart();

// Write one framed message. Takes the mutex; safe from either core.
void serialLinkSend(const uint8_t* data, size_t len);

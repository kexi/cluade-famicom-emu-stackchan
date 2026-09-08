'use strict';

// The device protocol, spoken from the browser over Web Serial.
//
// The same packets tools/serve_web.py relays as UDP, framed for a byte stream.
// This file exists because GitHub Pages has no relay to run: the page is static,
// and USB is the only transport a browser can drive on its own.
//
// The reference implementation is cli/src/proto/, not tools/serve_web.py. The
// Rust client fixed defects the Python relay still has — most importantly it
// checks the chunk number echoed in a ROM ACK (cli/src/rom_client.rs), without
// which a late ACK for the previous chunk is taken as the answer to this one and
// the transfer runs one behind until END fails with SIZE_MISMATCH.
//
// The authority for every constant here is m5stack/src/config.h.

(() => {
  // ------------------------------------------------------------- constants

  const MAGIC = [0x4e, 0x50]; // 'N','P'
  const VERSION = 1;
  const HEADER_SIZE = 8;

  const TYPE_PINS = 1;
  const TYPE_CTRL = 2;
  const TYPE_ROM = 4;
  const TYPE_SD = 5;
  const TYPE_PROV = 6;

  const CTRL_RESET = 0x01;
  const CTRL_VOLUME = 0x02;

  const ROM_OP_BEGIN = 0;
  const ROM_OP_DATA = 1;
  const ROM_OP_END = 2;
  const ROM_OP_ABORT = 3;
  const ROM_BEGIN_SIZE = 16;
  const ROM_DATA_HEADER = 12;
  const ROM_CHUNK = 1400;
  const ROM_ACK_SIZE = 12;
  const ROM_MAX_SIZE = 1024 * 1024;
  const ROM_FLAG_SWAP = 0x01;
  const ROM_FLAG_SAVE_SD = 0x02;
  const ROM_FLAG_NO_LOAD = 0x04;

  const SD_OP_LIST = 0;
  const SD_OP_LOAD = 1;
  const SD_OP_DELETE = 2;
  const SD_OP_RENAME = 3;
  const SD_LIST_HEADER = 30;

  const PROV_OP_SET = 0;
  const PROV_OP_STATUS = 1;
  const PROV_OP_APPLY = 2;
  const PROV_STATUS_SIZE = 13;

  const SERIAL_CRC_SIZE = 2;

  // Timings, all from the Rust client so the two behave the same.
  const ROM_TIMEOUT_MS = 300;
  const ROM_RETRIES = 8;
  const ROM_MAX_TOTAL_RETRIES = 64;
  const ROM_SAVE_TIMEOUT_MS = 6000;
  const SD_TIMEOUT_MS = 3000;
  const SD_BUSY_DEADLINE_MS = 12000;
  const SD_BUSY_RETRY_MS = 250;
  const SD_IDEMPOTENT_ATTEMPTS = 3;

  // Status codes worth naming here; the rest are passed through to the UI,
  // which already maps them (see ROM_STATUS_KEYS / SD_STATUS_KEYS in main.js).
  const ROM_STATUS_OK = 0;
  const ROM_STATUS_SEQ = 4;
  const SD_STATUS_OK = 0;
  const SD_STATUS_BUSY = 7;

  // --------------------------------------------------------------- CRC-32

  // zlib/IEEE, the same polynomial the firmware checks the staged image with.
  const CRC32_TABLE = (() => {
    const table = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
      let c = i;
      for (let bit = 0; bit < 8; bit++) {
        const lowSet = c & 1;
        c = lowSet ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      }
      table[i] = c >>> 0;
    }
    return table;
  })();

  function crc32(bytes) {
    let crc = 0xffffffff;
    for (let i = 0; i < bytes.length; i++) {
      crc = CRC32_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
    }
    return (crc ^ 0xffffffff) >>> 0;
  }

  // --------------------------------------------------------------- CRC-16

  // CCITT-FALSE, matching serial_link.cpp. Guards the frame itself; the ROM
  // image keeps its own CRC-32 end to end.
  function crc16(bytes, len) {
    let crc = 0xffff;
    for (let i = 0; i < len; i++) {
      crc ^= bytes[i] << 8;
      for (let bit = 0; bit < 8; bit++) {
        const topSet = crc & 0x8000;
        crc = topSet ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
      }
    }
    return crc & 0xffff;
  }

  // ----------------------------------------------------------------- COBS

  // Consistent Overhead Byte Stuffing. The encoded body contains no 0x00, so
  // the caller appends one as the delimiter and a decoder that lost sync
  // recovers at the next one — which is what lets protocol frames share this
  // line with the firmware's log output.
  function cobsEncode(input) {
    const out = new Uint8Array(input.length + Math.ceil(input.length / 254) + 2);
    let readIdx = 0;
    let writeIdx = 1;
    let codeIdx = 0;
    let code = 1;
    while (readIdx < input.length) {
      const isZero = input[readIdx] === 0;
      if (isZero) {
        out[codeIdx] = code;
        codeIdx = writeIdx++;
        code = 1;
        readIdx++;
        continue;
      }
      out[writeIdx++] = input[readIdx++];
      code++;
      const blockFull = code === 0xff;
      if (blockFull) {
        out[codeIdx] = code;
        codeIdx = writeIdx++;
        code = 1;
      }
    }
    out[codeIdx] = code;
    return out.subarray(0, writeIdx);
  }

  // Returns the decoded bytes, or null when the run structure is inconsistent —
  // the normal outcome for a log line that happened to sit between delimiters.
  function cobsDecode(input) {
    const out = new Uint8Array(input.length);
    let readIdx = 0;
    let writeIdx = 0;
    while (readIdx < input.length) {
      const code = input[readIdx];
      const zeroCode = code === 0;
      if (zeroCode) return null;
      readIdx++;
      for (let i = 1; i < code; i++) {
        const runsPast = readIdx >= input.length;
        if (runsPast) return null;
        out[writeIdx++] = input[readIdx++];
      }
      const impliedZero = code < 0xff && readIdx < input.length;
      if (impliedZero) out[writeIdx++] = 0;
    }
    return out.subarray(0, writeIdx);
  }

  // ------------------------------------------------------------- builders

  function header(type, seq) {
    const buf = new Uint8Array(HEADER_SIZE);
    buf[0] = MAGIC[0];
    buf[1] = MAGIC[1];
    buf[2] = VERSION;
    buf[3] = type;
    buf[4] = seq & 0xff;
    buf[5] = (seq >> 8) & 0xff;
    return buf;
  }

  function buildPins(mask) {
    const buf = new Uint8Array(14);
    buf.set(header(TYPE_PINS, 0));
    // BigInt, not Number: the mask is 60 bits and the bitwise operators would
    // truncate it to 32.
    let m = BigInt(mask);
    for (let i = 0; i < 8; i++) {
      buf[6 + i] = Number(m & 0xffn);
      m >>= 8n;
    }
    return buf;
  }

  function buildCtrl(cmd, value) {
    const buf = header(TYPE_CTRL, 0);
    buf[6] = cmd;
    buf[7] = value & 0xff;
    return buf;
  }

  // A length-prefixed string, the shape every name in this protocol uses.
  function nameField(text) {
    const encoded = new TextEncoder().encode(text ?? '');
    const buf = new Uint8Array(1 + encoded.length);
    buf[0] = encoded.length;
    buf.set(encoded, 1);
    return buf;
  }

  function concat(parts) {
    const total = parts.reduce((n, p) => n + p.length, 0);
    const out = new Uint8Array(total);
    let offset = 0;
    for (const part of parts) {
      out.set(part, offset);
      offset += part.length;
    }
    return out;
  }

  function buildRomBegin(session, flags, total, crc, name) {
    const buf = new Uint8Array(ROM_BEGIN_SIZE);
    buf.set(header(TYPE_ROM, session));
    buf[6] = ROM_OP_BEGIN;
    buf[7] = flags;
    new DataView(buf.buffer).setUint32(8, total, true);
    new DataView(buf.buffer).setUint32(12, crc, true);
    // Length-discriminated, not flag-discriminated: a sender that predates the
    // name field sends exactly ROM_BEGIN_SIZE, so anything longer is
    // unambiguously the new form (config.h:390-399).
    const named = name && name.length > 0;
    if (!named) return buf;
    return concat([buf, nameField(name)]);
  }

  function buildRomData(session, chunk, payload) {
    const buf = new Uint8Array(ROM_DATA_HEADER + payload.length);
    buf.set(header(TYPE_ROM, session));
    buf[6] = ROM_OP_DATA;
    const view = new DataView(buf.buffer);
    view.setUint16(8, chunk, true);
    view.setUint16(10, payload.length, true);
    buf.set(payload, ROM_DATA_HEADER);
    return buf;
  }

  function buildRomMark(session, op) {
    const buf = header(TYPE_ROM, session);
    buf[6] = op;
    return buf;
  }

  function buildSd(seq, op, args) {
    const parts = [header(TYPE_SD, seq)];
    parts[0][6] = op;
    if (op === SD_OP_LOAD || op === SD_OP_DELETE) parts.push(nameField(args.name));
    if (op === SD_OP_RENAME) {
      parts.push(nameField(args.name));
      parts.push(nameField(args.to));
    }
    return concat(parts);
  }

  function buildProv(seq, op, args) {
    const parts = [header(TYPE_PROV, seq)];
    parts[0][6] = op;
    if (op === PROV_OP_SET) {
      parts.push(nameField(args.ssid));
      parts.push(nameField(args.pass));
    }
    return concat(parts);
  }

  // --------------------------------------------------------------- parsers

  function parseRomAck(bytes) {
    const wrongSize = bytes.length < ROM_ACK_SIZE;
    if (wrongSize) return null;
    const isAck = bytes[0] === 0x4e && bytes[1] === 0x52; // 'N','R'
    if (!isAck) return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return {
      op: bytes[3],
      session: view.getUint16(4, true),
      chunk: view.getUint16(6, true),
      status: bytes[8],
      expected: view.getUint16(9, true),
    };
  }

  function parseRomSaveEvent(bytes) {
    const wrongSize = bytes.length < 8;
    if (wrongSize) return null;
    const isEvent = bytes[0] === 0x4e && bytes[1] === 0x53 && bytes[3] === TYPE_ROM; // 'N','S'
    if (!isEvent) return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return { session: view.getUint16(4, true), status: bytes[6] };
  }

  function parseSdAck(bytes) {
    const wrongSize = bytes.length < 8;
    if (wrongSize) return null;
    const isSd = bytes[0] === 0x4e && bytes[1] === 0x53 && bytes[3] !== TYPE_ROM;
    if (!isSd) return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return { op: bytes[3], seq: view.getUint16(4, true), status: bytes[6] };
  }

  function parseSdListPart(bytes) {
    const tooShort = bytes.length < SD_LIST_HEADER;
    if (tooShort) return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const part = {
      seq: view.getUint16(4, true),
      status: bytes[6],
      part: bytes[8],
      nparts: bytes[9],
      total: view.getUint16(10, true),
      count: view.getUint16(12, true),
      totalBytes: view.getBigUint64(14, true),
      freeBytes: view.getBigUint64(22, true),
      entries: [],
    };
    let offset = SD_LIST_HEADER;
    for (let i = 0; i < part.count; i++) {
      const runsPast = offset + 5 > bytes.length;
      if (runsPast) return null;
      const size = view.getUint32(offset, true);
      const nameLen = bytes[offset + 4];
      offset += 5;
      const nameRunsPast = offset + nameLen > bytes.length;
      if (nameRunsPast) return null;
      const name = new TextDecoder().decode(bytes.subarray(offset, offset + nameLen));
      offset += nameLen;
      part.entries.push({ name, size });
    }
    return part;
  }

  function parseProvReply(bytes) {
    const tooShort = bytes.length < 8;
    if (tooShort) return null;
    const isProv = bytes[0] === 0x4e && bytes[1] === 0x57; // 'N','W'
    if (!isProv) return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const reply = { op: bytes[3], seq: view.getUint16(4, true), status: bytes[6] };
    const hasStatusTail = reply.op === PROV_OP_STATUS && bytes.length > PROV_STATUS_SIZE;
    if (!hasStatusTail) return reply;

    reply.connected = bytes[8] === 1;
    reply.ip = `${bytes[9]}.${bytes[10]}.${bytes[11]}.${bytes[12]}`;
    let offset = PROV_STATUS_SIZE;
    const decoder = new TextDecoder();
    const ssidLen = bytes[offset++];
    reply.ssid = decoder.decode(bytes.subarray(offset, offset + ssidLen));
    offset += ssidLen;
    const hostLen = bytes[offset++];
    reply.host = decoder.decode(bytes.subarray(offset, offset + hostLen));
    return reply;
  }

  // A session number for a ROM transfer.
  //
  // Never from the clock. The firmware treats a BEGIN carrying a session it is
  // already running as a retransmission and answers OK without taking the new
  // size and CRC, so two transfers that pick the same number corrupt each other
  // (cli/src/rom_client.rs:344). 0 is reserved for "no session".
  function newSession() {
    const bytes = new Uint16Array(1);
    for (;;) {
      crypto.getRandomValues(bytes);
      const usable = bytes[0] !== 0;
      if (usable) return bytes[0];
    }
  }

  // Validate the image the way the firmware will, so an obviously bad file is
  // refused before 1MB goes down the wire.
  function looksLikeInes(bytes) {
    const tooSmall = bytes.length < 16;
    if (tooSmall) return false;
    return bytes[0] === 0x4e && bytes[1] === 0x45 && bytes[2] === 0x53 && bytes[3] === 0x1a;
  }

  // Which ports to offer in the browser's chooser.
  //
  // Vendor only. 0x303a is Espressif, and that is the part worth filtering on:
  // without it the chooser lists every serial device on the machine — Bluetooth
  // headphones, speakers, macOS's own cu.debug-console — and the user has to
  // know which one is the board.
  //
  // The product id is deliberately left out. A CoreS3 presents different ones
  // depending on what it is running (0x1001 for the ROM bootloader's USB
  // JTAG/serial, and whatever the app's CDC claims once the firmware is up),
  // and pinning the pair turned the chooser into "対応デバイスが見つからない"
  // for a board that was sitting right there.
  const PORT_FILTERS = [{ usbVendorId: 0x303a }];

  window.NesProto = {
    PORT_FILTERS,
    ROM_CHUNK,
    ROM_MAX_SIZE,
    ROM_FLAG_SWAP,
    ROM_FLAG_SAVE_SD,
    ROM_FLAG_NO_LOAD,
    ROM_OP_BEGIN,
    ROM_OP_DATA,
    ROM_OP_END,
    ROM_OP_ABORT,
    ROM_STATUS_OK,
    ROM_STATUS_SEQ,
    ROM_TIMEOUT_MS,
    ROM_RETRIES,
    ROM_MAX_TOTAL_RETRIES,
    ROM_SAVE_TIMEOUT_MS,
    SD_OP_LIST,
    SD_OP_LOAD,
    SD_OP_DELETE,
    SD_OP_RENAME,
    SD_STATUS_OK,
    SD_STATUS_BUSY,
    SD_TIMEOUT_MS,
    SD_BUSY_DEADLINE_MS,
    SD_BUSY_RETRY_MS,
    SD_IDEMPOTENT_ATTEMPTS,
    PROV_OP_SET,
    PROV_OP_STATUS,
    PROV_OP_APPLY,
    SERIAL_CRC_SIZE,
    CTRL_RESET,
    CTRL_VOLUME,
    crc32,
    crc16,
    cobsEncode,
    cobsDecode,
    buildPins,
    buildCtrl,
    buildRomBegin,
    buildRomData,
    buildRomMark,
    buildSd,
    buildProv,
    parseRomAck,
    parseRomSaveEvent,
    parseSdAck,
    parseSdListPart,
    parseProvReply,
    newSession,
    looksLikeInes,
    concat,
  };
})();

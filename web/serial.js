'use strict';

// The Web Serial transport: the same operations tools/serve_web.py performs
// over UDP, driven straight from the page over USB.
//
// Why this exists at all: a browser cannot send UDP, so every device feature in
// the web UI used to require `just serve` running locally. Hosting the page on
// GitHub Pages leaves no relay to run, and USB is the only device transport a
// browser can open by itself.
//
// The control logic here mirrors cli/src/{rom_client,sd_client}.rs rather than
// the Python relay, because the Rust client is the one that gets the retry
// semantics right — see the notes on chunk echoes and on non-idempotent SD ops.

(() => {
  const P = window.NesProto;

  // Replies are matched to callers by reply magic.
  //
  // A single FIFO of waiters is not enough, even though each individual caller
  // is stop-and-wait: the debug poll runs on its own 5Hz timer and overlaps
  // whatever else is in flight. With one queue the frames are handed out in
  // arrival order rather than by what each caller asked for, so a debug
  // snapshot lands in the ROM transfer's ACK check and the transfer stalls
  // until it times out — while the poll gets an ACK it cannot parse.
  //
  // Keyed on the first two bytes because that is what distinguishes the reply
  // families the device sends: 'NR' ROM ack, 'NS' SD ack and ROM save event,
  // 'ND' debug snapshot part, 'NW' provisioning. Within a family the callers'
  // own session/seq checks still apply.
  const REPLY_KEY = (frame) => String.fromCharCode(frame[0], frame[1]);

  class SerialLink {
    constructor(port) {
      this.port = port;
      this.reader = null;
      this.writer = null;
      // Both keyed by reply magic; see REPLY_KEY.
      this.pending = new Map();
      this.waiters = new Map();
      this.closed = false;
      this.logLines = [];
    }

    async open(baudRate = 115200) {
      await this.port.open({ baudRate, bufferSize: 8192 });
      this.writer = this.port.writable.getWriter();
      this.reader = this.port.readable.getReader();
      this.readLoop();
    }

    // Accumulate bytes and split on the COBS delimiter. Anything between
    // delimiters that does not decode, or fails its CRC, is log output rather
    // than a frame — the firmware writes both to this line on purpose, and
    // discarding non-frames is exactly how the two coexist.
    async readLoop() {
      let buffer = new Uint8Array(0);
      let logBuffer = '';
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          const merged = new Uint8Array(buffer.length + value.length);
          merged.set(buffer);
          merged.set(value, buffer.length);
          buffer = merged;

          for (;;) {
            const end = buffer.indexOf(0x00);
            const noFrameYet = end < 0;
            if (noFrameYet) break;
            const encoded = buffer.subarray(0, end);
            buffer = buffer.subarray(end + 1);
            if (encoded.length === 0) continue;

            const decoded = P.cobsDecode(encoded);
            const framed = decoded && decoded.length > P.SERIAL_CRC_SIZE;
            if (!framed) {
              logBuffer += new TextDecoder().decode(encoded);
              const lines = logBuffer.split('\n');
              logBuffer = lines.pop() ?? '';
              for (const line of lines) this.noteLog(line);
              continue;
            }
            const payloadLen = decoded.length - P.SERIAL_CRC_SIZE;
            const want = decoded[payloadLen] | (decoded[payloadLen + 1] << 8);
            const intact = want === P.crc16(decoded, payloadLen);
            if (!intact) continue;
            this.deliver(decoded.subarray(0, payloadLen));
          }
        }
      } catch (_) {
        // A disconnect surfaces here; close() is what the UI reacts to.
      }
      this.closed = true;
      this.failWaiters();
    }

    failWaiters() {
      for (const queue of this.waiters.values()) {
        for (const waiter of queue) waiter.reject(new Error('serial closed'));
      }
      this.waiters.clear();
    }

    noteLog(line) {
      const empty = line.trim().length === 0;
      if (empty) return;
      // Kept for the flasher's console pane. Bounded so a long session cannot
      // grow this without limit.
      this.logLines.push(line);
      if (this.logLines.length > 500) this.logLines.shift();
    }

    deliver(frame) {
      const copy = frame.slice();
      const key = REPLY_KEY(copy);
      const queue = this.waiters.get(key);
      const waiting = queue && queue.length > 0;
      if (waiting) {
        queue.shift().resolve(copy);
        return;
      }
      // An unclaimed reply is almost always a late answer to a request that has
      // already timed out. Keeping a few lets a caller that arrives immediately
      // after still find it; keeping all of them would leak.
      const buffered = this.pending.get(key) ?? [];
      buffered.push(copy);
      if (buffered.length > 8) buffered.shift();
      this.pending.set(key, buffered);
    }

    async send(bytes) {
      const gone = this.closed || !this.writer;
      if (gone) throw new Error('serial closed');
      const framed = new Uint8Array(bytes.length + P.SERIAL_CRC_SIZE);
      framed.set(bytes);
      const crc = P.crc16(bytes, bytes.length);
      framed[bytes.length] = crc & 0xff;
      framed[bytes.length + 1] = (crc >> 8) & 0xff;
      const encoded = P.cobsEncode(framed);
      const out = new Uint8Array(encoded.length + 1);
      out.set(encoded);
      out[encoded.length] = 0x00;
      await this.writer.write(out);
    }

    // Wait for one reply of the given kind, or null on timeout. `kind` is the
    // two-character reply magic. Callers still filter by session/seq themselves,
    // because "is this mine" within a family differs per message type.
    receive(timeoutMs, kind) {
      const buffered = this.pending.get(kind);
      const haveOne = buffered && buffered.length > 0;
      if (haveOne) return Promise.resolve(buffered.shift());
      const gone = this.closed;
      if (gone) return Promise.reject(new Error('serial closed'));
      return new Promise((resolve, reject) => {
        const waiter = { resolve, reject };
        const queue = this.waiters.get(kind) ?? [];
        queue.push(waiter);
        this.waiters.set(kind, queue);
        setTimeout(() => {
          const index = queue.indexOf(waiter);
          const stillWaiting = index >= 0;
          if (stillWaiting) {
            queue.splice(index, 1);
            resolve(null);
          }
        }, timeoutMs);
      });
    }

    async close() {
      this.closed = true;
      // Wake anyone waiting, rather than leaving them to their own timeouts:
      // an explicit close does not necessarily run readLoop's exit path, and a
      // caller that has just been disconnected should learn it now instead of
      // in three seconds. Rejecting rather than resolving null keeps "the link
      // went away" distinct from "the device did not answer" — the retry rules
      // treat those differently.
      this.failWaiters();
      try {
        await this.reader?.cancel();
      } catch (_) {}
      try {
        this.reader?.releaseLock();
      } catch (_) {}
      try {
        this.writer?.releaseLock();
      } catch (_) {}
      try {
        await this.port.close();
      } catch (_) {}
    }
  }

  // ------------------------------------------------------------ ROM transfer

  // Send one packet and wait for the ACK that actually answers it.
  //
  // The chunk number is checked, not just the session. Without that check a
  // late ACK for the previous chunk is taken as the answer to this one, the
  // transfer runs one behind for the rest of the image, and the failure only
  // surfaces at END as SIZE_MISMATCH — far from its cause. The Python relay
  // does not make this check; the Rust client does, and this follows the Rust
  // client (cli/src/rom_client.rs:284).
  async function sendUntilAck(link, packet, session, chunk, budget) {
    for (let attempt = 0; attempt < P.ROM_RETRIES; attempt++) {
      const exhausted = budget.total >= P.ROM_MAX_TOTAL_RETRIES;
      if (exhausted) throw new Error('rom: too many retransmissions');
      if (attempt > 0) budget.total++;
      await link.send(packet);

      const deadline = Date.now() + P.ROM_TIMEOUT_MS;
      for (;;) {
        const left = deadline - Date.now();
        const expired = left <= 0;
        if (expired) break;
        const frame = await link.receive(left, 'NR');
        if (!frame) break;
        const ack = P.parseRomAck(frame);
        // An unrelated frame must not consume the deadline's remainder, so the
        // loop continues rather than restarting the timer.
        const mine = ack && ack.session === session && ack.chunk === chunk;
        if (mine) return ack;
      }
    }
    throw new Error('rom: no answer from the device');
  }

  // Progress is thinned the way serve_web.py thins its NDJSON stream: a repaint
  // per 1.4KB chunk would dominate the transfer on a fast link.
  function thinner(onProgress, total) {
    let lastFraction = 0;
    let lastAt = 0;
    return (sent, force) => {
      const noCallback = !onProgress;
      if (noCallback) return;
      const fraction = total > 0 ? sent / total : 1;
      const now = Date.now();
      const due = force || fraction - lastFraction >= 0.03 || now - lastAt >= 50;
      if (!due) return;
      lastFraction = fraction;
      lastAt = now;
      onProgress(sent, total);
    };
  }

  async function sendRom(link, bytes, opts = {}, onProgress) {
    const tooBig = bytes.length > P.ROM_MAX_SIZE;
    if (tooBig) throw new Error('rom: image is larger than the device accepts');
    const notARom = !P.looksLikeInes(bytes);
    if (notARom) throw new Error('rom: not an iNES image');

    let flags = 0;
    if (opts.swap) flags |= P.ROM_FLAG_SWAP;
    if (opts.save) flags |= P.ROM_FLAG_SAVE_SD;
    if (opts.noLoad) flags |= P.ROM_FLAG_NO_LOAD;

    const session = P.newSession();
    const crc = P.crc32(bytes);
    const budget = { total: 0 };
    const report = thinner(onProgress, bytes.length);

    const begin = P.buildRomBegin(session, flags, bytes.length, crc, opts.save);
    const beginAck = await sendUntilAck(link, begin, session, 0, budget);
    const beginRefused = beginAck.status !== P.ROM_STATUS_OK;
    if (beginRefused) return { ok: false, status: beginAck.status, stage: 'begin' };

    report(0, true);
    let index = 0;
    let sent = 0;
    while (index * P.ROM_CHUNK < bytes.length) {
      const start = index * P.ROM_CHUNK;
      const payload = bytes.subarray(start, Math.min(start + P.ROM_CHUNK, bytes.length));
      const ack = await sendUntilAck(link, P.buildRomData(session, index, payload), session, index, budget);

      const outOfStep = ack.status === P.ROM_STATUS_SEQ;
      if (outOfStep) {
        // The device says which chunk it wants. Rewinding is normal after a lost
        // packet; not converging means something else is wrong, and the retry
        // budget is what stops it spinning.
        index = ack.expected;
        sent = index * P.ROM_CHUNK;
        budget.total++;
        continue;
      }
      const refused = ack.status !== P.ROM_STATUS_OK;
      if (refused) return { ok: false, status: ack.status, stage: 'data' };

      sent += payload.length;
      index++;
      report(sent, false);
    }
    report(bytes.length, true);

    const endAck = await sendUntilAck(link, P.buildRomMark(session, P.ROM_OP_END), session, 0, budget);
    const endRefused = endAck.status !== P.ROM_STATUS_OK;
    if (endRefused) return { ok: false, status: endAck.status, stage: 'end' };

    const wantsSave = (flags & P.ROM_FLAG_SAVE_SD) !== 0;
    if (!wantsSave) return { ok: true, status: P.ROM_STATUS_OK };

    // The card write happens on the emulation core at a frame boundary and can
    // take a second or two, so its outcome arrives as its own message well after
    // the END ACK (config.h:404-409).
    const deadline = Date.now() + P.ROM_SAVE_TIMEOUT_MS;
    for (;;) {
      const left = deadline - Date.now();
      const expired = left <= 0;
      // Silence here is "undetermined", not "failed": the image may well be on
      // the card. Saying it failed would be a claim we cannot support.
      if (expired) return { ok: true, status: P.ROM_STATUS_OK, save: { unknown: true } };
      // 'NS' also carries SD acks; parseRomSaveEvent checks byte 3 for the ROM
      // type, and the session check below rejects anything else.
      const frame = await link.receive(left, 'NS');
      if (!frame) continue;
      const event = P.parseRomSaveEvent(frame);
      const mine = event && event.session === session;
      if (!mine) continue;
      const saved = event.status === P.SD_STATUS_OK;
      return { ok: saved, status: P.ROM_STATUS_OK, save: { status: event.status } };
    }
  }

  // -------------------------------------------------------------- SD commands

  // Whether an unanswered request may be repeated is a property of the operation
  // and not of the caller, so it is decided here (cli/src/sd_client.rs:74).
  //
  // The firmware caches no per-seq result, so resending a DELETE that actually
  // succeeded gets NOT_FOUND back and turns a success into a reported failure.
  // LIST and LOAD can be repeated freely.
  function retriesFor(op) {
    const idempotent = op === P.SD_OP_LIST || op === P.SD_OP_LOAD;
    return idempotent ? P.SD_IDEMPOTENT_ATTEMPTS : 1;
  }

  let sdSeq = 0;

  async function sdCommand(link, op, args = {}) {
    const attempts = retriesFor(op);
    const busyDeadline = Date.now() + P.SD_BUSY_DEADLINE_MS;
    const idempotent = attempts > 1;

    for (let attempt = 0; attempt < attempts;) {
      sdSeq = (sdSeq + 1) & 0xffff;
      const seq = sdSeq;
      await link.send(P.buildSd(seq, op, args));

      const collected = new Map();
      let capacity = null;
      const deadline = Date.now() + P.SD_TIMEOUT_MS;
      let answered = false;

      for (;;) {
        const left = deadline - Date.now();
        const expired = left <= 0;
        if (expired) break;
        const frame = await link.receive(left, 'NS');
        if (!frame) break;

        const listing = op === P.SD_OP_LIST;
        if (listing) {
          const part = P.parseSdListPart(frame);
          const mine = part && part.seq === seq;
          if (!mine) continue;
          const refused = part.status !== P.SD_STATUS_OK;
          if (refused) {
            answered = true;
            const busy = part.status === P.SD_STATUS_BUSY;
            // Busy is an answer, not a failure: the device takes one request at
            // a time. Waiting it out must not consume an attempt, or a slow card
            // write would exhaust the budget while nothing is wrong.
            if (busy && Date.now() < busyDeadline) {
              await new Promise((r) => setTimeout(r, P.SD_BUSY_RETRY_MS));
              break;
            }
            return { ok: false, status: part.status };
          }
          collected.set(part.part, part);
          capacity ??= { totalBytes: part.totalBytes, freeBytes: part.freeBytes };
          const complete = collected.size === part.nparts;
          if (!complete) continue;
          // Assembled only when every part is in hand. A short listing is worse
          // than none: the user would act on a library that looks smaller than
          // it is (cli/src/transport.rs:305).
          const entries = [];
          for (let i = 0; i < part.nparts; i++) entries.push(...collected.get(i).entries);
          return { ok: true, status: P.SD_STATUS_OK, entries, ...capacity };
        }

        const ack = P.parseSdAck(frame);
        const mine = ack && ack.seq === seq && ack.op === op;
        if (!mine) continue;
        answered = true;
        const busy = ack.status === P.SD_STATUS_BUSY;
        if (busy && idempotent && Date.now() < busyDeadline) {
          await new Promise((r) => setTimeout(r, P.SD_BUSY_RETRY_MS));
          break;
        }
        return { ok: ack.status === P.SD_STATUS_OK, status: ack.status };
      }

      // Only a genuine silence costs an attempt; a busy reply loops without
      // spending one.
      if (!answered) attempt++;
    }

    // A non-idempotent request that was never answered is reported as
    // undetermined rather than failed — it may well have been carried out, and
    // the one thing that must not happen is sending it again.
    const undetermined = !idempotent;
    return { ok: false, unknown: undetermined, status: null };
  }

  // ------------------------------------------------------------------ debug

  let debugSeq = 0;

  // Ask for a snapshot and reassemble it.
  //
  // The reply is split because it runs to ~3.8KB with the scope rows, and the
  // parts are only useful together — a snapshot missing its middle would be
  // rendered as if it were whole. A dropped part therefore discards the attempt
  // rather than returning something partly stale, the same rule the SD listing
  // follows.
  async function fetchDebug(link, wantWaves) {
    debugSeq = (debugSeq + 1) & 0xffff;
    const seq = debugSeq;
    await link.send(P.buildDebug(seq, wantWaves));

    const parts = new Map();
    let expected = null;
    const deadline = Date.now() + P.DEBUG_TIMEOUT_MS;
    for (;;) {
      const left = deadline - Date.now();
      const expired = left <= 0;
      if (expired) return null;
      const frame = await link.receive(left, 'ND');
      if (!frame) return null;
      const part = P.parseDebugPart(frame);
      // A late answer to an abandoned poll carries an older seq; ignoring it
      // keeps this reply from being assembled out of two different snapshots.
      const mine = part && part.seq === seq;
      if (!mine) continue;
      // part and nparts are raw bytes off the wire, so nothing but this bounds
      // them. Counting a part >= nparts toward the completion check would call
      // the reply finished with a hole still in it, and the assembly below
      // would then read an absent index.
      const sane = part.nparts > 0 && part.nparts <= P.DEBUG_MAX_PARTS && part.part < part.nparts;
      if (!sane) continue;
      // Two replies disagreeing about the count means frames from different
      // snapshots reached here; assembling across them would splice one picture
      // out of two moments.
      const disagrees = expected !== null && expected !== part.nparts;
      if (disagrees) return null;
      expected = part.nparts;

      parts.set(part.part, part.payload);
      const complete = parts.size === part.nparts;
      if (!complete) continue;

      let total = 0;
      for (const payload of parts.values()) total += payload.length;
      const out = new Uint8Array(total);
      let offset = 0;
      for (let i = 0; i < part.nparts; i++) {
        const chunk = parts.get(i);
        out.set(chunk, offset);
        offset += chunk.length;
      }
      return out.buffer;
    }
  }

  // ------------------------------------------------------------ provisioning

  let provSeq = 0;

  async function provision(link, op, args = {}) {
    provSeq = (provSeq + 1) & 0xffff;
    const seq = provSeq;
    await link.send(P.buildProv(seq, op, args));
    const deadline = Date.now() + P.SD_TIMEOUT_MS;
    for (;;) {
      const left = deadline - Date.now();
      const expired = left <= 0;
      if (expired) return null;
      const frame = await link.receive(left, 'NW');
      if (!frame) return null;
      const reply = P.parseProvReply(frame);
      const mine = reply && reply.seq === seq;
      if (mine) return reply;
    }
  }

  // The page's one link to the device.
  //
  // Deliberately shared: a SerialPort can only be open once, so main.js (pins,
  // volume, ROM, SD) and flash.js (flashing, WiFi) must not each hold their own
  // — opening the second fails with "The port is already open", which is
  // exactly the trap this API exists to close. Whoever connects first publishes
  // it here and both read it from the same place.
  let current = null;
  const listeners = new Set();

  function setLink(link) {
    current = link;
    for (const listener of listeners) listener(link);
  }

  window.NesSerial = {
    supported: () => typeof navigator !== 'undefined' && 'serial' in navigator,
    SerialLink,
    sendRom,
    sdCommand,
    provision,
    fetchDebug,

    // Open the shared link, or hand back the one already open.
    async connect() {
      if (current && !current.closed) return current;
      const port = await navigator.serial.requestPort({ filters: P.PORT_FILTERS });
      const link = new SerialLink(port);
      await link.open();
      setLink(link);
      return link;
    },
    async disconnect() {
      await current?.close();
      setLink(null);
    },
    link: () => (current && !current.closed ? current : null),
    // Called whenever the link appears or goes away, so each panel can show the
    // rows it gates on connection without polling for it.
    onChange(listener) {
      listeners.add(listener);
    },
  };
})();

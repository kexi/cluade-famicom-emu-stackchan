'use strict';

// Firmware flashing and WiFi provisioning, as a panel on the emulator page.
//
// This began as its own page and was folded in here: it is the same device the
// rest of the page already talks to, so a second design to keep in step bought
// nothing. The panel borrows #swap-panel's shell and the console's colours.
//
// esptool-js is imported lazily from a CDN — only when the user actually starts
// a flash — because this repo has no JS build step at all (web/*.js is served as
// written, see build.sh) and most visitors never flash anything. A failed import
// is reported rather than left hanging.

(() => {
  const ESPTOOL_URL = 'https://cdn.jsdelivr.net/npm/esptool-js@0.5.4/+esm';

  // Flash offsets for the ESP32-S3, matching what `pio run -t upload` writes.
  // Derived from PlatformIO's own build script rather than guessed: the
  // bootloader sits at 0x0 on the S3 (0x1000 is the ESP32 value and would brick
  // the boot), partitions at 0x8000, boot_app0 at 0xe000, the app at 0x10000.
  const IMAGES = [
    { path: 'firmware/bootloader.bin', offset: 0x0000 },
    { path: 'firmware/partitions.bin', offset: 0x8000 },
    { path: 'firmware/boot_app0.bin', offset: 0xe000 },
    { path: 'firmware/firmware.bin', offset: 0x10000 },
  ];

  const $ = (id) => document.getElementById(id);
  const P = window.NesProto;
  const S = window.NesSerial;

  // The link is owned by NesSerial so this panel and the emulator share one
  // port; see the note there.
  const link = () => S.link();

  function say(el, text, cls) {
    el.textContent = text;
    el.className = 'status' + (cls ? ' ' + cls : '');
  }

  function log(line) {
    const box = $('flash-log');
    box.textContent += line + '\n';
    box.scrollTop = box.scrollHeight;
  }

  // ------------------------------------------------------------- flashing

  async function loadImages() {
    const loaded = [];
    for (const image of IMAGES) {
      const res = await fetch(image.path);
      const missing = !res.ok;
      if (missing) throw new Error(`${image.path} を取得できません (${res.status})`);
      const buffer = new Uint8Array(await res.arrayBuffer());
      // esptool-js takes the image as a binary string, not bytes.
      let binary = '';
      for (let i = 0; i < buffer.length; i++) binary += String.fromCharCode(buffer[i]);
      loaded.push({ data: binary, address: image.offset });
    }
    return loaded;
  }

  // Hand the port back to the browser so the next step can open it.
  //
  // Each teardown is attempted independently: a reader that was already
  // released must not stop the writer from being, and neither must stop the
  // close. Everything here is best-effort — the port may simply be gone,
  // which is also a fine outcome for a function whose job is "leave it closed".
  async function releasePort(transport) {
    const nothingToDo = !transport;
    if (nothingToDo) return;
    try {
      await transport.disconnect();
    } catch (_) {
      /* esptool may already have torn it down */
    }
    const port = transport.device;
    if (!port) return;
    // esptool's own helper waits for the reader and writer locks to be given up
    // rather than tearing the streams down underneath whoever holds them. Its
    // argument is a poll interval, NOT a deadline: a lock that is never released
    // makes it wait forever, which freezes the page rather than the port. Raced
    // against a real timeout so the cleanup always finishes.
    const unlocked = transport.waitForUnlock?.(100) ?? Promise.resolve();
    const timeout = new Promise((resolve) => setTimeout(resolve, 1500));
    try {
      await Promise.race([unlocked, timeout]);
    } catch (_) {
      /* not present in this build, or already unlocked */
    }
    try {
      await port.close();
    } catch (_) {
      /* already closed, or still locked — the unplug hint covers this */
    }
  }

  async function flash() {
    const status = $('flash-status');
    const progress = $('flash-progress');
    $('flash-btn').disabled = true;
    // Declared out here so the cleanup below can reach it. Without that, a
    // failure anywhere after the port is opened leaves it open, and every later
    // attempt dies with "The port is already open" — a state the user can only
    // clear by unplugging the board, with nothing on screen to say so.
    let transport = null;
    try {
      say(status, 'esptool を読み込んでいます...');
      const esptool = await import(/* @vite-ignore */ ESPTOOL_URL).catch(() => null);
      const unavailable = !esptool;
      if (unavailable) throw new Error('esptool-js を読み込めませんでした (ネットワークを確認してください)');

      say(status, 'ファームウェアを取得しています...');
      const fileArray = await loadImages();
      const bytes = fileArray.reduce((n, f) => n + f.data.length, 0);
      $('flash-fw-info').textContent = `${(bytes / 1024).toFixed(0)} KB`;

      say(status, 'ポートを選んでください...');
      const port = await navigator.serial.requestPort({ filters: P.PORT_FILTERS });
      // The port is handed over unopened: ESPLoader.main() opens it itself, at
      // romBaudrate first and then again at the negotiated rate. Opening it here
      // as well fails the second one with "The port is already open".
      transport = new esptool.Transport(port, true);
      const loader = new esptool.ESPLoader({
        transport,
        baudrate: 921600,
        romBaudrate: 115200,
        terminal: { clean() {}, writeLine: (d) => log(d), write: () => {} },
      });

      say(status, '実機に接続しています...');
      const chip = await loader.main();
      log(`chip: ${chip}`);

      const wipe = $('flash-erase').checked;
      if (wipe) {
        say(status, '全消去しています (時間がかかります)...', 'warn');
        await loader.eraseFlash();
      }

      progress.hidden = false;
      say(status, '書き込んでいます...');
      await loader.writeFlash({
        fileArray,
        flashSize: '16MB',
        flashMode: 'qio',
        flashFreq: '80m',
        eraseAll: false,
        compress: true,
        reportProgress: (index, written, total) => {
          const share = (index + written / total) / fileArray.length;
          progress.value = share;
        },
      });
      progress.value = 1;

      await loader.after();

      say(status, '書き込み完了。Step 2 で接続し直してください。', 'ok');
    } catch (err) {
      // A dismissed port chooser is a decision, not a fault.
      const dismissed = err && err.name === 'NotFoundError';
      if (dismissed) {
        say(status, '');
      } else {
        // A port left open by an earlier attempt cannot be reclaimed from here
        // (the lock belongs to a stream this page no longer holds), so the
        // message has to name the one thing that does clear it.
        const stuck = /already open|locked stream/i.test(err.message);
        say(
          status,
          stuck ? `失敗: ${err.message} — USB を抜き差ししてからやり直してください。` : `失敗: ${err.message}`,
          'err',
        );
        console.warn('[flash]', err);
      }
    } finally {
      // Always, on both paths: the success path has to release the port too, or
      // Step 2 cannot open the very port it just flashed.
      //
      // disconnect() alone is not enough. It stops esptool using the port but
      // leaves the reader and writer locks held, so the subsequent close()
      // fails with "Cannot cancel a locked stream" and the port stays open —
      // which then fails Step 2, and every later attempt, until the board is
      // physically unplugged. Release the locks first, then close.
      await releasePort(transport);
      $('flash-btn').disabled = false;
    }
  }

  // ------------------------------------------------------------ connecting

  function setConnected(connected) {
    const btn = $('btn-stackchan');
    btn.classList.toggle('sc-on', connected);
    btn.classList.toggle('sc-off', !connected);
    $('flash-wifi-btn').disabled = !connected;
    $('flash-status-btn').disabled = !connected;
  }

  async function connect() {
    const status = $('flash-conn-status');
    try {
      say(status, 'ポートを選んでください...');
      await S.connect();
      setConnected(true);
      say(status, '接続しました。', 'ok');

      // Confirm the firmware is actually answering, rather than only that the
      // port opened — a port opens fine against a board running anything.
      const reply = await S.provision(link(), P.PROV_OP_STATUS);
      const silent = !reply;
      if (silent) {
        say(status, '接続しましたが、実機からの応答がありません。Step 1 で書き込みましたか?', 'warn');
        return;
      }
      showStatus(reply);
    } catch (err) {
      const stuck = /already open|locked stream/i.test(err.message);
      say(
        status,
        stuck ? `失敗: ${err.message} — USB を抜き差ししてからやり直してください。` : `失敗: ${err.message}`,
        'err',
      );
      setConnected(false);
    }
  }

  async function disconnect() {
    await S.disconnect();
    setConnected(false);
    say($('flash-conn-status'), '切断しました。');
  }

  // ----------------------------------------------------------------- WiFi

  function showStatus(reply) {
    const box = $('flash-wifi-status');
    const online = reply.connected;
    if (online) {
      say(box, `接続中: ${reply.ssid} — ${reply.ip} (${reply.host}.local)`, 'ok');
      return;
    }
    const configured = reply.ssid && reply.ssid.length > 0;
    say(box, configured ? `未接続 (設定済み: ${reply.ssid})` : '未設定', 'warn');
  }

  async function saveWifi() {
    const box = $('flash-wifi-status');
    const ssid = $('flash-ssid').value.trim();
    const empty = ssid.length === 0;
    if (empty) {
      say(box, 'SSID を入力してください。', 'warn');
      return;
    }
    $('flash-wifi-btn').disabled = true;
    try {
      say(box, '保存しています...');
      const saved = await S.provision(link(), P.PROV_OP_SET, { ssid, pass: $('flash-pass').value });
      const noAnswer = !saved;
      if (noAnswer) throw new Error('応答がありません');
      const refused = saved.status !== 0;
      if (refused) throw new Error(`拒否されました (status=${saved.status})`);

      say(box, '接続しています (最大 15 秒)...');
      await S.provision(link(), P.PROV_OP_APPLY);
      // APPLY answers immediately and connects afterwards, so the outcome has to
      // be read back rather than inferred from the ack.
      await new Promise((r) => setTimeout(r, 16000));
      const reply = await S.provision(link(), P.PROV_OP_STATUS);
      if (!reply) throw new Error('状態を取得できません');
      showStatus(reply);
    } catch (err) {
      say(box, `失敗: ${err.message}`, 'err');
    } finally {
      $('flash-wifi-btn').disabled = false;
    }
  }

  async function readStatus() {
    const reply = await S.provision(link(), P.PROV_OP_STATUS);
    if (!reply) {
      say($('flash-wifi-status'), '応答がありません。', 'err');
      return;
    }
    showStatus(reply);
  }

  // ----------------------------------------------------------------- setup

  // Without Web Serial there is nothing here to offer, so the button never
  // appears rather than appearing and then explaining itself. Safari, iOS and
  // Android all land here.
  const usable = S.supported();
  if (!usable) return;

  const openBtn = $('btn-flash');
  const panel = $('flash-panel');
  const connBtn = $('btn-stackchan');
  openBtn.hidden = false;
  connBtn.hidden = false;
  openBtn.addEventListener('click', () => panel.classList.toggle('show'));
  $('flash-close').addEventListener('click', () => panel.classList.remove('show'));

  // One button for both directions: with the lamp showing which state it is in,
  // a separate "disconnect" would be a second control for the same fact.
  connBtn.addEventListener('click', () => (S.link() ? disconnect() : connect()));

  $('flash-btn').addEventListener('click', flash);
  $('flash-wifi-btn').addEventListener('click', saveWifi);
  $('flash-status-btn').addEventListener('click', readStatus);
})();

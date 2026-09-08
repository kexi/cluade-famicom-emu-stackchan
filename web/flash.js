'use strict';

// The browser flasher: writes the firmware over USB, then configures WiFi and
// pushes a ROM onto the SD card, all from a static page.
//
// esptool-js is loaded lazily from a CDN rather than bundled, because this repo
// has no JS build step at all (web/*.js is served as written, see build.sh) and
// adding one for a single page that most visitors never open would be a poor
// trade. The import failing is handled: the page says so instead of hanging.

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

  let link = null;

  function say(el, text, cls) {
    el.textContent = text;
    el.className = 'status' + (cls ? ' ' + cls : '');
  }

  function log(line) {
    const box = $('log');
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

  async function flash() {
    const status = $('flash-status');
    const progress = $('flash-progress');
    $('flash-btn').disabled = true;
    try {
      say(status, 'esptool を読み込んでいます...');
      const esptool = await import(/* @vite-ignore */ ESPTOOL_URL).catch(() => null);
      const unavailable = !esptool;
      if (unavailable) throw new Error('esptool-js を読み込めませんでした (ネットワークを確認してください)');

      say(status, 'ファームウェアを取得しています...');
      const fileArray = await loadImages();
      const bytes = fileArray.reduce((n, f) => n + f.data.length, 0);
      $('fw-info').textContent = `${(bytes / 1024).toFixed(0)} KB`;

      say(status, 'ポートを選んでください...');
      const port = await navigator.serial.requestPort();
      // The bootloader ROM speaks at a fixed rate; the transfer rate is
      // negotiated separately by esptool once it is talking.
      await port.open({ baudRate: 115200 });

      const transport = new esptool.Transport(port, true);
      const loader = new esptool.ESPLoader({
        transport,
        baudrate: 921600,
        romBaudrate: 115200,
        terminal: { clean() {}, writeLine: (d) => log(d), write: () => {} },
      });

      say(status, '実機に接続しています...');
      const chip = await loader.main();
      log(`chip: ${chip}`);

      const wipe = $('erase').checked;
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
      await transport.disconnect();

      say(status, '書き込み完了。Step 2 で接続し直してください。', 'ok');
    } catch (err) {
      say(status, `失敗: ${err.message}`, 'err');
    } finally {
      $('flash-btn').disabled = false;
    }
  }

  // ------------------------------------------------------------ connecting

  function setConnected(connected) {
    $('disconnect-btn').disabled = !connected;
    $('wifi-btn').disabled = !connected;
    $('status-btn').disabled = !connected;
    $('rom-btn').disabled = !connected;
    $('list-btn').disabled = !connected;
  }

  async function connect() {
    const status = $('conn-status');
    try {
      say(status, 'ポートを選んでください...');
      const port = await navigator.serial.requestPort();
      link = new S.SerialLink(port);
      await link.open();
      setConnected(true);
      say(status, '接続しました。', 'ok');

      // Confirm the firmware is actually answering, rather than only that the
      // port opened — a port opens fine against a board running anything.
      const reply = await S.provision(link, P.PROV_OP_STATUS);
      const silent = !reply;
      if (silent) {
        say(status, '接続しましたが、実機からの応答がありません。Step 1 で書き込みましたか?', 'warn');
        return;
      }
      showStatus(reply);
    } catch (err) {
      say(status, `失敗: ${err.message}`, 'err');
      setConnected(false);
    }
  }

  async function disconnect() {
    await link?.close();
    link = null;
    setConnected(false);
    say($('conn-status'), '切断しました。');
  }

  // ----------------------------------------------------------------- WiFi

  function showStatus(reply) {
    const box = $('wifi-status');
    const online = reply.connected;
    if (online) {
      say(box, `接続中: ${reply.ssid} — ${reply.ip} (${reply.host}.local)`, 'ok');
      return;
    }
    const configured = reply.ssid && reply.ssid.length > 0;
    say(box, configured ? `未接続 (設定済み: ${reply.ssid})` : '未設定', 'warn');
  }

  async function saveWifi() {
    const box = $('wifi-status');
    const ssid = $('ssid').value.trim();
    const empty = ssid.length === 0;
    if (empty) {
      say(box, 'SSID を入力してください。', 'warn');
      return;
    }
    $('wifi-btn').disabled = true;
    try {
      say(box, '保存しています...');
      const saved = await S.provision(link, P.PROV_OP_SET, { ssid, pass: $('pass').value });
      const noAnswer = !saved;
      if (noAnswer) throw new Error('応答がありません');
      const refused = saved.status !== 0;
      if (refused) throw new Error(`拒否されました (status=${saved.status})`);

      say(box, '接続しています (最大 15 秒)...');
      await S.provision(link, P.PROV_OP_APPLY);
      // APPLY answers immediately and connects afterwards, so the outcome has to
      // be read back rather than inferred from the ack.
      await new Promise((r) => setTimeout(r, 16000));
      const reply = await S.provision(link, P.PROV_OP_STATUS);
      if (!reply) throw new Error('状態を取得できません');
      showStatus(reply);
    } catch (err) {
      say(box, `失敗: ${err.message}`, 'err');
    } finally {
      $('wifi-btn').disabled = false;
    }
  }

  async function readStatus() {
    const reply = await S.provision(link, P.PROV_OP_STATUS);
    if (!reply) {
      say($('wifi-status'), '応答がありません。', 'err');
      return;
    }
    showStatus(reply);
  }

  // ------------------------------------------------------------------ ROM

  function sanitiseName(name) {
    // The firmware sanitises the name too and rejects anything that changes,
    // rather than silently saving under a different one. Doing the same
    // transformation here means the user sees the name that will be used
    // instead of a BadName they have to decode.
    const base = name.split(/[/\\]/).pop() ?? name;
    const cleaned = base.replace(/[^A-Za-z0-9._-]/g, '_');
    const hasExt = /\.nes$/i.test(cleaned);
    return hasExt ? cleaned.replace(/\.nes$/i, '.nes') : cleaned + '.nes';
  }

  async function sendRom() {
    const box = $('rom-status');
    const progress = $('rom-progress');
    const file = $('rom-file').files[0];
    const nothingChosen = !file;
    if (nothingChosen) {
      say(box, '.nes ファイルを選んでください。', 'warn');
      return;
    }
    $('rom-btn').disabled = true;
    progress.hidden = false;
    progress.value = 0;
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      const name = sanitiseName(file.name);
      say(box, `${name} を送っています...`);
      const result = await S.sendRom(link, bytes, { save: name, noLoad: !$('rom-boot').checked }, (sent, total) => {
        progress.value = total > 0 ? sent / total : 1;
      });
      progress.value = 1;

      const refused = !result.ok && !result.save;
      if (refused) {
        say(box, `実機が受け付けませんでした (status=${result.status}, ${result.stage})`, 'err');
        return;
      }
      const undetermined = result.save?.unknown;
      if (undetermined) {
        say(box, '送信しましたが、保存の結果が返りませんでした。一覧で確認してください。', 'warn');
        return;
      }
      const saveFailed = result.save && result.save.status !== 0;
      if (saveFailed) {
        say(box, `カードへの保存に失敗しました (status=${result.save.status})`, 'err');
        return;
      }
      say(box, `${name} を保存しました。`, 'ok');
    } catch (err) {
      say(box, `失敗: ${err.message}`, 'err');
    } finally {
      $('rom-btn').disabled = false;
    }
  }

  function formatSize(bytes) {
    const mb = Number(bytes) / (1024 * 1024);
    return mb >= 1024 ? `${(mb / 1024).toFixed(1)}GB` : `${mb.toFixed(0)}MB`;
  }

  async function listRoms() {
    const box = $('rom-status');
    $('list-btn').disabled = true;
    try {
      say(box, '一覧を取得しています...');
      const result = await S.sdCommand(link, P.SD_OP_LIST);
      const failed = !result.ok;
      if (failed) {
        say(box, `一覧を取得できません (status=${result.status})`, 'err');
        return;
      }
      const empty = result.entries.length === 0;
      say(
        box,
        empty
          ? 'SD カードに ROM はありません。'
          : `${result.entries.length} 本 — 空き ${formatSize(result.freeBytes)} / ${formatSize(result.totalBytes)}`,
        'ok',
      );
      for (const entry of result.entries) log(`SD: ${entry.name} (${Math.ceil(entry.size / 1024)}K)`);
    } catch (err) {
      say(box, `失敗: ${err.message}`, 'err');
    } finally {
      $('list-btn').disabled = false;
    }
  }

  // ----------------------------------------------------------------- setup

  const usable = S.supported();
  $('unsupported').hidden = usable;
  $('app').hidden = !usable;
  if (!usable) return;

  $('flash-btn').addEventListener('click', flash);
  $('connect-btn').addEventListener('click', connect);
  $('disconnect-btn').addEventListener('click', disconnect);
  $('wifi-btn').addEventListener('click', saveWifi);
  $('status-btn').addEventListener('click', readStatus);
  $('rom-btn').addEventListener('click', sendRom);
  $('list-btn').addEventListener('click', listRoms);
})();

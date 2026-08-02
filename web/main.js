'use strict';

(async () => {
  const canvas = document.getElementById('screen');
  const ctx = canvas.getContext('2d');
  const imageData = ctx.createImageData(256, 240);
  const statusEl = document.getElementById('status');

  const Module = await createNesModule({
    // cache-bust the .wasm fetch with the same version as the scripts
    locateFile: (path) => path + '?v=' + (window.NES_VER || '0'),
  });
  const api = {
    init: Module._nes_init,
    romBuffer: Module._nes_rom_buffer,
    loadRom: Module._nes_load_rom,
    reset: Module._nes_reset,
    powerOn: Module._nes_power_on,
    swapRom: Module._nes_swap_rom,
    frame: Module._nes_frame,
    runCycles: Module._nes_run_cycles,
    framebuffer: Module._nes_framebuffer,
    setButtons: Module._nes_set_buttons,
    audioBuffer: Module._nes_audio_buffer,
    audioBufferR: Module._nes_audio_buffer_r,
    setChannelVolume: Module._nes_set_channel_volume,
    setChannelPan: Module._nes_set_channel_pan,
    audioCount: Module._nes_audio_sample_count,
    audioClear: Module._nes_audio_clear,
    ram: Module._nes_ram,
    setPin: Module._nes_set_pin,
    getPin: Module._nes_get_pin,
    resetPins: Module._nes_reset_pins,
    renderChr: Module._nes_render_chr,
    chanBuffer: Module._nes_chan_buffer,
    setChannel: Module._nes_set_channel,
    hasExpansionAudio: Module._nes_has_expansion_audio,
    apuRegs: Module._nes_apu_regs,
    cpuRegs: Module._nes_cpu_regs,
    peek: Module._nes_peek,
    setProbe: Module._nes_set_probe,
    probeBuffer: Module._nes_probe_buffer,
    probePos: Module._nes_probe_pos,
    probeLevel: Module._nes_probe_level,
    keyCover: Module._nes_key_cover,
    keyRattle: Module._nes_key_rattle,
    keyState: Module._nes_key_state,
    sram: Module._nes_sram,
    sramSize: Module._nes_sram_size,
    hasBattery: Module._nes_has_battery,
  };
  window.__nes = { api, Module, frames: 0, getButtons: () => buttons };

  // ------------------------------------------------------------------ i18n
  // 'auto' follows the browser locale; an explicit choice is persisted
  function detectLang() {
    const n = navigator.language || '';
    return n.startsWith('ja') ? 'ja' : n.startsWith('zh') ? 'zh' : 'en';
  }
  let langPref = localStorage.getItem('lang') || 'auto';
  if (langPref !== 'auto' && !window.I18N[langPref]) langPref = 'auto';
  let lang = langPref === 'auto' ? detectLang() : langPref;
  function t(key, vars) {
    let s = (window.I18N[lang] && window.I18N[lang][key]) || window.I18N.ja[key] || key;
    if (vars) for (const k of Object.keys(vars)) s = s.split('{' + k + '}').join(vars[k]);
    return s;
  }
  function applyLanguage() {
    document.documentElement.lang = lang;
    const set = (id, key) => {
      const el = document.getElementById(id);
      if (el) el.textContent = t(key);
    };
    set('lbl-open', 'openRom');
    set('btn-power', 'power');
    set('btn-reset', 'reset');
    set('btn-swap', 'swap');
    set('btn-bus', 'bus');
    set('btn-exp', 'expBtn');
    set('btn-debug', 'debug');
    set('btn-xev', 'xevCheck');
    set('btn-tas', 'tas');
    set('bus-hint', 'busHint');
    set('cart-title-h3', 'cartTitle');
    set('cart-ccw', 'ccw');
    set('cart-cw', 'cw');
    set('cart-straight', 'reinsert');
    set('cart-blow', 'blow');
    set('cart-note', 'cartNote');
    set('exp-title-h3', 'expTitle');
    set('exp-note-h3', 'expNoteTitle');
    set('exp-note', 'expNote');
    set('exp-hint', 'expHint');
    set('exp-viewlabel', 'expViewFront');
    set('exp-rattle', 'expRattle');
    set('exp-leg-float', 'expLegFloat');
    set('exp-leg-cover', 'expLegCover');
    set('exp-leg-short', 'expLegShort');
    set('exp-leg-shell', 'expLegShell');
    set('exp-leg-out', 'expLegOut');
    set('exp-meter-h4', 'expMeterTitle');
    set('exp-m-k-key', 'expMeterKeyLabel');
    set('exp-m-k-gnd', 'expMeterGndLabel');
    set('exp-m-k-pins', 'expMeterPinsLabel');
    // The meter's values are words, so they have to be re-rendered on a language
    // change. Routed through a single hoisted helper rather than reading
    // expInserted here: this function is declared above the expansion-port block
    // and a `let` from that block would be in its temporal dead zone, where even
    // `typeof` throws.
    if (typeof refreshExpMeter === 'function') refreshExpMeter();
    // the pull button is a toggle: its label depends on where the key currently is
    if (typeof refreshExpPullLabel === 'function') refreshExpPullLabel();
    set('swap-title', 'swapTitle');
    set('lbl-swap-whole', 'swapWhole');
    set('lbl-swap-prg', 'swapPrg');
    set('lbl-swap-chr', 'swapChr');
    set('swap-note', 'swapNote');
    set('swap-url-btn', 'urlLoad');
    set('swap-device-btn', 'sendDevice');
    set('lbl-swap-noreset', 'sendDeviceNoReset');
    set('btn-settings', 'settingsBtn');
    set('settings-title', 'settingsTitle');
    set('lbl-master', 'masterVol');
    set('settings-note', 'settingsNote');
    set('settings-close', 'close');
    set('swap-close', 'close');
    set('check-close', 'close');
    set('h-cpu', 'cpuRegs');
    set('h-waves', 'apuWaves');
    set('h-mixer', 'mixer');
    set('h-apuregs', 'apuRegs');
    set('h-wram', 'wramTitle');
    set('dbg-src-local', 'dbgSrcLocal');
    set('dbg-src-remote', 'dbgSrcRemote');
    if (typeof romLoaded !== 'undefined' && !romLoaded) statusEl.textContent = t('statusDefault');
    if (typeof refreshPinTitles === 'function') refreshPinTitles();
    if (typeof refreshExpPinTitles === 'function') refreshExpPinTitles();
    if (typeof updateChrTitle === 'function') updateChrTitle();
    if (typeof updateMuteTips === 'function') updateMuteTips();
    const sel = document.getElementById('lang-select');
    if (sel) sel.value = langPref;
  }
  document.getElementById('lang-select').addEventListener('change', (e) => {
    langPref = e.target.value;
    localStorage.setItem('lang', langPref);
    lang = langPref === 'auto' ? detectLang() : langPref;
    applyLanguage();
  });

  // ------------------------------------------------------------------ audio
  let audioCtx = null;
  let audioNode = null;
  let masterGain = null;
  let muted = false;
  // Set once the device-mirror block below has been evaluated. Until then the
  // volume mirror must not be called: it reads state declared further down.
  let mirrorReady = false;
  let masterVolume = parseFloat(localStorage.getItem('masterVolume'));
  if (!(masterVolume >= 0 && masterVolume <= 1.5)) masterVolume = 1;

  // fallback ring buffer for ScriptProcessorNode (insecure contexts have no AudioWorklet)
  const fallbackRing = new Float32Array(16384 * 2); // interleaved L,R
  let fbRead = 0,
    fbWrite = 0,
    fbAvail = 0,
    fbLast = 0,
    fbLastR = 0;
  let pushSamples = null;

  function makeMasterGain() {
    masterGain = audioCtx.createGain();
    masterGain.gain.value = muted ? 0 : masterVolume;
    masterGain.connect(audioCtx.destination);
    return masterGain;
  }
  function applyMasterVolume() {
    if (masterGain) masterGain.gain.value = muted ? 0 : masterVolume;
    // Keep the device's speaker in step with the page's slider.
    //
    // refreshMaster() calls this during setup, before the mirror block further
    // down has been evaluated, so route through a flag the mirror sets once it
    // is actually ready. Calling mirrorVolume directly would touch `deviceIp`
    // in its temporal dead zone and throw.
    if (mirrorReady) mirrorVolume(muted ? 0 : masterVolume);
  }

  async function initAudio() {
    if (audioCtx) return;
    audioCtx = new AudioContext({ sampleRate: 44100 });
    if (audioCtx.audioWorklet) {
      await audioCtx.audioWorklet.addModule('audio-worklet.js');
      audioNode = new AudioWorkletNode(audioCtx, 'nes-audio', { outputChannelCount: [2] });
      audioNode.connect(makeMasterGain());
      pushSamples = (s) => audioNode.port.postMessage(s);
    } else {
      // http:// on a LAN address etc. — fall back to ScriptProcessorNode
      const sp = audioCtx.createScriptProcessor(1024, 0, 2);
      const cap = fallbackRing.length >> 1;
      sp.onaudioprocess = (e) => {
        const outL = e.outputBuffer.getChannelData(0);
        const outR = e.outputBuffer.getChannelData(1);
        for (let i = 0; i < outL.length; i++) {
          if (fbAvail > 0) {
            fbLast = fallbackRing[fbRead * 2];
            fbLastR = fallbackRing[fbRead * 2 + 1];
            fbRead = (fbRead + 1) % cap;
            fbAvail--;
          }
          outL[i] = fbLast;
          outR[i] = fbLastR;
        }
      };
      sp.connect(makeMasterGain());
      audioNode = sp;
      pushSamples = (s) => {
        const frames = s.length >> 1;
        for (let i = 0; i < frames; i++) {
          if (fbAvail >= cap) break;
          fallbackRing[fbWrite * 2] = s[i * 2];
          fallbackRing[fbWrite * 2 + 1] = s[i * 2 + 1];
          fbWrite = (fbWrite + 1) % cap;
          fbAvail++;
        }
      };
    }
    api.init(audioCtx.sampleRate);
  }
  api.init(44100); // provisional rate until AudioContext exists

  // Android Chrome requires a user gesture before audio can start
  async function resumeAudio() {
    try {
      await initAudio();
      if (audioCtx.state !== 'running') await audioCtx.resume();
    } catch (e) {
      console.warn('audio unavailable:', e);
    }
  }
  ['touchstart', 'mousedown', 'keydown'].forEach((ev) =>
    document.addEventListener(ev, resumeAudio, { once: false, passive: true }),
  );

  // ------------------------------------------------------------------ input
  let buttons = 0; // bit0:A 1:B 2:Select 3:Start 4:Up 5:Down 6:Left 7:Right

  const KEYMAP = {
    KeyX: 1,
    KeyZ: 2,
    ShiftRight: 4,
    ShiftLeft: 4,
    Enter: 8,
    ArrowUp: 16,
    ArrowDown: 32,
    ArrowLeft: 64,
    ArrowRight: 128,
  };
  function toggleFullscreen() {
    if (document.fullscreenElement) document.exitFullscreen();
    else
      document
        .getElementById('app')
        .requestFullscreen()
        .catch(() => {});
  }

  document.addEventListener('keydown', (e) => {
    if (e.code === 'KeyF' && !e.repeat) {
      toggleFullscreen();
      e.preventDefault();
      return;
    }
    if (e.code === 'KeyR' && !e.repeat) {
      setResetHold(true);
      e.preventDefault();
      return;
    }
    if (e.code === 'KeyD' && !e.repeat) {
      document.getElementById('btn-debug').click();
      e.preventDefault();
      return;
    }
    const bit = KEYMAP[e.code];
    if (bit) {
      buttons |= bit;
      e.preventDefault();
    }
  });
  document.addEventListener('keyup', (e) => {
    if (e.code === 'KeyR') {
      setResetHold(false);
      e.preventDefault();
      return;
    }
    const bit = KEYMAP[e.code];
    if (bit) {
      buttons &= ~bit;
      e.preventDefault();
    }
  });

  // virtual pad: multi-touch with slide support
  const pad = document.getElementById('pad');
  const padButtons = [...pad.querySelectorAll('.pbtn')];
  const touchBits = new Map(); // touch identifier -> bit

  function hitButton(x, y) {
    for (const el of padButtons) {
      const r = el.getBoundingClientRect();
      // generous hit margin for small screens
      if (x >= r.left - 8 && x <= r.right + 8 && y >= r.top - 8 && y <= r.bottom + 8) return el;
    }
    return null;
  }
  function refreshPadState() {
    let bits = 0;
    for (const b of touchBits.values()) bits |= b;
    for (const el of padButtons) {
      const bit = +el.dataset.bit;
      el.classList.toggle('active', !!(bits & bit));
    }
    buttons = (buttons & 0) | bits; // touch pad owns state on touch devices
  }
  function onTouch(e) {
    e.preventDefault();
    for (const t of e.changedTouches) {
      if (e.type === 'touchend' || e.type === 'touchcancel') {
        touchBits.delete(t.identifier);
      } else {
        const el = hitButton(t.clientX, t.clientY);
        if (el) touchBits.set(t.identifier, +el.dataset.bit);
        else touchBits.delete(t.identifier);
      }
    }
    refreshPadState();
  }
  ['touchstart', 'touchmove', 'touchend', 'touchcancel'].forEach((ev) =>
    pad.addEventListener(ev, onTouch, { passive: false }),
  );

  // ------------------------------------------------------------------ SRAM save
  let romKey = null;
  let sramDirty = 0;

  function saveSram() {
    if (!romKey || !api.hasBattery()) return;
    const ptr = api.sram();
    const size = api.sramSize();
    const data = Module.HEAPU8.subarray(ptr, ptr + size);
    let bin = '';
    for (let i = 0; i < size; i++) bin += String.fromCharCode(data[i]);
    try {
      localStorage.setItem('sram:' + romKey, btoa(bin));
    } catch (_) {}
  }
  function loadSram() {
    if (!romKey || !api.hasBattery()) return;
    const b64 = localStorage.getItem('sram:' + romKey);
    if (!b64) return;
    const bin = atob(b64);
    const ptr = api.sram();
    const size = Math.min(bin.length, api.sramSize());
    for (let i = 0; i < size; i++) Module.HEAPU8[ptr + i] = bin.charCodeAt(i);
  }
  window.addEventListener('pagehide', saveSram);
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden') saveSram();
  });

  // ------------------------------------------------------------------ ROM load / power
  let running = false;
  let romLoaded = false;
  let powered = false;
  const btnPower = document.getElementById('btn-power');

  // No signal: animated TV snow. Rendered at the canvas's on-screen
  // resolution rather than 256x240, so the grain stays fine instead of
  // inheriting the emulator's chunky pixels.
  let snowImage = null,
    snowView = null,
    snowW = 0,
    snowH = 0;
  let noiseSeed = 0x9e3779b9;

  function ensureSnowBuffer() {
    const r = canvas.getBoundingClientRect();
    const w = Math.max(256, Math.min(1280, Math.round(r.width) || 256));
    const h = Math.max(240, Math.min(960, Math.round(r.height) || 240));
    if (w === snowW && h === snowH && canvas.width === w) return;
    snowW = w;
    snowH = h;
    canvas.width = w;
    canvas.height = h;
    snowImage = ctx.createImageData(w, h);
    snowView = new Uint32Array(snowImage.data.buffer);
  }

  function drawStatic() {
    ensureSnowBuffer();
    const view = snowView,
      len = view.length;
    // one xorshift32 step feeds four pixels (one per byte) to keep this cheap
    for (let i = 0; i < len; i += 4) {
      noiseSeed ^= noiseSeed << 13;
      noiseSeed ^= noiseSeed >>> 17;
      noiseSeed ^= noiseSeed << 5;
      const r = noiseSeed;
      const a = 30 + (((r & 0xff) * 210) >> 8);
      const b = 30 + ((((r >>> 8) & 0xff) * 210) >> 8);
      const c = 30 + ((((r >>> 16) & 0xff) * 210) >> 8);
      const d = 30 + ((((r >>> 24) & 0xff) * 210) >> 8);
      view[i] = 0xff000000 | (a << 16) | (a << 8) | a;
      if (i + 1 < len) view[i + 1] = 0xff000000 | (b << 16) | (b << 8) | b;
      if (i + 2 < len) view[i + 2] = 0xff000000 | (c << 16) | (c << 8) | c;
      if (i + 3 < len) view[i + 3] = 0xff000000 | (d << 16) | (d << 8) | d;
    }
    ctx.putImageData(snowImage, 0, 0);
  }

  // back to the NES framebuffer resolution when the machine runs again
  function restoreScreenCanvas() {
    if (canvas.width !== 256 || canvas.height !== 240) {
      canvas.width = 256;
      canvas.height = 240;
      snowW = snowH = 0;
    }
  }

  function setPower(on) {
    powered = on;
    btnPower.classList.toggle('power-on', on);
    btnPower.classList.toggle('power-off', !on);
    running = on && romLoaded;
    if (on) restoreScreenCanvas();
    else saveSram();
  }
  btnPower.addEventListener('click', () => {
    if (!romLoaded) {
      statusEl.textContent = t('needRom');
      return;
    }
    if (powered) {
      setPower(false);
    } else {
      api.powerOn(); // 電源投入 = RAMクリア+リセット
      setPower(true);
    }
  });

  function refreshExpansionUI() {
    document.body.classList.toggle('has-expansion', !!api.hasExpansionAudio());
  }

  function keepRomCopies(buf) {
    // PRG/CHR copies for dump diagnostics
    const trainer = buf[6] & 0x04;
    const off = 16 + (trainer ? 512 : 0);
    const prgLen = buf[4] * 16384,
      chrLen = buf[5] * 8192;
    lastRom = {
      prg: buf.slice(off, off + prgLen),
      chr: buf.slice(off + prgLen, off + prgLen + chrLen),
    };
  }

  document.getElementById('rom-input').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    saveSram();
    let buf;
    try {
      buf = new Uint8Array(await file.arrayBuffer());
    } catch (err) {
      console.error('[nes] rom read failed:', err);
      statusEl.textContent = t('readFail');
      return;
    }
    const ptr = api.romBuffer();
    if (buf.length > 4 * 1024 * 1024) {
      statusEl.textContent = t('tooBig');
      return;
    }
    Module.HEAPU8.set(buf, ptr);
    if (!api.loadRom(buf.length)) {
      let info = '';
      if (buf.length >= 16 && buf[0] === 0x4e && buf[1] === 0x45 && buf[2] === 0x53 && buf[3] === 0x1a) {
        const dirty = buf[12] || buf[13] || buf[14] || buf[15];
        const mapper = (buf[6] >> 4) | (dirty ? 0 : buf[7] & 0xf0);
        info = ` (mapper ${mapper}, PRG ${buf[4] * 16}KB, CHR ${buf[5] * 8}KB)`;
      } else {
        info = t('noHeader');
      }
      statusEl.textContent = t('unsupported') + info;
      return;
    }
    romKey = file.name + ':' + buf.length;
    keepRomCopies(buf);
    refreshExpansionUI();
    setCartSources(file.name, buf);
    updateRamLabels(file.name);
    loadSram();
    resumeAudio(); // don't await: resume() only settles after a user gesture
    statusEl.textContent = file.name;
    romLoaded = true;
    setPower(true); // 電源ON(パワーオンリセット込み)
  });

  // リセットはレベル信号: 押している間はリセット状態(停止)、離した瞬間に再起動
  let resetHeld = false;
  const btnReset = document.getElementById('btn-reset');
  function setResetHold(held) {
    if (held === resetHeld) return;
    resetHeld = held;
    btnReset.classList.toggle('held', held);
    // オフトリガーでリセットベクタから起動
    if (!held && running) {
      api.reset();
      mirrorResetNow();
    }
  }
  btnReset.addEventListener('pointerdown', (e) => {
    setResetHold(true);
    btnReset.setPointerCapture(e.pointerId);
  });
  btnReset.addEventListener('pointerup', () => setResetHold(false));
  btnReset.addEventListener('pointercancel', () => setResetHold(false));

  // ---- カセット入替ダイアログ: まるごと or PRG/CHR を別カセットから合体 ----
  const swapPanel = document.getElementById('swap-panel');
  const swapCurrent = document.getElementById('swap-current');
  let cartPrg = null; // {name, header(16B), data}
  let cartChr = null; // {name, data}

  function parseNes(buf) {
    if (buf.length < 16 || buf[0] !== 0x4e || buf[1] !== 0x45 || buf[2] !== 0x53 || buf[3] !== 0x1a) return null;
    const off = 16 + (buf[6] & 0x04 ? 512 : 0);
    const prgLen = buf[4] * 16384,
      chrLen = buf[5] * 8192;
    if (off + prgLen + chrLen > buf.length) return null;
    return {
      header: buf.slice(0, 16),
      prg: buf.slice(off, off + prgLen),
      chr: buf.slice(off + prgLen, off + prgLen + chrLen),
    };
  }
  function updateSwapInfo() {
    swapCurrent.textContent = `PRG: ${cartPrg ? cartPrg.name : '-'} / CHR: ${cartChr ? cartChr.name : '-'}`;
    const strip = (n) => n.replace(/\.nes$/i, '');
    document.getElementById('cart-label').textContent =
      cartPrg && cartChr && cartPrg.name !== cartChr.name
        ? strip(cartPrg.name) + ' + ' + strip(cartChr.name)
        : cartPrg
          ? strip(cartPrg.name)
          : 'CASSETTE';
  }
  function setCartSources(name, buf) {
    const p = parseNes(buf);
    if (!p) return;
    cartPrg = { name, header: p.header, data: p.prg };
    cartChr = { name, data: p.chr };
    updateSwapInfo();
  }
  // combined iNES image: mapper/mirroring follow the PRG cart's header
  function buildCombined() {
    const h = new Uint8Array(16);
    h.set(cartPrg.header);
    h[4] = cartPrg.data.length / 16384;
    h[5] = cartChr.data.length / 8192;
    h[6] &= ~0x04; // trainer stripped
    const img = new Uint8Array(16 + cartPrg.data.length + cartChr.data.length);
    img.set(h);
    img.set(cartPrg.data, 16);
    img.set(cartChr.data, 16 + cartPrg.data.length);
    return img;
  }
  // リセットは掛けない(電源入れっぱなし差し替え=バグ技用)
  function applySwap() {
    const img = buildCombined();
    if (img.length > 4 * 1024 * 1024) {
      statusEl.textContent = t('tooBig');
      return false;
    }
    Module.HEAPU8.set(img, api.romBuffer());
    if (!api.swapRom(img.length)) {
      statusEl.textContent = t('unsupportedSwap');
      return false;
    }
    romKey = `${cartPrg.name}+${cartChr.name}:${img.length}`;
    keepRomCopies(img);
    refreshExpansionUI();
    updateRamLabels(cartPrg.name);
    loadSram();
    romLoaded = true;
    updateSwapInfo();
    statusEl.textContent = t(powered ? 'swapDoneReset' : 'swapDonePower');
    return true;
  }
  async function readSwapFile(e) {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return null;
    let buf;
    try {
      buf = new Uint8Array(await file.arrayBuffer());
    } catch (_) {
      statusEl.textContent = t('readFail');
      return null;
    }
    const p = parseNes(buf);
    if (!p) {
      statusEl.textContent = t('unsupportedFmt');
      return null;
    }
    return { name: file.name, header: p.header, prg: p.prg, chr: p.chr };
  }
  document.getElementById('btn-swap').addEventListener('click', () => {
    updateSwapInfo();
    swapPanel.classList.add('show');
  });
  document.getElementById('swap-close').addEventListener('click', () => swapPanel.classList.remove('show'));
  document.getElementById('swap-input').addEventListener('change', async (e) => {
    const f = await readSwapFile(e);
    if (!f) return;
    saveSram(); // 旧カセットのSRAMを保存してから抜く
    cartPrg = { name: f.name, header: f.header, data: f.prg };
    cartChr = { name: f.name, data: f.chr };
    applySwap();
  });
  document.getElementById('swap-prg-input').addEventListener('change', async (e) => {
    const f = await readSwapFile(e);
    if (!f) return;
    saveSram();
    cartPrg = { name: f.name, header: f.header, data: f.prg };
    if (!cartChr) cartChr = { name: f.name, data: f.chr };
    applySwap();
  });
  document.getElementById('swap-chr-input').addEventListener('change', async (e) => {
    const f = await readSwapFile(e);
    if (!f) return;
    saveSram();
    cartChr = { name: f.name, data: f.chr };
    if (!cartPrg) cartPrg = { name: f.name, header: f.header, data: f.prg };
    applySwap();
  });

  // ---- load ROM from a URL (CORS permitting) ----

  // Read a response body while reporting how much has landed.
  //
  // Not res.arrayBuffer(): that resolves only once the whole ROM is down, which
  // on a slow link is exactly the stretch the user needs to see moving. A
  // cross-origin fetch often has no Content-Length to work from, so fall back to
  // showing raw KB rather than a percentage that would be a guess.
  async function readWithProgress(res) {
    const declared = parseInt(res.headers.get('Content-Length') || '', 10);
    const total = Number.isFinite(declared) && declared > 0 ? declared : 0;
    const reader = res.body.getReader();
    const parts = [];
    let received = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      parts.push(value);
      received += value.length;
      statusEl.textContent = total
        ? t('urlFetching') + ' ' + Math.round((received / total) * 100) + '%'
        : t('urlFetching') + ' ' + Math.round(received / 1024) + 'KB';
    }
    const buf = new Uint8Array(received);
    let at = 0;
    for (const part of parts) {
      buf.set(part, at);
      at += part.length;
    }
    return buf;
  }

  async function loadRomFromUrl(url) {
    // GitHub's file-view page is what people actually copy, and it neither
    // serves the bytes nor allows CORS — rewriting to raw.githubusercontent
    // beats surfacing that as an opaque fetch failure.
    url = url.replace(
      /^https:\/\/github\.com\/([^/]+)\/([^/]+)\/(?:blob|raw)\/(.+)$/,
      'https://raw.githubusercontent.com/$1/$2/$3',
    );
    statusEl.textContent = t('urlFetching');
    let buf;
    try {
      const res = await fetch(url);
      if (!res.ok) throw new Error(res.status);
      buf = await readWithProgress(res);
    } catch (_) {
      statusEl.textContent = t('urlFail');
      return;
    }
    const p = parseNes(buf);
    if (!p) {
      statusEl.textContent = t('unsupportedFmt');
      return;
    }
    const name = decodeURIComponent((url.split('/').pop() || 'rom.nes').split('?')[0]) || 'rom.nes';
    saveSram();
    cartPrg = { name, header: p.header, data: p.prg };
    cartChr = { name, data: p.chr };
    if (applySwap()) {
      // URL load boots like ROMを開く: power-cycle and run
      api.powerOn();
      updateRamLabels(name);
      setPower(true);
      resumeAudio();
      statusEl.textContent = name;
      swapPanel.classList.remove('show');
    }
  }
  const swapUrlInput = document.getElementById('swap-url');
  document.getElementById('swap-url-btn').addEventListener('click', () => {
    const u = swapUrlInput.value.trim();
    if (u) loadRomFromUrl(u);
  });
  swapUrlInput.addEventListener('keydown', (e) => {
    e.stopPropagation();
    if (e.key === 'Enter') document.getElementById('swap-url-btn').click();
  });
  swapUrlInput.addEventListener('keyup', (e) => e.stopPropagation());

  const muteBtn = document.getElementById('btn-mute');
  muteBtn.addEventListener('click', () => {
    muted = !muted;
    muteBtn.textContent = muted ? '🔇' : '🔊';
    applyMasterVolume();
  });

  // ---- settings dialog ----
  const settingsPanel = document.getElementById('settings-panel');
  const masterSlider = document.getElementById('master-vol');
  const masterVal = document.getElementById('master-val');
  function refreshMaster() {
    masterSlider.value = Math.round(masterVolume * 100);
    masterVal.textContent = Math.round(masterVolume * 100) + '%';
    applyMasterVolume();
  }
  masterSlider.addEventListener('input', () => {
    masterVolume = parseFloat(masterSlider.value) / 100;
    localStorage.setItem('masterVolume', masterVolume);
    refreshMaster();
  });
  masterSlider.addEventListener('dblclick', () => {
    masterVolume = 1;
    localStorage.setItem('masterVolume', masterVolume);
    refreshMaster();
  });
  masterSlider.addEventListener('keydown', (e) => e.stopPropagation());
  document.getElementById('btn-settings').addEventListener('click', () => {
    refreshMaster();
    settingsPanel.classList.toggle('show');
  });
  document.getElementById('settings-close').addEventListener('click', () => settingsPanel.classList.remove('show'));
  refreshMaster();

  // ------------------------------------------------------------------ dump check (XEVIOUS判定)
  // reference CRCs from a known-good Xevious (Japan) cartridge
  const XEV_REF = { name: 'XEVIOUS (J)', prgCrc: 0xeeb16683, prgKB: 32, chrCrc: 0x668b4ee6, chrKB: 8 };
  let lastRom = null; // {prg, chr} Uint8Array copies of the loaded ROM

  const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();
  function crc32(u8) {
    let c = 0xffffffff;
    for (let i = 0; i < u8.length; i++) c = CRC_TABLE[(c ^ u8[i]) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  }
  const hex8 = (v) => (v >>> 0).toString(16).toUpperCase().padStart(8, '0');

  // Detect a dead (stuck) address line: the dump then contains mirrored halves.
  function findStuckAddrLines(data, addrBits) {
    const stuck = [];
    for (let b = 0; b < addrBits; b++) {
      const m = 1 << b;
      let dup = true;
      for (let a = 0; a < data.length; a++)
        if (data[a] !== data[a ^ m]) {
          dup = false;
          break;
        }
      if (dup) stuck.push(b);
    }
    return stuck;
  }
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const progressBox = document.getElementById('check-progress');
  const progressLabel = document.getElementById('check-progress-label');
  const progressFill = document.getElementById('check-bar-fill');
  function setProgress(label, ratio) {
    progressLabel.textContent = `${label} ${Math.round(ratio * 100)}%`;
    progressFill.style.width = ratio * 100 + '%';
  }

  // CRC32 computed over ~durationMs of wall-clock time, with retro progress
  // display. Paced by elapsed time (not per-step sleeps) so background-tab
  // timer throttling doesn't stretch the total duration.
  async function crc32Slow(u8, label, durationMs) {
    const t0 = performance.now();
    let c = 0xffffffff;
    let processed = 0;
    while (processed < u8.length) {
      const ratio = Math.min(1, (performance.now() - t0) / durationMs);
      const target = Math.floor(u8.length * ratio);
      for (; processed < target; processed++) c = CRC_TABLE[(c ^ u8[processed]) & 0xff] ^ (c >>> 8);
      setProgress(label, processed / u8.length);
      if (processed >= u8.length) break;
      await sleep(40);
    }
    setProgress(label, 1);
    return (c ^ 0xffffffff) >>> 0;
  }

  async function findBusMiswireSlow(data, refCrc, addrBits, label) {
    const combos = [];
    for (let i = 0; i < addrBits; i++) for (let j = i + 1; j < addrBits; j++) combos.push({ kind: 'addr', i, j });
    for (let i = 0; i < 8; i++) for (let j = i + 1; j < 8; j++) combos.push({ kind: 'data', i, j });
    const buf = new Uint8Array(data.length);
    const DIAG_MS = 1500;
    const t0 = performance.now();
    let k = 0;
    while (k < combos.length) {
      const ratio = Math.min(1, (performance.now() - t0) / DIAG_MS);
      const target = Math.max(k + 1, Math.floor(combos.length * ratio));
      for (; k < target; k++) {
        const { kind, i, j } = combos[k];
        const mi = 1 << i,
          mj = 1 << j;
        if (kind === 'addr') {
          for (let a = 0; a < data.length; a++) {
            let b2 = a & ~(mi | mj);
            if (a & mi) b2 |= mj;
            if (a & mj) b2 |= mi;
            buf[b2] = data[a];
          }
        } else {
          for (let a = 0; a < data.length; a++) {
            const v = data[a];
            let w = v & ~(mi | mj);
            if (v & mi) w |= mj;
            if (v & mj) w |= mi;
            buf[a] = w;
          }
        }
        if (crc32(buf) === refCrc) return combos[k];
      }
      setProgress(label, k / combos.length);
      if (k < combos.length) await sleep(40);
    }
    return null;
  }

  async function checkRegionSlow(label, data, refCrc, refKB, addrBits, crcMs) {
    if (!data || data.length === 0) return t('xevNone', { label }) + '\n';
    if (data.length !== refKB * 1024) {
      return t('xevSizeNg', { label, size: data.length / 1024, ref: refKB }) + '\n';
    }
    const crc = await crc32Slow(data, t('xevCrcLabel', { label }), crcMs);
    if (crc === refCrc) return t('xevOk', { label, crc: hex8(crc) }) + '\n';
    let out = t('xevNg', { label, crc: hex8(crc), ref: hex8(refCrc) }) + '\n';
    const stuck = findStuckAddrLines(data, addrBits);
    if (stuck.length) {
      out += t('xevStuck', { lines: stuck.map((b) => 'A' + b).join(', ') }) + '\n';
    }
    const mis = await findBusMiswireSlow(data, refCrc, addrBits, t('xevBusLabel', { label }));
    if (mis) {
      const p = mis.kind === 'addr' ? 'A' : 'D';
      out += t(mis.kind === 'addr' ? 'xevSwapAddr' : 'xevSwapData', { a: p + mis.i, b: p + mis.j, label }) + '\n';
    } else if (!stuck.length) {
      out += t('xevUnknown') + '\n';
    }
    return out;
  }

  const checkPanel = document.getElementById('check-panel');
  const checkText = document.getElementById('check-text');
  let checking = false;
  document.getElementById('check-close').addEventListener('click', () => {
    if (!checking) checkPanel.classList.remove('show');
  });
  document.getElementById('btn-xev').addEventListener('click', async () => {
    if (checking) return;
    checkPanel.classList.add('show');
    if (!lastRom) {
      checkText.innerHTML = t('xevNoRom');
      return;
    }
    checking = true;
    checkText.innerHTML = t('xevTitle', { name: XEV_REF.name }) + '\n\n';
    progressBox.classList.add('show');
    setProgress(t('prep'), 0);
    checkText.innerHTML += await checkRegionSlow('PRGROM', lastRom.prg, XEV_REF.prgCrc, XEV_REF.prgKB, 15, 2000);
    checkText.innerHTML += await checkRegionSlow('CGROM ', lastRom.chr, XEV_REF.chrCrc, XEV_REF.chrKB, 13, 2000);
    checkText.innerHTML += '\n' + t('xevRef');
    progressBox.classList.remove('show');
    checking = false;
  });

  // ------------------------------------------------------------------ cartridge connector (60pin)
  const PIN_NAMES = [
    null,
    'GND',
    'CPU A11',
    'CPU A10',
    'CPU A9',
    'CPU A8',
    'CPU A7',
    'CPU A6',
    'CPU A5',
    'CPU A4',
    'CPU A3',
    'CPU A2',
    'CPU A1',
    'CPU A0',
    'CPU R/W',
    '/IRQ',
    'GND',
    'PPU /RD',
    'CIRAM A10',
    'PPU A6',
    'PPU A5',
    'PPU A4',
    'PPU A3',
    'PPU A2',
    'PPU A1',
    'PPU A0',
    'PPU D0',
    'PPU D1',
    'PPU D2',
    'PPU D3',
    '+5V',
    '+5V',
    'M2',
    'CPU A12',
    'CPU A13',
    'CPU A14',
    'CPU D7',
    'CPU D6',
    'CPU D5',
    'CPU D4',
    'CPU D3',
    'CPU D2',
    'CPU D1',
    'CPU D0',
    '/ROMSEL',
    'SOUND IN',
    'SOUND OUT',
    'PPU /WR',
    'CIRAM /CE',
    'PPU /A13',
    'PPU A7',
    'PPU A8',
    'PPU A9',
    'PPU A10',
    'PPU A11',
    'PPU A12',
    'PPU A13',
    'PPU D7',
    'PPU D6',
    'PPU D5',
    'PPU D4',
    '/NMI',
    'APU /IRQ',
    'MAPPER /IRQ',
  ];
  const busFront = document.getElementById('bus-front');
  const busBack = document.getElementById('bus-back');
  const pinEls = [null];
  const manualOff = new Set(); // pins the user broke by clicking
  let tilt = 0; // cartridge tilt in degrees (-6 .. +6)
  let clockHz = 1789773; // CPU clock in Hz (1 .. 1789773)
  const TILT_MAX = 6;

  // per-signal hover explanations (localized)
  function pinDesc(name) {
    if (name === 'GND') return t('pin_gnd');
    if (name === '+5V') return t('pin_5v');
    if (/^CPU A/.test(name)) return t('pin_cpuA', { n: name.slice(4) });
    if (/^CPU D/.test(name)) return t('pin_cpuD', { n: name.slice(4) });
    if (name === 'CPU R/W') return t('pin_rw');
    if (name === '/IRQ') return t('pin_irq');
    if (name === 'M2') return t('pin_m2');
    if (name === '/ROMSEL') return t('pin_romsel');
    if (name === 'SOUND IN') return t('pin_sndin');
    if (name === 'SOUND OUT') return t('pin_sndout');
    if (name === 'PPU /RD') return t('pin_ppurd');
    if (name === 'PPU /WR') return t('pin_ppuwr');
    if (name === 'CIRAM A10') return t('pin_ciramA10');
    if (name === 'CIRAM /CE') return t('pin_ciramCe');
    if (name === 'PPU /A13') return t('pin_ppuA13n');
    if (/^PPU A/.test(name)) return t('pin_ppuA', { n: name.slice(4) });
    if (/^PPU D/.test(name)) return t('pin_ppuD', { n: name.slice(4) });
    if (name === '/NMI') return t('pin_nmi');
    if (name === 'APU /IRQ') return t('pin_apuirq');
    if (name === 'MAPPER /IRQ') return t('pin_mapirq');
    return '';
  }

  const busBackFunc = document.getElementById('bus-back-func');
  const busFrontFunc = document.getElementById('bus-front-func');
  const funcEls = [null];
  function refreshPinTitles() {
    for (let pin = 1; pin <= 60; pin++) {
      const tip = `pin ${pin}: ${PIN_NAMES[pin]}\n${pinDesc(PIN_NAMES[pin])}`;
      if (pinEls[pin]) pinEls[pin].title = tip;
      if (funcEls[pin]) funcEls[pin].title = tip;
    }
  }
  for (let pin = 1; pin <= 60; pin++) {
    const name = PIN_NAMES[pin];
    const el = document.createElement('div');
    el.className = 'pin';
    el.dataset.pin = pin;
    el.innerHTML = `<b>${pin}</b>`;
    el.addEventListener('click', () => {
      if (manualOff.has(pin)) manualOff.delete(pin);
      else manualOff.add(pin);
      applyContacts();
      updateBusUI(true);
    });
    el.addEventListener('mouseenter', () => probeAttach(pin, el));
    el.addEventListener('mouseleave', () => probeDetach(pin));
    pinEls[pin] = el;
    (pin <= 30 ? busFront : busBack).appendChild(el);
    // function label above (back row) / below (front row)
    const fl = document.createElement('div');
    fl.className = 'func-label';
    fl.textContent = name;
    funcEls[pin] = fl;
    (pin <= 30 ? busFrontFunc : busBackFunc).appendChild(fl);
  }
  refreshPinTitles();
  document.getElementById('btn-bus').addEventListener('click', () => {
    document.body.classList.toggle('bus-on');
    updateBusUI(true);
  });

  // --- half-insertion model: tilting lifts one side of the edge connector ---
  // column 0..29 across the connector; both rows share the column
  const pinColumn = (pin) => (pin <= 30 ? pin - 1 : pin - 31);
  function contactQuality(col) {
    // 1 = solid, 0 = no contact
    if (tilt === 0) return 1;
    const x = (col / 29) * 2 - 1; // -1 (left) .. +1 (right)
    // clockwise (右回り, tilt>0) about the bottom-center pivot lifts the LEFT side
    const lift = (tilt / TILT_MAX) * -x;
    if (lift <= 0.15) return 1;
    if (lift >= 0.6) return 0;
    return 1 - (lift - 0.15) / 0.45;
  }
  // --- device mirror: forward the connector state to a CoreS3 over UDP ---
  //
  // The dice are rolled here in the browser and the result is mirrored, rather
  // than letting the device roll its own: that way the picture on the panel and
  // the picture in the page are showing the same cartridge, not two independent
  // simulations that happen to share a tilt angle.
  //
  // The browser cannot speak UDP, so this POSTs to the local server (see
  // tools/serve_web.py), which relays it as a type=1 packet.
  const MIRROR_MIN_INTERVAL_MS = 66;
  const deviceIp = (() => {
    // Query string wins over the stored value so a link can always retarget.
    const fromQuery = new URLSearchParams(location.search).get('device');
    if (fromQuery) {
      try {
        localStorage.setItem('nesDeviceIp', fromQuery);
      } catch (e) {
        /* private mode */
      }
      return fromQuery;
    }
    try {
      return localStorage.getItem('nesDeviceIp') || '';
    } catch (e) {
      return '';
    }
  })();

  let mirrorLastSent = 0;
  let mirrorPending = null; // newest mask held back by the throttle
  let mirrorTimer = 0;
  let mirrorWarned = false;

  function mirrorPost(mask) {
    mirrorLastSent = performance.now();
    fetch('/api/pins', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host: deviceIp, mask: mask.toString(16).padStart(16, '0') }),
    }).catch(() => {
      // One warning only: a disconnected device would otherwise flood the
      // console at frame rate while the cart is tilted.
      if (mirrorWarned) return;
      mirrorWarned = true;
      console.warn('pin mirror: cannot reach the relay at /api/pins — is `just serve` running?');
    });
  }

  // Throttled so a tilted cart re-rolling every frame cannot swamp the link,
  // but the *last* state always goes out — otherwise the device could be left
  // showing a stale mask after the user stops moving the slider.
  function mirrorPins(mask) {
    if (!deviceIp) return;
    const now = performance.now();
    const sinceLast = now - mirrorLastSent;
    if (sinceLast >= MIRROR_MIN_INTERVAL_MS) {
      mirrorPending = null;
      mirrorPost(mask);
      return;
    }
    mirrorPending = mask;
    if (mirrorTimer) return;
    mirrorTimer = setTimeout(() => {
      mirrorTimer = 0;
      if (mirrorPending === null) return;
      const pending = mirrorPending;
      mirrorPending = null;
      mirrorPost(pending);
    }, MIRROR_MIN_INTERVAL_MS - sinceLast);
  }

  // Mirror the master volume onto the device's speaker.
  //
  // 1.0 on the page maps to the firmware's SPEAKER_VOLUME (128), so "normal" is
  // the same loudness in both places; the slider's 0..1.5 range therefore spans
  // 0..192. Throttled like the pin mask — dragging the slider fires continuously
  // — but the final value is always delivered so the two cannot end up disagreeing.
  const DEVICE_VOLUME_BASE = 128;
  let volLastSent = 0;
  let volPending = null;
  let volTimer = 0;

  function volPost(level) {
    volLastSent = performance.now();
    fetch('/api/volume', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host: deviceIp, volume: level }),
    }).catch(() => {
      if (mirrorWarned) return;
      mirrorWarned = true;
      console.warn('pin mirror: cannot reach the relay at /api/volume — is `just serve` running?');
    });
  }

  function mirrorVolume(gain) {
    if (!deviceIp) return;
    const level = Math.max(0, Math.min(255, Math.round(DEVICE_VOLUME_BASE * gain)));
    const now = performance.now();
    const sinceLast = now - volLastSent;
    if (sinceLast >= MIRROR_MIN_INTERVAL_MS) {
      volPending = null;
      volPost(level);
      return;
    }
    volPending = level;
    if (volTimer) return;
    volTimer = setTimeout(() => {
      volTimer = 0;
      if (volPending === null) return;
      const pending = volPending;
      volPending = null;
      volPost(pending);
    }, MIRROR_MIN_INTERVAL_MS - sinceLast);
  }

  // Press RESET on the device.
  //
  // Only for resets the *user* performs (the RESET button, blowing on the cart,
  // reseating it). Internal resets — TAS start, ROM load — must not reach the
  // device, which is running its own built-in ROM and has no idea about them.
  //
  // This exists because pulling a CPU-bus pin can wedge the emulated program,
  // and restoring the contacts alone does not un-wedge it: the console has to
  // re-fetch the reset vector, same as pressing RESET on real hardware.
  function mirrorResetNow() {
    if (!deviceIp) return;
    fetch('/api/reset', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host: deviceIp }),
    }).catch(() => {
      if (mirrorWarned) return;
      mirrorWarned = true;
      console.warn('pin mirror: cannot reach the relay at /api/reset — is `just serve` running?');
    });
  }

  // Bypasses the throttle. For user actions that must land immediately.
  function mirrorPinsNow(mask) {
    if (!deviceIp) return;
    mirrorPending = null;
    if (mirrorTimer) {
      clearTimeout(mirrorTimer);
      mirrorTimer = 0;
    }
    mirrorPost(mask);
  }

  // --- send the current cartridge to the device over WiFi ---
  //
  // The device runs a ROM baked into its firmware, so this is the only way to
  // put a different game on it. The relay does the UDP work (see send_rom in
  // tools/serve_web.py) and answers once the device has accepted the image, so
  // the button can stay disabled for the whole transfer instead of guessing.
  //
  // Offered only when a device is configured — without one there is nowhere to
  // send to. Revealed here rather than where deviceIp is resolved so the whole
  // feature reads as one block.
  const DEVICE_ROM_MAX = 1024 * 1024; // matches ROM_MAX_SIZE on the device
  const swapDeviceRow = document.getElementById('swap-device-row');
  const swapDeviceBtn = document.getElementById('swap-device-btn');
  const swapDeviceNoReset = document.getElementById('swap-device-noreset');
  if (deviceIp) swapDeviceRow.hidden = false;

  // Device verdicts, as the relay maps them onto HTTP.
  const DEVICE_ROM_ERRORS = {
    409: 'deviceBusy',
    413: 'deviceTooBig',
    422: 'deviceUnsupported',
    504: 'deviceNoAnswer',
  };

  // Consume the relay's NDJSON stream, handing each complete line to `onLine`.
  //
  // Split here rather than accumulating the whole body: the point of asking for
  // progress is to show it while the transfer is still running.
  async function readNdjson(res, onLine) {
    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buf = '';
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += decoder.decode(value, { stream: true });
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (line) onLine(JSON.parse(line));
      }
    }
    const tail = buf.trim();
    if (tail) onLine(JSON.parse(tail));
  }

  async function sendRomToDevice() {
    const hasCart = cartPrg && cartChr;
    if (!hasCart) {
      statusEl.textContent = t('deviceNoCart');
      return;
    }
    const img = buildCombined();
    if (img.length > DEVICE_ROM_MAX) {
      statusEl.textContent = t('deviceTooBig');
      return;
    }

    const noReset = swapDeviceNoReset.checked;
    swapDeviceBtn.disabled = true;
    statusEl.textContent = t('deviceSending');
    try {
      const res = await fetch(
        '/api/rom?host=' + encodeURIComponent(deviceIp) + '&swap=' + (noReset ? 1 : 0) + '&progress=1',
        {
          method: 'POST',
          headers: { 'Content-Type': 'application/octet-stream' },
          body: img,
        },
      );
      // A rejection before the stream opens (bad host, oversized body) still
      // arrives as a plain status code, so keep the original mapping for it.
      if (!res.ok) {
        statusEl.textContent = t(DEVICE_ROM_ERRORS[res.status] || 'deviceFail');
        return;
      }
      let verdict = null;
      await readNdjson(res, (line) => {
        const isProgress = line.chunks > 0 && line.ok === undefined && !line.error;
        if (isProgress) {
          statusEl.textContent = t('deviceSending') + ' ' + Math.round((line.sent / line.chunks) * 100) + '%';
          return;
        }
        verdict = line;
      });
      if (verdict && verdict.ok) {
        statusEl.textContent = t(noReset ? 'deviceSentSwap' : 'deviceSent');
        return;
      }
      // No verdict means the stream died mid-transfer: the relay committed to
      // 200 in its headers, so there is no status code left to explain it.
      if (!verdict) {
        statusEl.textContent = t('deviceFail');
        return;
      }
      statusEl.textContent = t(DEVICE_ROM_ERRORS[verdict.http] || 'deviceFail');
    } catch (err) {
      // The relay itself is unreachable — a device that merely stayed silent
      // comes back as 504 above, not as a rejected fetch.
      console.warn('[nes] rom send failed:', err);
      statusEl.textContent = t('deviceFail');
    } finally {
      swapDeviceBtn.disabled = false;
    }
  }
  swapDeviceBtn.addEventListener('click', sendRomToDevice);

  // Everything the mirror needs is now initialised; volume changes may flow.
  // Push the current setting once so a device that booted at SPEAKER_VOLUME
  // picks up a slider the user had already moved in a previous session.
  mirrorReady = true;
  if (deviceIp) {
    console.info(`pin mirror: forwarding connector state to ${deviceIp}`);
    mirrorVolume(muted ? 0 : masterVolume);
  }

  // roll the dice for flaky pins — called every frame while tilted
  function applyContacts() {
    // Build the device mirror's mask from the same decisions the local core
    // gets, so the CoreS3 sees exactly the contacts shown in the UI — including
    // the per-frame re-roll of a flaky (partially seated) pin.
    let mask = 0n;
    for (let pin = 1; pin <= 60; pin++) {
      let on = 0;
      if (!manualOff.has(pin)) {
        const q = contactQuality(pinColumn(pin));
        on = q >= 1 || Math.random() < q ? 1 : 0;
      }
      api.setPin(pin, on);
      if (on) mask |= 1n << BigInt(pin - 1);
    }
    mirrorPins(mask);
  }

  let lastBusUi = 0;
  function updateBusUI(force) {
    const now = performance.now();
    if (!force && now - lastBusUi < 150) return;
    lastBusUi = now;
    if (!document.body.classList.contains('bus-on')) return;
    for (let pin = 1; pin <= 60; pin++) {
      const q = manualOff.has(pin) ? 0 : contactQuality(pinColumn(pin));
      const el = pinEls[pin];
      el.classList.toggle('off', q === 0);
      el.classList.toggle('unstable', q > 0 && q < 1);
    }
  }

  // ---- front expansion port (DA-15): the house-key rattle ----
  //
  // Pin map (NesDev "Expansion port"). Only the data lines are modelled; the
  // supply/strobe pins get a tooltip and nothing else, because shorting them on
  // real hardware does not produce a button press — it produces a repair bill.
  //   top row (odd):  1 GND, 3 /IRQ, 5 $4017 D3, 7 $4017 D1, 9 /OE, 11 OUT1,
  //                   13 $4016 D1, 15 +5V
  //   bottom (even):  2 SOUND, 4 $4017 D4, 6 $4017 D2, 8 $4017 D0, 10 OUT2,
  //                   12 OUT0, 14 /OE
  const EXP_PIN_NAMES = [
    null,
    'GND', 'SOUND', '/IRQ', '$4017 D4', '$4017 D3', '$4017 D2', '$4017 D1',
    '$4017 D0', '/OE (JP2)', 'OUT2', 'OUT1', 'OUT0', '$4016 D1', '/OE (JP1)', '+5V',
  ];
  // pin -> port bit. p1 = $4017 D0-D4, p0 = $4016 D1. Everything else: no effect.
  const EXP_P1_BIT = { 4: 4, 5: 3, 6: 2, 7: 1, 8: 0 };
  const EXP_IRQ_PIN = 3;
  const EXP_P0_PIN = 13;
  const EXP_GND_PIN = 1; // ground itself: touching it grounds the key
  const expFront = document.getElementById('exp-front'); // port face + drag surface
  const expBladeMark = document.getElementById('exp-blade-mark');
  const expRot = document.getElementById('exp-rot');
  const expAngleEl = document.getElementById('exp-angle');
  const expRattleBtn = document.getElementById('exp-rattle');
  const expStateEl = document.getElementById('exp-state');
  const expPinEls = [null];
  // Front-view geometry, in artwork pixels times EXP_ART_SCALE. The port art is
  // the 128x48 deformed asset (make_port_deformed_png.py), drawn for legibility
  // rather than photographic accuracy: 4x4 pin holes on a 12px pitch. The hole
  // centres below are quoted from that script and verified against the decoded
  // PNG (all 15 holes read 16/16 opening-coloured pixels).
  //
  // Each hole spans cx-2..cx+1, so its visual centre is half a pixel back from
  // the script's integer coordinate — hence the -0.5 on both axes. Without it
  // every highlight sits half a source pixel (2 screen px at 4x) low and right.
  const EXP_ART_SCALE = 4; // must match the CSS width/height on #exp-front-img
  const EXP_ROW_TOP_Y = 17.5 * EXP_ART_SCALE; // script y=18, less the half-pixel
  const EXP_ROW_BOT_Y = 29.5 * EXP_ART_SCALE; // script y=30, less the half-pixel
  const EXP_TOP_X0 = 21.5 * EXP_ART_SCALE; // pin 1 at script x=22
  const EXP_BOT_X0 = 27.5 * EXP_ART_SCALE; // pin 2 at script x=28
  // One pitch for both rows; the bottom row is offset half a pitch, as drawn.
  const EXP_PIN_STEP = 12 * EXP_ART_SCALE;
  const EXP_TOP_STEP = EXP_PIN_STEP; // 8 pins: script x 22 .. 106
  const EXP_BOT_STEP = EXP_PIN_STEP; // 7 pins: script x 28 .. 100
  function expPinPos(pin) {
    const isTopRow = pin % 2 === 1;
    if (isTopRow) return { x: EXP_TOP_X0 + ((pin - 1) / 2) * EXP_TOP_STEP, y: EXP_ROW_TOP_Y };
    return { x: EXP_BOT_X0 + (pin / 2 - 1) * EXP_BOT_STEP, y: EXP_ROW_BOT_Y };
  }
  function expPinDesc(pin) {
    if (pin === 1) return t('exp_pin_gnd');
    if (pin === 15) return t('exp_pin_5v');
    if (pin === 2) return t('exp_pin_sound');
    if (pin === EXP_IRQ_PIN) return t('exp_pin_irq');
    if (pin === EXP_P0_PIN) return t('exp_pin_p0d1');
    if (EXP_P1_BIT[pin] !== undefined) return t('exp_pin_p1', { n: EXP_P1_BIT[pin] });
    if (pin === 9 || pin === 14) return t('exp_pin_oe');
    return t('exp_pin_out');
  }
  function refreshExpPinTitles() {
    for (let pin = 1; pin <= 15; pin++) {
      const el = expPinEls[pin];
      if (el) el.title = `pin ${pin}: ${EXP_PIN_NAMES[pin]}\n${expPinDesc(pin)}`;
    }
  }
  for (let pin = 1; pin <= 15; pin++) {
    const el = document.createElement('div');
    el.className = 'dpin';
    el.dataset.dpin = pin;
    el.textContent = pin;
    const p = expPinPos(pin);
    el.style.left = p.x + 'px';
    el.style.top = p.y + 'px';
    expPinEls[pin] = el;
    expFront.appendChild(el);
  }
  refreshExpPinTitles();

  // --- key geometry: insertion point + roll angle -> the set of bridged pins ---
  //
  // Turning the key rolls it about the blade's long axis — the same axis you turn
  // a door key on. Seen from the front, that axis points straight at you, so the
  // blade's cross-section sweeps round like the hand of a clock: roll right and
  // the right-hand end of the blade dips onto the bottom row of pins while the
  // left-hand end lifts onto the top row.
  // --- the physical model, in millimetres, scaled by the artwork's pin pitch ---
  //
  // Sizes here are a functional requirement, not decoration: which pins a key
  // can bridge at once is decided entirely by how big the key is relative to
  // the connector, so getting the ratios wrong emulates a machine that does not
  // exist. Connector figures are from MIL-DTL-24308/1 and the Cinch/JAE/
  // Amphenol M24308 catalogues, 15 position, shell size 2. What mates on the
  // female side is not an opening but a protruding D-shaped tongue, outside
  // dims 24.59 x 7.82mm nominal (24.53-24.79 x 7.67-8.03 per MIL); the Famicom
  // recesses that tongue at the bottom of a plastic well, which is why on
  // screen it reads as a cavity. The key's travel is bounded by the tongue
  // face, so those are the dimensions used here.
  //
  // Pitch 2.77mm is drawn as 12px, so 1mm = 12/2.77 = 4.33px. (MIL true
  // position is 2.74mm; catalogues and JIS quote 2.76-2.77 — the 0.03mm/pitch
  // difference is far below one source pixel.) The rows sit ±1.42 either side
  // of centre, i.e. 2.84mm apart, which lands at 12.3px — the art's 12px row
  // gap, so the deformed asset is already to scale.
  const EXP_PX_PER_MM = EXP_PIN_STEP / 2.77;
  // What reaches the contacts is the key's TIP, not the full width of the
  // blade. A MIWA-pattern blade is about 9mm across at the bow end, but it is
  // ground to a guide taper, so the section that gets deep enough to touch the
  // pins measures roughly 5.5mm across. NOTE these key figures are estimates
  // from typical Japanese house keys: the Showa-era danchi key was MIWA's
  // disc-cylinder type (U9 only replaced it in 1991), and no published spec
  // for its blade was found — pinning these down needs a real key measured.
  // Why not the full 9mm: at 39px the contact body was half again as long as
  // anything that physically fits down the cavity, and it shorted pins the real
  // key could never reach at once.
  //
  // Thickness is the tip's, not the blade's. The blade body is the usual 2.3mm,
  // but a real key is bevelled at the tip, and it is that bevelled section —
  // about 1.5mm — which reaches the recessed contacts. Why not 2.3mm: drawn at
  // 10px it looked like a chisel rather than a key, and it claimed contact at a
  // depth the tapered part of the key never actually occupies.
  //
  // These two are the hit test AND the drawing: #exp-blade-mark::before is laid
  // out from them at load, so the rectangle you see is the one being
  // intersected. Why not tune them separately: a hit size picked to make the
  // covered set behave gave contacts that did not match the picture, which is
  // the one thing this panel exists to show.
  const EXP_BLADE_HALF = (5.5 / 2) * EXP_PX_PER_MM; // 5.5mm tip  -> ~24px
  const EXP_BLADE_HALF_H = (1.5 / 2) * EXP_PX_PER_MM; // 1.5mm bevel -> ~6.5px
  // Height of the tongue face the key works against: MIL dimension D,
  // 7.67-8.03, catalogue nominal 7.82. The tip has to stay inside it, which is
  // what bounds the turn. Why not the 8.36 used before: that is the PLUG
  // shell's inner window (25.30 x 8.40 nominal), not the female face.
  const EXP_OPENING_H = 7.82 * EXP_PX_PER_MM; // ~34px
  // What the tip has to touch is the metal, not the hole. A size-20 socket
  // contact receives a ⌀1.02mm pin (MIL-DTL-24308/1: "accommodates a .040
  // diameter pin"); the funnelled entry hole in the insulator face is wider
  // but unpublished. The artwork draws the entry ring 8px (1.85mm) across —
  // measured off the decoded PNG: pin 8's ring spans source x 60..67 — a
  // plausible funnel over the ⌀1.02 throat. The hit radius follows the
  // DRAWING at 4px: matching what is on screen is what keeps "lit" and
  // "touching" the same statement. (An earlier note here called the bore
  // ⌀1.68mm; that figure is the crimp barrel's, not the mating face's.)
  const EXP_HOLE_RADIUS = 4 * EXP_ART_SCALE;
  // The shell: the metal wall around the cavity, which on a real D-sub is
  // bonded to ground. Its inner edge is the D-shaped outline drawn in the
  // artwork, so rather than approximate it with a formula the profile is
  // quoted straight from the asset — half-width of the cavity about its centre
  // for each source row, measured off the decoded PNG. Indexed from
  // EXP_CAVITY_TOP; a row outside the table is outside the opening entirely.
  const EXP_CAVITY_TOP = 11;
  const EXP_CAVITY_CX = 64;
  const EXP_CAVITY_HALF_W = [
    33, 38, 42, 43, 44, 43, 43, 43, 43, 44, 45, 45, 45,
    45, 45, 44, 43, 43, 42, 42, 41, 40, 40, 39, 37, 34,
  ]; // rows 11..36

  // --- where the key may be put, all derived from the model above ---
  // Insertion point x, at the port centre — both rows are centred on 63.5.
  const EXP_SLOT_X0 = 63.5 * EXP_ART_SCALE;
  // Insertion x travel, clamped to the span of the contacts.
  const EXP_SLOT_X_MIN = 21.5 * EXP_ART_SCALE;
  const EXP_SLOT_X_MAX = 105.5 * EXP_ART_SCALE;
  // Insertion point y, default midway between the two rows. At true scale the
  // tip reaches both rows from here, so the neutral position shorts across
  // them — which is exactly the failure the trick exploits — and turning lifts
  // one end clear onto a single row.
  const EXP_SLOT_Y0 = 23.5 * EXP_ART_SCALE;
  // Insertion y travel: the tip's centre can go anywhere the tip still fits
  // inside the opening, i.e. the opening's half-height less the tip's own.
  // Derived rather than picked so it stays right if the tip dimensions change.
  // It reaches past both rows, so parking the key clear of every contact is
  // possible — touching nothing is a real position for a key to be in.
  const EXP_SLOT_Y_MIN = EXP_SLOT_Y0 - EXP_OPENING_H / 2 + EXP_BLADE_HALF_H;
  const EXP_SLOT_Y_MAX = EXP_SLOT_Y0 + EXP_OPENING_H / 2 - EXP_BLADE_HALF_H;
  let expKeyX = EXP_SLOT_X0; // insertion x along the row of contacts
  let expKeyY = EXP_SLOT_Y0; // insertion y across the two rows
  let expKeyAngle = 0; // degrees, + = turned clockwise
  let expInserted = true; // false = pulled clear of the port
  // Declared here rather than beside the rattle handlers because the grounding
  // test below reads it, and that runs from the very first setExpKey().
  let expRattling = false;
  // Lay the drawn bar out from the hit-test constants, so the rectangle on
  // screen is the rectangle being intersected.
  expBladeMark.style.setProperty('--blade-w', EXP_BLADE_HALF * 2 + 'px');
  expBladeMark.style.setProperty('--blade-h', EXP_BLADE_HALF_H * 2 + 'px');
  expBladeMark.style.setProperty('--blade-x', -EXP_BLADE_HALF + 'px');
  expBladeMark.style.setProperty('--blade-y', -EXP_BLADE_HALF_H + 'px');
  // Decoration that makes the tip read as a KEY rather than a bar lying on the
  // port: the bow seen end-on behind it, the warding grooves across its face,
  // and a crosshair marking the point you drag. All three are sized off the
  // same --blade-* custom properties, are pointer-events: none, and take no
  // part in expBridgedPins()/expShellContact() — the hit test is still the tip
  // rectangle alone, so the drawing cannot claim a contact the model does not
  // have. Added as children rather than pseudo-elements because ::before and
  // ::after on #exp-blade-mark are already the tip and the pivot, and because
  // children inherit the rotation transform for free.
  for (const id of ['exp-blade-bow', 'exp-blade-grooves', 'exp-blade-cross']) {
    const d = document.createElement('div');
    d.id = id;
    expBladeMark.appendChild(d);
  }
  // Distance from a point to the blade rectangle: rotate the point into the
  // bar's own frame, where the rectangle is axis-aligned, then take the usual
  // clamped box distance. Zero means the point is inside the bar.
  function expBladeDistance(px, py) {
    const rad = (expKeyAngle * Math.PI) / 180;
    const ux = Math.cos(rad); // unit vector along the blade cross-section
    const uy = Math.sin(rad);
    const dx = px - expKeyX;
    const dy = py - expKeyY;
    const along = dx * ux + dy * uy; // coordinate along the bar
    const across = -dx * uy + dy * ux; // coordinate across its thickness
    const outAlong = Math.max(0, Math.abs(along) - EXP_BLADE_HALF);
    const outAcross = Math.max(0, Math.abs(across) - EXP_BLADE_HALF_H);
    return Math.hypot(outAlong, outAcross);
  }
  function expBridgedPins() {
    if (!expInserted) return []; // out of the socket: nothing can be bridged
    const hit = [];
    for (let pin = 1; pin <= 15; pin++) {
      const p = expPinPos(pin);
      // rectangle-vs-circle: they meet when the box distance is within the radius
      const touching = expBladeDistance(p.x, p.y) <= EXP_HOLE_RADIUS;
      if (touching) hit.push(pin);
    }
    return hit;
  }
  // Half-width of the cavity at a given y, interpolated between the measured
  // rows. Outside the opening there is no cavity, so the width is zero.
  function expCavityHalfWidth(y) {
    const row = y / EXP_ART_SCALE - EXP_CAVITY_TOP;
    const outsideOpening = row < 0 || row > EXP_CAVITY_HALF_W.length - 1;
    if (outsideOpening) return 0;
    const i = Math.floor(row);
    const frac = row - i;
    const a = EXP_CAVITY_HALF_W[i];
    const b = EXP_CAVITY_HALF_W[Math.min(i + 1, EXP_CAVITY_HALF_W.length - 1)];
    return (a + (b - a) * frac) * EXP_ART_SCALE;
  }
  // Is the tip touching the shell — the grounded metal wall around the cavity?
  // The tip is a rotated rectangle, so its four corners are the points that
  // reach furthest out; a corner at or past the cavity edge means metal
  // contact. Sampling the corners rather than the whole outline is enough
  // because the wall is convex from the inside: no edge can cross it without a
  // corner crossing first.
  function expShellContact() {
    if (!expInserted) return false;
    const rad = (expKeyAngle * Math.PI) / 180;
    const ux = Math.cos(rad);
    const uy = Math.sin(rad);
    for (const sl of [-1, 1]) {
      for (const st of [-1, 1]) {
        const along = sl * EXP_BLADE_HALF;
        const across = st * EXP_BLADE_HALF_H;
        const cx = expKeyX + along * ux - across * uy;
        const cy = expKeyY + along * uy + across * ux;
        const reachesWall = Math.abs(cx - EXP_CAVITY_CX * EXP_ART_SCALE) >= expCavityHalfWidth(cy);
        if (reachesWall) return true;
      }
    }
    return false;
  }
  // A short needs a return path, not just a touched pin. The key is only a
  // short if it is also connected to ground, by either route:
  //   - the shell: the metal wall round the cavity is bonded to ground, so a
  //     key resting against the opening's edge is itself at ground. This is the
  //     main route, and it is why the real trick worked so readily — one signal
  //     pin is enough once the key is leaning on the shell.
  //   - pin 1, which is ground itself.
  // With neither, the key is a floating piece of metal touching a pulled-up
  // line: nothing happens, and reporting noise there would be a lie.
  //
  // Rattling is allowed to count as grounded on its own. Working the key back
  // and forth in the opening means it is knocking against the shell throughout,
  // even at instants when the drawn angle happens not to reach the wall; the
  // core's chattering supplies the intermittency, so gating it on the exact
  // frame's geometry would just drop contacts the real thing makes.
  function expKeyGrounded(pins) {
    if (!expInserted) return false;
    if (expRattling) return true;
    if (expShellContact()) return true;
    return pins.includes(EXP_GND_PIN);
  }
  function expApplyCover() {
    const pins = expBridgedPins();
    const grounded = expKeyGrounded(pins);
    let p0 = 0,
      p1 = 0,
      irq = false;
    for (const pin of pins) {
      if (pin === EXP_P0_PIN) p0 |= 1 << 1;
      const bit = EXP_P1_BIT[pin];
      if (bit !== undefined) p1 |= 1 << bit;
      if (pin === EXP_IRQ_PIN) irq = true;
    }
    // Ungrounded: the pins are touched but no current path exists.
    if (grounded) api.keyCover(p0, p1, irq ? 1 : 0);
    else api.keyCover(0, 0, 0);
    const covered = new Set(pins);
    for (let pin = 1; pin <= 15; pin++) {
      const el = expPinEls[pin];
      const touched = covered.has(pin);
      el.classList.toggle('covered', touched && grounded);
      // touching but with no return path: shown differently so the difference
      // between "the key is on it" and "this is actually shorted" is visible
      el.classList.toggle('floating', touched && !grounded);
    }
    document.body.classList.toggle('exp-grounded', grounded);
    // The geometry half of the instrument panel. Split from updateExpUI(),
    // which reports what the CORE is doing: this half is what the DRAWING is
    // doing, and it is only meaningful when the key actually moves.
    expMeterGeometry(pins, grounded);
  }
  // --- instrument panel ---
  //
  // Two halves, updated from two places: the geometry rows change only when the
  // key is moved (expApplyCover), while the port-bit rows are re-read from the
  // core every frame (updateExpUI). Why not refresh all of it every frame:
  // rebuilding the pin list allocates, and the geometry cannot change without
  // going through setExpKey() anyway.
  const expMeterEls = {
    key: document.getElementById('exp-m-key'),
    gnd: document.getElementById('exp-m-gnd'),
    pins: document.getElementById('exp-m-pins'),
    p1: document.getElementById('exp-m-p1'),
    p0: document.getElementById('exp-m-p0'),
    irq: document.getElementById('exp-m-irq'),
  };
  // $4017 D4..D0, drawn high bit first so the row reads like the register.
  const expBitCells = [];
  for (let i = 4; i >= 0; i--) {
    const b = document.createElement('b');
    b.textContent = '0';
    expBitCells[i] = b;
    expMeterEls.p1.appendChild(b);
  }
  function expSetMeter(el, text, cls) {
    el.textContent = text;
    el.classList.toggle('on', cls === 'on');
    el.classList.toggle('off', cls === 'off');
    el.classList.toggle('alarm', cls === 'alarm');
  }
  function expMeterGeometry(pins, grounded) {
    if (!expInserted) {
      expSetMeter(expMeterEls.key, t('expMeterOut'), 'alarm');
      expSetMeter(expMeterEls.gnd, '-', 'off');
      expSetMeter(expMeterEls.pins, '-', 'off');
      return;
    }
    expSetMeter(expMeterEls.key, expRattling ? t('expMeterRattling') : t('expMeterIn'), 'on');
    // Naming the route matters: "grounded" is the whole reason a touched pin is
    // or is not a short, and the two routes behave differently to the user.
    let how = t('expMeterFloat');
    let cls = 'off';
    if (grounded) {
      cls = 'on';
      if (expRattling) how = t('expMeterGndRattle');
      else if (expShellContact()) how = t('expMeterGndShell');
      else how = t('expMeterGndPin1');
    }
    expSetMeter(expMeterEls.gnd, how, cls);
    const list = pins.length ? pins.map((p) => `${p}:${EXP_PIN_NAMES[p]}`).join(', ') : t('expMeterNone');
    expSetMeter(expMeterEls.pins, list, pins.length ? '' : 'off');
  }
  // Re-render the geometry rows from the current state, for a language change.
  function refreshExpMeter() {
    if (!expInserted) {
      expMeterGeometry([], false);
      return;
    }
    const pins = expBridgedPins();
    expMeterGeometry(pins, expKeyGrounded(pins));
  }
  // The port-bit half: exactly what api.keyState() is reporting this frame, so
  // the panel shows the core's answer rather than the page's expectation of it.
  function expMeterPorts(st, active) {
    for (let i = 0; i <= 4; i++) {
      const set = active && (st & (1 << i)) !== 0;
      expBitCells[i].textContent = set ? '1' : '0';
      expBitCells[i].classList.toggle('set', set);
    }
    const p0 = active && (st & 0x100) !== 0;
    const irq = active && (st & 0x200) !== 0;
    expSetMeter(expMeterEls.p0, p0 ? t('expMeterShort') : '-', p0 ? 'alarm' : 'off');
    expSetMeter(expMeterEls.irq, irq ? t('expMeterShort') : '-', irq ? 'alarm' : 'off');
  }
  function expLayoutKey() {
    // An angle only means anything while the blade is inside the port.
    const angle = expInserted ? expKeyAngle : 0;
    // The blade's cross-section, turning like a clock hand about the insertion
    // point, which is also the bar's centre.
    expBladeMark.style.left = expKeyX + 'px';
    expBladeMark.style.top = expKeyY + 'px';
    const bladeTf = `rotate(${angle}deg)`;
    // the rattle keyframes compose on top of this base transform
    expBladeMark.style.setProperty('--blade-tf', bladeTf);
    expBladeMark.style.transform = bladeTf;
    expBladeMark.style.display = expInserted ? 'block' : 'none';
    document.body.classList.toggle('exp-pulled', !expInserted);
  }
  function setExpKey(x, angle, y) {
    expKeyX = Math.max(EXP_SLOT_X_MIN, Math.min(EXP_SLOT_X_MAX, x));
    const wantY = y === undefined ? expKeyY : y;
    expKeyY = Math.max(EXP_SLOT_Y_MIN, Math.min(EXP_SLOT_Y_MAX, wantY));
    expKeyAngle = Math.max(-30, Math.min(30, Math.round(angle * 10) / 10));
    expRot.value = expKeyAngle;
    expAngleEl.textContent = (expKeyAngle > 0 ? '+' : '') + expKeyAngle.toFixed(1) + '°';
    expLayoutKey();
    expApplyCover();
  }
  expRot.addEventListener('input', () => setExpKey(expKeyX, parseFloat(expRot.value)));
  // Drag anywhere on the port face to move the insertion point in both axes.
  // Grabbing away from the blade jumps it to the pointer, so a click picks a
  // spot directly instead of only nudging from wherever the blade already was.
  let expDragId = null;
  let expDragDx = 0;
  let expDragDy = 0;
  expFront.addEventListener('pointerdown', (e) => {
    if (!expInserted) return; // nothing to position while the key is out
    const box = expFront.getBoundingClientRect();
    const localX = e.clientX - box.left;
    const localY = e.clientY - box.top;
    const grabbedBlade =
      Math.abs(localX - expKeyX) <= EXP_BLADE_HALF &&
      Math.abs(localY - expKeyY) <= EXP_HOLE_RADIUS * 2;
    expDragId = e.pointerId;
    expDragDx = grabbedBlade ? localX - expKeyX : 0;
    expDragDy = grabbedBlade ? localY - expKeyY : 0;
    expFront.classList.add('dragging');
    expFront.setPointerCapture(e.pointerId);
    setExpKey(localX - expDragDx, expKeyAngle, localY - expDragDy);
    e.preventDefault();
  });
  expFront.addEventListener('pointermove', (e) => {
    if (expDragId !== e.pointerId) return;
    const box = expFront.getBoundingClientRect();
    const x = e.clientX - box.left - expDragDx;
    const y = e.clientY - box.top - expDragDy;
    setExpKey(x, expKeyAngle, y);
  });
  const expEndDrag = (e) => {
    if (expDragId !== e.pointerId) return;
    expDragId = null;
    expFront.classList.remove('dragging');
  };
  expFront.addEventListener('pointerup', expEndDrag);
  expFront.addEventListener('pointercancel', expEndDrag);

  const EXP_RATTLE_CYCLES = 1789773; // ~1 second at the rated clock
  function expStopRattleUI() {
    expRattling = false;
    expRattleBtn.disabled = !expInserted; // a pulled key has nothing to rattle
    expBladeMark.classList.remove('rattling');
    expLayoutKey(); // drop the keyframe transform back to the base one
    // rattling counts as grounded, so ending it can change the cover
    if (expInserted) expApplyCover();
  }
  function expStartRattle() {
    if (expRattling || !expInserted) return;
    // Set first: expApplyCover() treats a rattling key as grounded, so the
    // burst has to be flagged before the cover it runs with is computed.
    expRattling = true;
    expApplyCover();
    api.keyRattle(EXP_RATTLE_CYCLES);
    expRattleBtn.disabled = true;
    expBladeMark.classList.add('rattling');
  }
  expRattleBtn.addEventListener('click', expStartRattle);

  // Insert / pull is one toggle, matching the cartridge panel's single
  // "re-insert" button rather than adding a second mode control.
  const expPullBtn = document.getElementById('exp-pull');
  function refreshExpPullLabel() {
    expPullBtn.textContent = expInserted ? t('expPull') : t('expInsert');
  }
  function expSetInserted(inserted) {
    expInserted = inserted;
    if (!inserted) {
      // Pulling the key out ends the burst as far as the port is concerned: with
      // nothing bridging the contacts there is nothing left to chatter, so the
      // cover is cleared and the shake stops even if the core's counter still has
      // cycles on it (a paused CPU would otherwise never retire them).
      api.keyCover(0, 0, 0);
      expStopRattleUI();
      // 'floating' too: a pin the key was resting on without a return path is
      // still lit otherwise, so pulling the key left hollow rings on a port
      // with nothing in it.
      for (let pin = 1; pin <= 15; pin++) {
        expPinEls[pin].classList.remove('covered', 'shorted', 'floating');
      }
      expStateEl.textContent = '';
      expMeterGeometry([], false);
      expMeterPorts(0, false);
    }
    expRattleBtn.disabled = !inserted;
    refreshExpPullLabel();
    expLayoutKey();
    if (inserted) expApplyCover();
  }
  expPullBtn.addEventListener('click', () => expSetInserted(!expInserted));

  // Reflect what the core is actually shorting right now. Driven from the frame
  // loop rather than a timer so a slowed clock stretches the burst honestly, and
  // so the animation stops exactly when the core says the burst is over.
  function updateExpUI() {
    const panelHidden = !document.body.classList.contains('exp-on');
    if (panelHidden) return;
    const st = api.keyState();
    const active = (st & 0x8000) !== 0;
    if (expRattling && !active) expStopRattleUI();
    for (let pin = 1; pin <= 15; pin++) {
      const bit = EXP_P1_BIT[pin];
      let shorted = false;
      if (active && bit !== undefined) shorted = (st & (1 << bit)) !== 0;
      if (active && pin === EXP_P0_PIN) shorted = (st & 0x100) !== 0;
      if (active && pin === EXP_IRQ_PIN) shorted = (st & 0x200) !== 0;
      expPinEls[pin].classList.toggle('shorted', shorted);
    }
    expMeterPorts(st, active);
    if (!active) {
      expStateEl.textContent = '';
      return;
    }
    const p1 = st & 0x1f;
    const parts = [`$4017=${p1.toString(2).padStart(5, '0')}b`];
    if (st & 0x100) parts.push('$4016 D1');
    if (st & 0x200) parts.push('/IRQ');
    expStateEl.textContent = parts.join('  ');
  }
  document.getElementById('btn-exp').addEventListener('click', () => {
    document.body.classList.toggle('exp-on');
    updateExpUI();
  });
  setExpKey(expKeyX, 0);
  // Test hook: lets the headless screenshot/verification driver set the key
  // state directly and read back the geometry it is asserting against.
  window.__nes = window.__nes || {};
  window.__nes.exp = {
    scale: EXP_ART_SCALE,
    setKey: (x, angle, y) => setExpKey(x, angle, y),
    pinPos: (pin) => expPinPos(pin),
    state: () => ({ x: expKeyX, y: expKeyY, angle: expKeyAngle, inserted: expInserted }),
    bridged: () => expBridgedPins(),
    // Run one pass of the per-frame core readback on demand. The rAF loop only
    // runs with a ROM going, so a driver that wants to see the shorted state has
    // no other way to make the panel reflect a cover it just set.
    refresh: () => updateExpUI(),
    shellContact: () => expShellContact(),
    grounded: () => expKeyGrounded(expBridgedPins()),
    cavityHalfWidth: (y) => expCavityHalfWidth(y),
    // the rectangle the hit test uses, for cross-checking against the drawing
    blade: () => ({
      x: expKeyX,
      y: expKeyY,
      angle: expKeyAngle,
      halfLen: EXP_BLADE_HALF,
      halfThick: EXP_BLADE_HALF_H,
      holeRadius: EXP_HOLE_RADIUS,
    }),
  };

  // --- cartridge front view / rotation controls ---
  const cartBody = document.getElementById('cart-body');
  const cartAngle = document.getElementById('cart-angle');
  const cartSlider = document.getElementById('cart-tilt');
  function setTilt(t) {
    tilt = Math.max(-TILT_MAX, Math.min(TILT_MAX, Math.round(t * 10) / 10));
    cartBody.style.transform = `rotate(${tilt}deg)`;
    cartAngle.textContent = (tilt > 0 ? '+' : '') + tilt.toFixed(1) + '\u00b0';
    cartSlider.value = tilt;
    applyContacts();
    updateBusUI(true);
  }
  cartSlider.addEventListener('input', () => setTilt(parseFloat(cartSlider.value)));

  // ---- variable clock: 1 Hz .. 1.79 MHz (log slider + Hz input box) ----
  const NES_CLOCK = 1789773;
  const clockSlider = document.getElementById('clock-slider');
  const clockInput = document.getElementById('clock-input');
  const clockLabel = document.getElementById('clock-label');
  function fmtHz(hz) {
    if (hz >= 1e6) return (hz / 1e6).toFixed(2) + ' MHz';
    if (hz >= 1e3) return (hz / 1e3).toFixed(1) + ' kHz';
    return hz + ' Hz';
  }
  function setClock(hz) {
    clockHz = Math.max(1, Math.min(NES_CLOCK, Math.round(hz)));
    clockSlider.value = Math.log10(clockHz);
    clockInput.value = clockHz;
    clockLabel.textContent = 'CLOCK ' + fmtHz(clockHz);
    // APU resample ratio follows the clock: slower clock = lower pitch
    const rate = audioCtx ? audioCtx.sampleRate : 44100;
    api.init((rate * NES_CLOCK) / clockHz);
  }
  clockSlider.addEventListener('input', () => setClock(Math.pow(10, parseFloat(clockSlider.value))));
  clockSlider.addEventListener('dblclick', () => setClock(NES_CLOCK));
  clockInput.addEventListener('change', () => {
    const v = parseFloat(clockInput.value);
    if (!isNaN(v)) setClock(v);
    else clockInput.value = clockHz;
  });
  clockInput.addEventListener('keydown', (e) => e.stopPropagation());
  clockInput.addEventListener('keyup', (e) => e.stopPropagation());

  // drag the cartridge itself to rotate it (pivot = bottom center)
  const cartStage = document.getElementById('cart-stage');
  function pointerTiltAngle(e) {
    const r = cartStage.getBoundingClientRect();
    const px = r.left + r.width / 2;
    const py = r.bottom - 21; // cart-body bottom (0.75-scaled stage)
    return (Math.atan2(e.clientX - px, py - e.clientY) * 180) / Math.PI;
  }
  let dragBaseAngle = 0,
    dragBaseTilt = 0;
  cartBody.style.cursor = 'grab';
  cartBody.addEventListener('pointerdown', (e) => {
    dragBaseAngle = pointerTiltAngle(e);
    dragBaseTilt = tilt;
    cartBody.setPointerCapture(e.pointerId);
    cartBody.classList.add('dragging');
    e.preventDefault();
  });
  cartBody.addEventListener('pointermove', (e) => {
    if (!cartBody.classList.contains('dragging')) return;
    setTilt(dragBaseTilt + (pointerTiltAngle(e) - dragBaseAngle));
  });
  const endDrag = (e) => {
    if (!cartBody.classList.contains('dragging')) return;
    cartBody.classList.remove('dragging');
    try {
      cartBody.releasePointerCapture(e.pointerId);
    } catch (_) {}
  };
  cartBody.addEventListener('pointerup', endDrag);
  cartBody.addEventListener('pointercancel', endDrag);
  document.getElementById('cart-ccw').addEventListener('click', () => setTilt(tilt - 0.1));
  document.getElementById('cart-cw').addEventListener('click', () => setTilt(tilt + 0.1));
  // 息を吹く: ホコリが飛んで直ることもあれば、湿気で余計ダメになることも
  const blowSe = new Audio('foofoo.mp3?v=' + (window.NES_VER || '0'));
  document.getElementById('cart-blow').addEventListener('click', () => {
    blowSe.currentTime = 0;
    blowSe.play().catch(() => {});
    // 変化は基本PPU側(グラフィック系)に出る: 画面化けが定番症状
    const isPpuPin = (pin) => (pin >= 17 && pin <= 29) || (pin >= 47 && pin <= 60);
    for (let pin = 1; pin <= 60; pin++) {
      const breakChance = isPpuPin(pin) ? 0.1 : 0.005;
      if (manualOff.has(pin)) {
        if (Math.random() < 0.65) manualOff.delete(pin); // ゴミが飛んで復活
      } else {
        if (Math.random() < breakChance) manualOff.add(pin); // 湿気で接触不良に
      }
    }
    applyContacts();
    updateBusUI(true);
    api.reset(); // フーフーしたらリセットを押すのがお作法
    mirrorResetNow(); // applyContacts() above already mirrored the new pin state
    // 💨 演出
    const stage = document.getElementById('cart-stage');
    const puff = document.createElement('div');
    puff.className = 'blow-puff';
    puff.textContent = '💨';
    puff.style.left = 30 + Math.random() * 40 + '%';
    stage.appendChild(puff);
    setTimeout(() => puff.remove(), 1000);
    const label = document.getElementById('cart-label');
    label.classList.remove('shake');
    void label.offsetWidth; // restart animation
    label.classList.add('shake');
  });

  document.getElementById('cart-straight').addEventListener('click', () => {
    manualOff.clear();
    setTilt(0);
    api.resetPins();
    // Reseating must reach the device even if the throttle is mid-interval:
    // this is the one action the user expects to take effect instantly, and
    // leaving a fault latched on the panel would look like a hang.
    // Pins first, then reset — the same order as on a real console, and the
    // order the device must see them in: contacts restored before the reset
    // vector is fetched, or it would boot straight back into a broken bus.
    mirrorPinsNow((1n << 60n) - 1n);
    api.reset(); // 挿し直したらリセットボタンを押すのがお作法
    mirrorResetNow();
    updateBusUI(true);
  });

  // ------------------------------------------------------------------ TAS (FM2) playback
  let tasFrames = null;
  let tasIndex = 0;

  function parseFm2(text) {
    const frames = [];
    const map = [128, 64, 32, 16, 8, 4, 2, 1]; // R L D U T(Start) S(Select) B A
    for (const line of text.split(/\r?\n/)) {
      if (!line.startsWith('|')) continue;
      const parts = line.split('|');
      const cmd = parseInt(parts[1], 10) || 0;
      const p0 = parts[2] || '';
      let bits = 0;
      for (let i = 0; i < 8 && i < p0.length; i++) {
        const ch = p0[i];
        if (ch !== '.' && ch !== ' ') bits |= map[i];
      }
      frames.push({ cmd, bits });
    }
    return frames.length ? frames : null;
  }

  function tasStop(msg) {
    tasFrames = null;
    tasIndex = 0;
    document.getElementById('btn-tas').classList.remove('tas-on');
    if (msg) statusEl.textContent = msg;
  }

  function tasStart(frames) {
    // deterministic start: power cycle + FCEUX-style RAM pattern (00x4 FFx4)
    api.powerOn();
    const ramPtr = api.ram();
    for (let i = 0; i < 0x800; i++) Module.HEAPU8[ramPtr + i] = i & 4 ? 0xff : 0x00;
    api.reset(); // vectors fetched fresh after the pattern fill
    tasFrames = frames;
    tasIndex = 0;
    setPower(true);
    document.getElementById('btn-tas').classList.add('tas-on');
    statusEl.textContent = t('tasPlaying', { cur: 0, total: frames.length });
  }

  document.getElementById('btn-tas').addEventListener('click', () => {
    if (tasFrames) {
      tasStop(t('tasStopped'));
      return;
    }
    if (!romLoaded) {
      statusEl.textContent = t('needRom');
      return;
    }
    document.getElementById('tas-input').click();
  });
  document.getElementById('tas-input').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return;
    let text;
    try {
      text = await file.text();
    } catch (_) {
      statusEl.textContent = t('readFail');
      return;
    }
    const frames = parseFm2(text);
    if (!frames) {
      statusEl.textContent = t('tasBad');
      return;
    }
    tasStart(frames);
  });

  // ------------------------------------------------------------------ oscilloscope probe
  const probeScope = document.getElementById('probe-scope');
  const probeLabel = document.getElementById('probe-label');
  const probeCanvas = document.getElementById('probe-canvas');
  const probeCtx = probeCanvas.getContext('2d');
  let probeActive = 0;

  function probeAttach(pin, el) {
    probeActive = pin;
    api.setProbe(pin);
    probeLabel.textContent = (pin > 60 ? 'TP: ' : `pin ${pin}: `) + PIN_NAMES[pin];
    const r = el.getBoundingClientRect();
    const w = 272;
    probeScope.style.left = Math.max(4, Math.min(window.innerWidth - w - 4, r.left - w / 2)) + 'px';
    probeScope.style.top = document.getElementById('cartbus').getBoundingClientRect().bottom + 4 + 'px';
    probeScope.classList.add('show');
  }
  function probeDetach(pin) {
    if (probeActive !== pin) return;
    probeActive = 0;
    api.setProbe(0);
    probeScope.classList.remove('show');
  }
  document.querySelectorAll('.tp').forEach((el) => {
    const pin = +el.dataset.pin;
    el.title = `${PIN_NAMES[pin]}`;
    el.addEventListener('mouseenter', () => probeAttach(pin, el));
    el.addEventListener('mouseleave', () => probeDetach(pin));
  });
  const stripChart = new Uint8Array(256).fill(30);
  function drawProbe() {
    if (!probeActive) return;
    const ptr = api.probeBuffer();
    if (!ptr) return;
    const buf = Module.HEAPU8.subarray(ptr, ptr + 2048);
    const head = api.probePos();
    const W = probeCanvas.width,
      H = probeCanvas.height;
    // slow clock: the 2048-cycle window spans seconds — switch to a
    // wall-time strip chart sampled every animation frame instead
    const slowMode = clockHz < 60000;
    if (slowMode) {
      stripChart.copyWithin(0, 1);
      stripChart[255] = api.probeLevel();
    }
    probeCtx.fillStyle = '#0a0f05';
    probeCtx.fillRect(0, 0, W, H);
    // graticule
    probeCtx.strokeStyle = '#1c2a12';
    probeCtx.lineWidth = 1;
    probeCtx.beginPath();
    for (let gx = 0; gx <= W; gx += 32) {
      probeCtx.moveTo(gx + 0.5, 0);
      probeCtx.lineTo(gx + 0.5, H);
    }
    for (let gy = 0; gy <= H; gy += 20) {
      probeCtx.moveTo(0, gy + 0.5);
      probeCtx.lineTo(W, gy + 0.5);
    }
    probeCtx.stroke();
    probeCtx.strokeStyle = '#7CFC66';
    probeCtx.lineWidth = 0.8;
    probeCtx.beginPath();
    if (slowMode) {
      // real-time scrolling trace (right edge = now)
      for (let x = 0; x < W; x++) {
        const v = stripChart[((x * 256) / W) | 0];
        const y = H - 4 - (v / 255) * (H - 8);
        if (x === 0) probeCtx.moveTo(x, y);
        else probeCtx.lineTo(x, y);
      }
    } else {
      // simple rising-edge trigger for a stable trace
      const at = (i) => buf[(head + i) & 2047];
      let trig = 0;
      for (let i = 1; i < 1024; i++) {
        if (at(i - 1) < 128 && at(i) >= 128) {
          trig = i;
          break;
        }
      }
      const N = 1024;
      for (let x = 0; x < W; x++) {
        const i = trig + (((x * N) / W) | 0);
        const y = H - 4 - (at(i) / 255) * (H - 8);
        if (x === 0) probeCtx.moveTo(x, y);
        else probeCtx.lineTo(x, y);
      }
    }
    probeCtx.stroke();
  }

  // ------------------------------------------------------------------ gamepad
  // Standard mapping, Famicom-layout accurate: right button (1) = A, bottom (0) = B
  let gamepadConnected = false;
  window.addEventListener('gamepadconnected', (e) => {
    gamepadConnected = true;
    statusEl.textContent = '🎮 ' + e.gamepad.id.slice(0, 40);
  });
  window.addEventListener('gamepaddisconnected', () => {
    gamepadConnected = false;
  });

  function pollGamepad() {
    if (!gamepadConnected) return 0;
    let bits = 0;
    for (const gp of navigator.getGamepads()) {
      if (!gp || !gp.connected) continue;
      const b = gp.buttons;
      const pressed = (i) => b[i] && b[i].pressed;
      if (pressed(1) || pressed(3)) bits |= 1; // A (right / top)
      if (pressed(0) || pressed(2)) bits |= 2; // B (bottom / left)
      if (pressed(8)) bits |= 4; // Select
      if (pressed(9)) bits |= 8; // Start
      if (pressed(12)) bits |= 16; // Up
      if (pressed(13)) bits |= 32; // Down
      if (pressed(14)) bits |= 64; // Left
      if (pressed(15)) bits |= 128; // Right
      // left stick fallback
      if (gp.axes.length >= 2) {
        if (gp.axes[1] < -0.5) bits |= 16;
        if (gp.axes[1] > 0.5) bits |= 32;
        if (gp.axes[0] < -0.5) bits |= 64;
        if (gp.axes[0] > 0.5) bits |= 128;
      }
    }
    return bits;
  }

  // ------------------------------------------------------------------ debug panel
  const APU_REG_NAMES = [
    'SQ1_VOL',
    'SQ1_SWEEP',
    'SQ1_LO',
    'SQ1_HI',
    'SQ2_VOL',
    'SQ2_SWEEP',
    'SQ2_LO',
    'SQ2_HI',
    'TRI_LINEAR',
    '(unused)',
    'TRI_LO',
    'TRI_HI',
    'NOISE_VOL',
    '(unused)',
    'NOISE_LO',
    'NOISE_HI',
    'DMC_FREQ',
    'DMC_RAW',
    'DMC_START',
    'DMC_LEN',
    'OAMDMA',
    'SND_CHN',
    'JOY1',
    'JOY2/FRAME',
  ];
  const dbgApu = document.getElementById('dbg-apu');
  const dbgWram = document.getElementById('dbg-wram');
  // WRAM dump: one span per byte, double-click to edit in place
  const wramSpans = [];
  const wramHotAt = new Float64Array(0x800);
  const wramStreak = new Uint8Array(0x800);
  let wramPrimed = false;
  let editingSpan = null;
  for (let row = 0; row < 0x800; row += 16) {
    const line = document.createElement('div');
    const lab = document.createElement('span');
    lab.textContent = '$' + row.toString(16).toUpperCase().padStart(4, '0') + '  ';
    line.appendChild(lab);
    for (let i = 0; i < 16; i++) {
      const s = document.createElement('span');
      s.className = 'ram-b';
      s.dataset.addr = row + i;
      s.textContent = '00';
      line.appendChild(s);
      wramSpans.push(s);
    }
    dbgWram.appendChild(line);
  }
  // SMB loaded → show SMBDIS work-RAM names on hover
  function updateRamLabels(fileName) {
    const isSmb =
      /mario/i.test(fileName) ||
      (lastRom && lastRom.prg && lastRom.prg.length === 0x8000 && crc32(lastRom.prg) === 0x5cf548d3);
    const L = isSmb && window.SMB_RAM_LABELS ? window.SMB_RAM_LABELS : null;
    for (let a = 0; a < 0x800; a++) {
      wramSpans[a].title = '$' + a.toString(16).toUpperCase().padStart(4, '0');
    }
    if (!L) return;
    const addrs = Object.keys(L)
      .map(Number)
      .sort((x, y) => x - y);
    for (let i = 0; i < addrs.length; i++) {
      const start = addrs[i];
      const end = Math.min(i + 1 < addrs.length ? addrs[i + 1] : start + 16, 0x800);
      const [name, comment] = L[start];
      for (let a = start; a < end; a++) {
        const off = a - start;
        wramSpans[a].title =
          '$' +
          a.toString(16).toUpperCase().padStart(4, '0') +
          '  ' +
          name +
          (off ? '+' + off : '') +
          (comment ? '\n' + comment : '');
      }
    }
  }

  dbgWram.addEventListener('dblclick', (e) => {
    const span = e.target.closest('.ram-b');
    if (!span || editingSpan) return;
    // A snapshot is a read-only view: writing here would edit the local
    // emulator's RAM while the panel is showing the device's, which is worse
    // than not offering the edit at all.
    if (dbgSource !== localSource) return;
    e.preventDefault();
    editingSpan = span;
    const addr = +span.dataset.addr;
    const inp = document.createElement('input');
    inp.className = 'ram-edit';
    inp.maxLength = 2;
    inp.value = span.textContent;
    span.textContent = '';
    span.appendChild(inp);
    inp.focus();
    inp.select();
    const finish = (commit) => {
      if (editingSpan !== span) return;
      editingSpan = null;
      const v = parseInt(inp.value, 16);
      inp.remove();
      if (commit && !isNaN(v)) Module.HEAPU8[api.ram() + addr] = v & 0xff;
      span.textContent = hex2(Module.HEAPU8[api.ram() + addr]);
    };
    // keep hotkeys (R/D/F, pad keys) from firing while typing hex
    inp.addEventListener('keydown', (ev) => {
      ev.stopPropagation();
      if (ev.key === 'Enter') finish(true);
      else if (ev.key === 'Escape') finish(false);
    });
    inp.addEventListener('keyup', (ev) => ev.stopPropagation());
    inp.addEventListener('blur', () => finish(true));
  });
  const chrCanvas = document.getElementById('chr-canvas');
  const chrCtx = chrCanvas.getContext('2d');
  const chrImage = chrCtx.createImageData(128, 256);
  let chrPal = 0;
  function updateChrTitle() {
    document.getElementById('chr-title').textContent = t('chrTitle', {
      pal: t(chrPal < 4 ? 'bgPal' : 'spPal', { n: chrPal & 3 }),
    });
  }
  chrCanvas.addEventListener('click', () => {
    chrPal = (chrPal + 1) & 7;
    updateChrTitle();
    lastDebugUpdate = 0;
  });

  // waveform scopes
  const waveCanvases = [...document.querySelectorAll('canvas.wave')];
  const waveCtxs = waveCanvases.map((c) => c.getContext('2d'));
  // ---- per-channel mixer: mute / volume / pan ----
  const CHAN_NAMES = ['SQ1', 'SQ2', 'TRI', 'NOI', 'DMC', 'VP1', 'VP2', 'VSW'];
  const chanOn = [true, true, true, true, true, true, true, true];
  const chanVol = [1, 1, 1, 1, 1, 1, 1, 1];
  const chanPan = [0, 0, 0, 0, 0, 0, 0, 0];
  const waveLabels = [...document.querySelectorAll('#dbg-waves .wave-row span')].slice(0, 8);
  const mixLabels = [];

  function setChannelMute(ch, on) {
    chanOn[ch] = on;
    api.setChannel(ch, on ? 1 : 0);
    waveLabels[ch].classList.toggle('muted', !on);
    if (mixLabels[ch]) mixLabels[ch].classList.toggle('muted', !on);
  }
  waveLabels.forEach((span, ch) => {
    span.classList.add('chan-toggle');
    span.addEventListener('click', () => setChannelMute(ch, !chanOn[ch]));
  });

  const mixerBox = document.getElementById('dbg-mixer');
  CHAN_NAMES.forEach((name, ch) => {
    const row = document.createElement('div');
    row.className = 'mix-row' + (ch >= 5 ? ' exp-row' : '');
    const label = document.createElement('span');
    label.textContent = name;
    label.addEventListener('click', () => setChannelMute(ch, !chanOn[ch]));
    const vol = document.createElement('input');
    vol.type = 'range';
    vol.className = 'vol';
    vol.min = 0;
    vol.max = 2;
    vol.step = 0.05;
    vol.value = 1;
    const pan = document.createElement('input');
    pan.type = 'range';
    pan.className = 'pan';
    pan.min = -1;
    pan.max = 1;
    pan.step = 0.05;
    pan.value = 0;
    const val = document.createElement('span');
    val.className = 'val';
    const refresh = () => {
      const p = chanPan[ch];
      const side = Math.abs(p) < 0.03 ? 'C' : p < 0 ? 'L' : 'R';
      val.textContent = Math.round(chanVol[ch] * 100) + '% ' + side;
    };
    vol.addEventListener('input', () => {
      chanVol[ch] = parseFloat(vol.value);
      api.setChannelVolume(ch, chanVol[ch]);
      refresh();
    });
    pan.addEventListener('input', () => {
      chanPan[ch] = parseFloat(pan.value);
      api.setChannelPan(ch, chanPan[ch]);
      refresh();
    });
    vol.addEventListener('dblclick', () => {
      vol.value = 1;
      vol.dispatchEvent(new Event('input'));
    });
    pan.addEventListener('dblclick', () => {
      pan.value = 0;
      pan.dispatchEvent(new Event('input'));
    });
    refresh();
    row.append(label, vol, pan, val);
    mixerBox.appendChild(row);
    mixLabels[ch] = label;
    label.title = name;
    vol.title = 'volume (double-click = 100%)';
    pan.title = 'pan (double-click = center)';
  });
  function updateMuteTips() {
    document.querySelectorAll('#dbg-waves .wave-row span.chan-toggle').forEach((sp) => {
      sp.title = t('muteTip');
    });
    document.querySelectorAll('#dbg-mixer .vol').forEach((el) => {
      el.title = t('volTip');
    });
    document.querySelectorAll('#dbg-mixer .pan').forEach((el) => {
      el.title = t('panTip');
    });
  }
  // raw level range per channel: SQ1 SQ2 TRI NOI DMC / VRC6 pulse1 pulse2 sawtooth
  const WAVE_SCALE = [15, 15, 15, 15, 127, 15, 15, 31];
  const MIX_ROW = 8;
  let waveData = null; // captured per frame while debug is on

  function captureWave(count) {
    const chans = [];
    for (let i = 0; i < 8; i++) {
      const p = api.chanBuffer(i);
      chans.push(Module.HEAPU8.slice(p, p + count));
    }
    const mp = api.audioBuffer() >> 2;
    waveData = { count, chans, mix: Module.HEAPF32.slice(mp, mp + count) };
  }

  function drawWaves() {
    if (!waveData) return;
    const { count, chans, mix } = waveData;
    for (let ch = 0; ch <= MIX_ROW; ch++) {
      const ctx2 = waveCtxs[ch];
      const w = waveCanvases[ch].width,
        h = waveCanvases[ch].height;
      ctx2.fillStyle = '#000';
      ctx2.fillRect(0, 0, w, h);
      ctx2.strokeStyle = ch === MIX_ROW ? '#ffcf5a' : ch >= 5 ? '#6fd0dc' : '#6fdc6f';
      ctx2.lineWidth = 1;
      ctx2.beginPath();
      for (let x = 0; x < w; x++) {
        const i = Math.min(count - 1, ((x * count) / w) | 0);
        const v = ch < MIX_ROW ? chans[ch][i] / WAVE_SCALE[ch] : Math.min(1, mix[i] * 2);
        const y = h - 2 - v * (h - 4);
        if (x === 0) ctx2.moveTo(x, y);
        else ctx2.lineTo(x, y);
      }
      ctx2.stroke();
    }
  }
  let debugOn = false;
  let lastDebugUpdate = 0;
  const hex2 = (v) => v.toString(16).toUpperCase().padStart(2, '0');

  document.getElementById('btn-debug').addEventListener('click', () => {
    debugOn = !debugOn;
    document.body.classList.toggle('debug-on', debugOn);
  });

  // Source switcher, only offered when a device is configured — without one
  // there is nothing to switch to.
  const dbgSourceBox = document.getElementById('dbg-source');
  const dbgSrcNote = document.getElementById('dbg-src-note');
  // Revealed here rather than where deviceIp is resolved: that runs earlier in
  // the script, while this element's const is still in its temporal dead zone.
  if (deviceIp) dbgSourceBox.hidden = false;
  let dbgRemoteWarned = false;
  let dbgFetchInFlight = false;
  let dbgLastFetch = 0;

  function setDbgSource(which) {
    const remote = which === 'remote';
    dbgSource = remote ? remoteSource : localSource;
    // Panels a snapshot cannot feed are marked dead rather than left showing
    // stale local data that looks like it came from the device.
    document.body.classList.toggle('dbg-remote', remote);
    if (!remote) {
      remoteSnap = null;
      dbgSrcNote.textContent = '';
    }
  }

  function selectLocalRadio() {
    const r = dbgSourceBox.querySelector('input[value="local"]');
    if (r) r.checked = true;
    setDbgSource('local');
  }

  dbgSourceBox.addEventListener('change', (e) => {
    if (e.target.name !== 'dbgsrc') return;
    setDbgSource(e.target.value);
  });

  // Poll the device while the remote source is showing. 200ms matches the panel's
  // own refresh; a request in flight is never doubled up, because the device
  // answers on a frame boundary and a backlog would only add latency.
  function pollRemoteDebug(now) {
    if (dbgSource !== remoteSource || !debugOn) return;
    if (dbgFetchInFlight || now - dbgLastFetch < 200) return;
    dbgFetchInFlight = true;
    dbgLastFetch = now;
    fetch('/api/debug', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      // Ask for waves only while the scope is actually on screen: the flag is
      // what arms the device's per-sample capture, and it disarms itself once
      // the requests stop.
      body: JSON.stringify({ host: deviceIp, waves: debugOn }),
    })
      .then((r) => (r.ok ? r.arrayBuffer() : Promise.reject(new Error('HTTP ' + r.status))))
      .then((buf) => {
        const s = parseSnapshot(buf);
        if (s) {
          remoteSnap = s;
          if (s.waves) waveData = s.waves;
          dbgSrcNote.textContent = '';
        }
      })
      .catch((err) => {
        // Fall back to local rather than freeze on a stale snapshot: a panel that
        // silently stops updating is worse than one that says why.
        if (!dbgRemoteWarned) {
          dbgRemoteWarned = true;
          console.warn('debug monitor: cannot reach the device —', err.message);
        }
        dbgSrcNote.textContent = '× ' + err.message;
        selectLocalRadio();
      })
      .finally(() => {
        dbgFetchInFlight = false;
      });
  }

  // ---- debug data source: the local emulator, or a Stack-chan snapshot ----
  //
  // The DEBUG panel reads CPU registers, APU shadow registers, work RAM and
  // arbitrary bytes near PC. Routing all four through one object is what lets the
  // same rendering code show either machine; the remote implementation just
  // answers from the last snapshot instead of from WASM memory.
  //
  // Deliberately read-only and partial: a snapshot carries 48 bytes around PC,
  // not the whole address space, so peek() reports -1 outside that window and the
  // disassembler prints `??` rather than inventing bytes. Only the panels that a
  // snapshot can actually feed are offered remotely — waveforms, CHR and WRAM
  // editing stay local-only (see setDbgSource).
  const SNAP_SIZE = 12 + 0x18 + 2 + 48 + 0x800;
  const SNAP_APU = 12;
  const SNAP_PC = SNAP_APU + 0x18;
  const SNAP_CODE = SNAP_PC + 2;
  const SNAP_RAM = SNAP_CODE + 48;

  // Scope rows, appended only when the query asked for them: P1,P2,TRI,NOI,DMC,MIX.
  const SNAP_WAVE = SNAP_RAM + 0x800;
  const WAVE_W = 280;
  const WAVE_ROWS = 6;

  function parseSnapshot(buf) {
    if (!buf || buf.byteLength < SNAP_SIZE) return null;
    const b = new Uint8Array(buf);
    const s = {
      cpu: b.subarray(0, 12),
      apu: b.subarray(SNAP_APU, SNAP_APU + 0x18),
      pc: b[SNAP_PC] | (b[SNAP_PC + 1] << 8),
      code: b.subarray(SNAP_CODE, SNAP_CODE + 48),
      ram: b.subarray(SNAP_RAM, SNAP_RAM + 0x800),
      waves: null,
    };
    if (buf.byteLength >= SNAP_WAVE + WAVE_W * WAVE_ROWS) {
      // Repack into what drawWaves already consumes, so the scope needs no
      // separate remote rendering path. The device pre-decimated to the canvas
      // width using the same nearest-sample pick, so count === WAVE_W here and
      // drawWaves' own x->i mapping becomes the identity.
      const chans = [];
      for (let r = 0; r < 5; r++) {
        chans.push(b.subarray(SNAP_WAVE + r * WAVE_W, SNAP_WAVE + (r + 1) * WAVE_W));
      }
      // Expansion rows 5-7 have no device-side source; zero keeps them flat.
      for (let r = 5; r < 8; r++) chans.push(new Uint8Array(WAVE_W));
      // MIX arrived quantised through drawWaves' own min(1, mix*2) scaling, so
      // undo exactly that to hand back a float the same code can rescale.
      const mixRow = b.subarray(SNAP_WAVE + 5 * WAVE_W, SNAP_WAVE + 6 * WAVE_W);
      const mix = new Float32Array(WAVE_W);
      for (let i = 0; i < WAVE_W; i++) mix[i] = mixRow[i] / 255 / 2;
      s.waves = { count: WAVE_W, chans, mix };
    }
    return s;
  }
  window.__nes = window.__nes || {};
  window.__nes.parseSnapshot = parseSnapshot;

  const localSource = {
    cpuRegs: () => Module.HEAPU8.subarray(api.cpuRegs(), api.cpuRegs() + 12),
    apuRegs: () => Module.HEAPU8.subarray(api.apuRegs(), api.apuRegs() + 0x18),
    ram: () => Module.HEAPU8.subarray(api.ram(), api.ram() + 0x800),
    peek: (addr) => api.peek(addr & 0xffff),
  };

  let remoteSnap = null;
  const remoteSource = {
    cpuRegs: () => (remoteSnap ? remoteSnap.cpu : new Uint8Array(12)),
    apuRegs: () => (remoteSnap ? remoteSnap.apu : new Uint8Array(0x18)),
    ram: () => (remoteSnap ? remoteSnap.ram : new Uint8Array(0x800)),
    peek: (addr) => {
      if (!remoteSnap) return -1;
      const off = (addr - remoteSnap.pc) & 0xffff;
      // Work RAM is carried in full, so honour it wherever it is asked for.
      if ((addr & 0xffff) < 0x2000) return remoteSnap.ram[addr & 0x7ff];
      return off < remoteSnap.code.length ? remoteSnap.code[off] : -1;
    },
  };

  let dbgSource = localSource;

  // ---- 6502 disassembler (for the debug panel) ----
  const DIS_TAB = (() => {
    const tab = {};
    const src = `ADC:69 imm,65 zp,75 zpx,6D abs,7D abx,79 aby,61 izx,71 izy
AND:29 imm,25 zp,35 zpx,2D abs,3D abx,39 aby,21 izx,31 izy
ASL:0A acc,06 zp,16 zpx,0E abs,1E abx
BCC:90 rel;BCS:B0 rel;BEQ:F0 rel;BIT:24 zp,2C abs;BMI:30 rel;BNE:D0 rel;BPL:10 rel
BRK:00 imp;BVC:50 rel;BVS:70 rel;CLC:18 imp;CLD:D8 imp;CLI:58 imp;CLV:B8 imp
CMP:C9 imm,C5 zp,D5 zpx,CD abs,DD abx,D9 aby,C1 izx,D1 izy
CPX:E0 imm,E4 zp,EC abs;CPY:C0 imm,C4 zp,CC abs
DEC:C6 zp,D6 zpx,CE abs,DE abx;DEX:CA imp;DEY:88 imp
EOR:49 imm,45 zp,55 zpx,4D abs,5D abx,59 aby,41 izx,51 izy
INC:E6 zp,F6 zpx,EE abs,FE abx;INX:E8 imp;INY:C8 imp
JMP:4C abs,6C ind;JSR:20 abs
LDA:A9 imm,A5 zp,B5 zpx,AD abs,BD abx,B9 aby,A1 izx,B1 izy
LDX:A2 imm,A6 zp,B6 zpy,AE abs,BE aby
LDY:A0 imm,A4 zp,B4 zpx,AC abs,BC abx
LSR:4A acc,46 zp,56 zpx,4E abs,5E abx
NOP:EA imp
ORA:09 imm,05 zp,15 zpx,0D abs,1D abx,19 aby,01 izx,11 izy
PHA:48 imp;PHP:08 imp;PLA:68 imp;PLP:28 imp
ROL:2A acc,26 zp,36 zpx,2E abs,3E abx
ROR:6A acc,66 zp,76 zpx,6E abs,7E abx
RTI:40 imp;RTS:60 imp
SBC:E9 imm,E5 zp,F5 zpx,ED abs,FD abx,F9 aby,E1 izx,F1 izy,EB imm
SEC:38 imp;SED:F8 imp;SEI:78 imp
STA:85 zp,95 zpx,8D abs,9D abx,99 aby,81 izx,91 izy
STX:86 zp,96 zpy,8E abs;STY:84 zp,94 zpx,8C abs
TAX:AA imp;TAY:A8 imp;TSX:BA imp;TXA:8A imp;TXS:9A imp;TYA:98 imp
LAX:A7 zp,B7 zpy,AF abs,BF aby,A3 izx,B3 izy
SAX:87 zp,97 zpy,8F abs,83 izx
DCP:C7 zp,D7 zpx,CF abs,DF abx,DB aby,C3 izx,D3 izy
ISC:E7 zp,F7 zpx,EF abs,FF abx,FB aby,E3 izx,F3 izy
SLO:07 zp,17 zpx,0F abs,1F abx,1B aby,03 izx,13 izy
RLA:27 zp,37 zpx,2F abs,3F abx,3B aby,23 izx,33 izy
SRE:47 zp,57 zpx,4F abs,5F abx,5B aby,43 izx,53 izy
RRA:67 zp,77 zpx,6F abs,7F abx,7B aby,63 izx,73 izy
ANC:0B imm,2B imm;ALR:4B imm;ARR:6B imm;AXS:CB imm
NOP*:1A imp,3A imp,5A imp,7A imp,DA imp,FA imp,80 imm,82 imm,89 imm,C2 imm,E2 imm,04 zp,44 zp,64 zp,14 zpx,34 zpx,54 zpx,74 zpx,D4 zpx,F4 zpx,0C abs,1C abx,3C abx,5C abx,7C abx,DC abx,FC abx`;
    for (const group of src.split(/[\n;]/)) {
      const [name, list] = group.split(':');
      for (const ent of list.split(',')) {
        const [code, mode] = ent.trim().split(' ');
        tab[parseInt(code, 16)] = [name, mode];
      }
    }
    return tab;
  })();
  const DIS_LEN = {
    imp: 1,
    acc: 1,
    imm: 2,
    zp: 2,
    zpx: 2,
    zpy: 2,
    rel: 2,
    izx: 2,
    izy: 2,
    abs: 3,
    abx: 3,
    aby: 3,
    ind: 3,
  };
  const h4 = (v) => v.toString(16).toUpperCase().padStart(4, '0');

  function disasmLine(addr) {
    const op = dbgSource.peek(addr);
    // -1 = outside what the source can see (a remote snapshot only carries a
    // window around PC). Render the line as unknown instead of guessing.
    if (op < 0) return { len: 1, text: `${h4(addr)}  ??        ???` };
    const ent = DIS_TAB[op] || ['???', 'imp'];
    const [name, mode] = ent;
    const len = DIS_LEN[mode];
    const b1 = len > 1 ? dbgSource.peek(addr + 1) : 0;
    const b2 = len > 2 ? dbgSource.peek(addr + 2) : 0;
    if (b1 < 0 || b2 < 0) return { len, text: `${h4(addr)}  ??        ${name} ?` };
    const w = b1 | (b2 << 8);
    let operand = '';
    switch (mode) {
      case 'acc':
        operand = 'A';
        break;
      case 'imm':
        operand = '#$' + hex2(b1);
        break;
      case 'zp':
        operand = '$' + hex2(b1);
        break;
      case 'zpx':
        operand = '$' + hex2(b1) + ',X';
        break;
      case 'zpy':
        operand = '$' + hex2(b1) + ',Y';
        break;
      case 'abs':
        operand = '$' + h4(w);
        break;
      case 'abx':
        operand = '$' + h4(w) + ',X';
        break;
      case 'aby':
        operand = '$' + h4(w) + ',Y';
        break;
      case 'ind':
        operand = '($' + h4(w) + ')';
        break;
      case 'izx':
        operand = '($' + hex2(b1) + ',X)';
        break;
      case 'izy':
        operand = '($' + hex2(b1) + '),Y';
        break;
      case 'rel':
        operand = '$' + h4((addr + 2 + ((b1 << 24) >> 24)) & 0xffff);
        break;
    }
    const bytes = [op, b1, b2].slice(0, len).map(hex2).join(' ').padEnd(9);
    return { len, text: `${h4(addr)}  ${bytes} ${name} ${operand}`.trimEnd() };
  }

  function renderDisasm(pc) {
    let addr = pc;
    const lines = [];
    for (let i = 0; i < 12; i++) {
      const l = disasmLine(addr);
      lines.push(
        (i === 0 ? '<span class="cur">&gt;' : '\u00a0') + l.text.replace(/</g, '&lt;') + (i === 0 ? '</span>' : ''),
      );
      addr = (addr + l.len) & 0xffff;
    }
    document.getElementById('dbg-disasm').innerHTML = lines.join('\n');
  }

  function updateDebug(now) {
    if (!debugOn || now - lastDebugUpdate < 100) return;
    lastDebugUpdate = now;
    pollRemoteDebug(now);
    {
      const c = dbgSource.cpuRegs();
      const pc = c[0] | (c[1] << 8);
      const p = c[6];
      const flags = ['C', 'Z', 'I', 'D', 'B', '-', 'V', 'N']
        .map((f, i) => ((p >> i) & 1 ? f : f.toLowerCase()))
        .reverse()
        .join('');
      const frameN = c[8] | (c[9] << 8) | (c[10] << 16) | (c[11] << 24);
      document.getElementById('dbg-cpu').textContent =
        `PC=${pc.toString(16).toUpperCase().padStart(4, '0')}  A=${hex2(c[2])} X=${hex2(c[3])} Y=${hex2(c[4])}  SP=${hex2(c[5])}  P=${hex2(p)} [${flags}]  FRAME=${frameN}`;
      renderDisasm(pc);
    }
    const regs = dbgSource.apuRegs();
    let apuText = '';
    for (let i = 0; i < 0x18; i++) {
      apuText += '$' + (0x4000 + i).toString(16).toUpperCase() + '  ' + hex2(regs[i]) + '  ' + APU_REG_NAMES[i] + '\n';
    }
    dbgApu.textContent = apuText;

    const ram = dbgSource.ram();
    for (let a = 0; a < 0x800; a++) {
      const s = wramSpans[a];
      if (s === editingSpan) continue; // don't clobber the byte being edited
      const h = hex2(ram[a]);
      const changed = s.textContent !== h;
      if (changed) s.textContent = h;
      if (!wramPrimed) continue; // no classification on the initial fill
      // consecutive-change streak: constantly-changing bytes (timers, RNG)
      // are shown gray instead of glowing, so real events stand out
      let st = wramStreak[a];
      st = changed ? Math.min(st + 1, 20) : st > 0 ? st - 1 : 0;
      wramStreak[a] = st;
      if (st >= 8) {
        s.classList.add('busy');
        s.classList.remove('hot');
        wramHotAt[a] = 0;
      } else {
        s.classList.remove('busy');
        if (changed) {
          s.classList.add('hot');
          wramHotAt[a] = now;
        } else if (wramHotAt[a] && now - wramHotAt[a] > 700) {
          s.classList.remove('hot'); // let the CSS transition fade it out
          wramHotAt[a] = 0;
        }
      }
    }
    wramPrimed = true;

    // CHR needs the pattern tables, which a snapshot does not carry, so it keeps
    // showing the local emulator and is greyed out while remote is selected.
    if (dbgSource === localSource) {
      const chrPtr = api.renderChr(chrPal);
      if (chrPtr) {
        chrImage.data.set(Module.HEAPU8.subarray(chrPtr, chrPtr + 128 * 256 * 4));
        chrCtx.putImageData(chrImage, 0, 0);
      }
    }
    // The scope does have a remote source: waveData is refilled either by the
    // local capture or from the snapshot's decimated rows.
    drawWaves();
  }
  window.__nes.updateDebug = (now) => updateDebug(now);
  window.__nes.drawStatic = () => drawStatic();
  window.__nes.masterGainValue = () => (masterGain ? masterGain.gain.value : null);
  window.__nes.captureWave = (c) => captureWave(c);

  // ------------------------------------------------------------------ main loop
  const FRAME_MS = 1000 / 60.0988; // NTSC
  let lastTime = performance.now();
  let acc = 0;

  function tick(now) {
    requestAnimationFrame(tick);
    drawProbe(); // oscilloscope updates in real time, even when paused
    // Same deal: the burst only advances while the CPU runs, so the panel has to
    // keep polling even when it does not, or a rattle started while paused would
    // leave the button disabled and the key shaking forever.
    updateExpUI();
    if (!powered) {
      drawStatic();
      return;
    } // no signal
    if (!running || resetHeld) return; // reset held: CPU frozen in reset state

    acc += now - lastTime;
    lastTime = now;
    // at very low clocks we must accumulate enough time for at least 1 cycle
    const accCap = Math.max(150, (2200 * 1000) / clockHz);
    if (acc > accCap) acc = accCap;

    let ranFrame = false;
    if (tasFrames) {
      // TAS: strict frame stepping with the movie's inputs
      const effFrameMs = (FRAME_MS * NES_CLOCK) / clockHz;
      let burst = 0;
      while (tasFrames && acc >= effFrameMs && burst < 8) {
        burst++;
        acc -= effFrameMs;
        const f = tasFrames[tasIndex];
        if (f.cmd & 2) api.powerOn();
        else if (f.cmd & 1) api.reset();
        api.setButtons(0, f.bits);
        api.frame();
        window.__nes.frames++;
        ranFrame = true;
        tasIndex++;
        if (tasIndex % 30 === 0) statusEl.textContent = t('tasPlaying', { cur: tasIndex, total: tasFrames.length });
        if (tasIndex >= tasFrames.length) tasStop(t('tasDone'));
      }
      if (ranFrame) {
        const count = api.audioCount();
        if (count > 0) {
          if (pushSamples && !muted) {
            const ptr = api.audioBuffer() >> 2;
            pushSamples(Module.HEAPF32.slice(ptr, ptr + count));
          }
          if (debugOn) captureWave(count);
          api.audioClear();
        }
      }
    } else {
      const wantCycles = Math.min(((clockHz * acc) / 1000) | 0, 240000);
      if (wantCycles >= 1) {
        acc -= (wantCycles * 1000) / clockHz;
        if (tilt !== 0) applyContacts(); // flaky contacts re-roll each burst
        api.setButtons(0, buttons | pollGamepad());
        api.runCycles(wantCycles);
        window.__nes.frames++;
        ranFrame = true;

        // ship audio produced by this frame
        const count = api.audioCount();
        if (count > 0) {
          if (pushSamples && !muted) {
            const l = api.audioBuffer() >> 2,
              r = api.audioBufferR() >> 2;
            const inter = new Float32Array(count * 2);
            for (let i = 0; i < count; i++) {
              inter[i * 2] = Module.HEAPF32[l + i];
              inter[i * 2 + 1] = Module.HEAPF32[r + i];
            }
            pushSamples(inter);
          }
          if (debugOn) captureWave(count);
          api.audioClear();
        }

        // periodic SRAM save (~every 5s)
        if (++sramDirty >= 300) {
          sramDirty = 0;
          saveSram();
        }
      }
    }
    if (ranFrame) {
      const ptr = api.framebuffer();
      imageData.data.set(Module.HEAPU8.subarray(ptr, ptr + 256 * 240 * 4));
      ctx.putImageData(imageData, 0, 0);
      updateDebug(now);
      if (tilt !== 0) updateBusUI(false);
    }
  }
  // ---- URL query parameters ----
  // ?rom=<url> ?debug=1 ?pin=0 ?exp=1 ?clock=<Hz> ?tilt=<deg> ?break=25,29 ?mute=1 ?lang=en
  {
    const qs = new URLSearchParams(location.search);
    const langQ = qs.get('lang');
    if (langQ && (langQ === 'auto' || window.I18N[langQ])) {
      langPref = langQ;
      lang = langQ === 'auto' ? detectLang() : langQ;
    }
    if (qs.get('debug') === '1' && !document.body.classList.contains('debug-on')) {
      document.getElementById('btn-debug').click();
    }
    const pinQ = qs.get('pin') ?? qs.get('bus'); // 端子パネルの表示 (既定は1)
    if (pinQ === '0' && document.body.classList.contains('bus-on')) document.getElementById('btn-bus').click();
    if (pinQ === '1' && !document.body.classList.contains('bus-on')) document.getElementById('btn-bus').click();
    const expQ = qs.get('exp'); // 前面拡張端子パネルの表示 (既定は非表示)
    if (expQ === '0' && document.body.classList.contains('exp-on')) document.getElementById('btn-exp').click();
    if (expQ === '1' && !document.body.classList.contains('exp-on')) document.getElementById('btn-exp').click();
    const clk = parseFloat(qs.get('clock'));
    if (!isNaN(clk)) setClock(clk);
    const tiltQ = parseFloat(qs.get('tilt'));
    if (!isNaN(tiltQ)) setTilt(tiltQ);
    const brk = qs.get('break');
    if (brk) {
      for (const n of brk.split(',').map(Number)) {
        if (Number.isInteger(n) && n >= 1 && n <= 60) manualOff.add(n);
      }
      applyContacts();
      updateBusUI(true);
    }
    if (qs.get('mute') === '1' && !muted) document.getElementById('btn-mute').click();
    const volQ = parseFloat(qs.get('vol'));
    if (!isNaN(volQ)) {
      masterVolume = Math.max(0, Math.min(1.5, volQ / 100));
      refreshMaster();
    }
    const romQ = qs.get('rom');
    // ROM未指定時は既定のゲームを起動
    const DEFAULT_ROM_URL = 'https://raw.githubusercontent.com/GOROman/calude-famicom-game/main/game.nes';
    loadRomFromUrl(romQ || DEFAULT_ROM_URL);
  }
  applyLanguage();
  requestAnimationFrame((now) => {
    lastTime = now;
    tick(now);
  });
})();

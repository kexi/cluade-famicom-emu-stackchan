# claude-famicom-emu-stackchan

A Famicom (NES) emulator that runs in the browser **and on an M5Stack CoreS3 (Stack-chan)**. Fork of [GOROman/cluade-famicom-emu](https://github.com/GOROman/cluade-famicom-emu), which adds the embedded port and lets the browser's hardware playground — the breakable 60-pin cartridge connector, the tiltable half-inserted cart — drive the physical device over WiFi.

**▶ Play the original web version: https://goroman.github.io/cluade-famicom-emu/**

Open any .NES file (iNES format) with "Open ROM". Works on desktop and Android Chrome.

🌐 [日本語](README.ja.md) · [中文](README.zh.md)

## What this fork adds

### M5Stack CoreS3 / Stack-chan port (`m5stack/`)
- The same C++ core, compiled with `-DNES_EMBEDDED`: CPU-ahead batched execution with catch-up at register accesses, a per-scanline PPU renderer, and a 256-entry CPU dispatch table with the hottest opcodes in IRAM. **Bit-exact against the web core's dot-accurate renderer** (verified frame-by-frame on host), currently ~33 fps on the 240 MHz ESP32-S3.
- 256×240 RGB565 pushed to the LCD by DMA, fully overlapped with emulation; adaptive display divisor trades refresh rate for emulation speed.
- Audio through the built-in speaker with a ring buffer; playback rate follows the measured frame rate so it stays continuous instead of underrunning.
- Controller input over UDP — `just procon <device-ip>` streams a Nintendo Switch Pro Controller from your PC (`tools/procon_udp.py`).
- **The cartridge connector fault model runs on the device too.** All 60 pins can be broken at runtime; the healthy path costs a single branch, so full speed is kept until you start breaking pins.

### Browser → device mirroring
Serve the web UI locally and add `?device=<CoreS3 IP>` (the IP is shown on the device at boot):

```sh
just serve            # serves web/ and relays /api/* to the device as UDP
# then open http://localhost:8000/?device=192.168.x.x
```

Now the connector panel drives both emulators at once: tilt the cart and the Stack-chan glitches with the page, blow on it 💨, re-insert (which also presses RESET — reseating alone won't un-crash a wedged CPU, exactly like the real thing), and the master volume slider sets the device speaker. Protocol details (UDP types 0/1/2) are in [m5stack/README.md](m5stack/README.md).

### Reproducible toolchain (nix)
The whole toolchain — clang, Emscripten, PlatformIO, uv, just, lefthook, gitleaks — comes from a nix flake (Python scripts run through uv, which resolves the interpreter and dependencies from PEP 723 metadata at run time). With [nix](https://nixos.org) and [direnv](https://direnv.net) installed, `cd` into the repo and everything is provided; otherwise prefix commands with `nix develop --command`. Setup details (direnv/nix-direnv, the pre-commit hook, first-run steps) are in [CONTRIBUTING.md](CONTRIBUTING.md).

| Task | Command |
|------|---------|
| Build M5Stack firmware | `just build` |
| Build + flash | `just flash [port]` |
| Serial monitor | `just monitor` |
| Fetch default ROM | `just fetch-rom` |
| WiFi credentials template | `just secrets` (edit `m5stack/src/secrets.h`) |
| Pro Controller → UDP | `just procon <device-ip>` |
| Build web (WASM) | `just build-web` |
| Serve web + device relay | `just serve` |
| Core syntax check (both modes) | `just check` |

## Features (web version)

### Emulation core (C++ / WASM)
- **6502 CPU** — all official opcodes plus the common unofficial ones. Verified against nestest.nes: all 8991 steps match the reference log down to cycle counts.
- **PPU** — cycle-accurate scanline rendering, Loopy scrolling, sprite 0 hit, sprite overflow.
- **APU** — 2 pulse + triangle + noise + DPCM, played through an AudioWorklet (falls back to ScriptProcessor on non-HTTPS origins).
- **Mappers** — 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM, including oversize 64 KB CHR), 4 (MMC3 with scanline IRQ).
- Archaic iNES headers (the ones with `DiskDude!` garbage in the tail) are handled.
- Battery-backed SRAM is saved to localStorage automatically.

### Cartridge connector simulation ("PINS" mode, on by default)
- The **60-pin card edge** is drawn to the real pinout. Click a pin to break its contact.
- Breakage is modeled physically: missing address lines send the CPU to the wrong place, missing data lines return open-bus garbage, CIRAM faults wreck the nametables, a broken SOUND pin mutes the console (Famicom audio loops through the cartridge), and power is redundant across pins 1/16 (GND) and 30/31 (+5V).
- **Tilt the cartridge** with the slider (±6°, 0.1° steps) or by dragging it — the lifted side of the connector goes intermittent, then dead, exactly like a half-inserted cart.
- **Blow 💨** — clears a bad contact 65 % of the time, but each good PPU-side pin has a 10 % chance of going bad from the moisture. Comes with a sound effect and a reset, as tradition demands.
- **Re-insert** restores every pin, straightens the cart and resets.

### Hot cartridge swap (bug techniques)
The **RESET** button behaves like the real one — work RAM survives. **Swap Cart** loads a new ROM *without any reset at all*, so the classic swap-carts-with-the-power-on tricks work (Super Mario → Tennis → Super Mario for the minus/9-1 world). The dialog can also take the PRG-ROM and the CHR-ROM from **different cartridges** to build a franken-cart, or fetch a ROM straight **from a URL**.

### Debugging (DEBUG button / D key)
- **CHR (CGROM) viewer** — pattern tables colorized with the live PPU palette; click to cycle BG/sprite palettes. Connector faults show up here too.
- **Oscilloscope** — hover any connector pin (or the /NMI, APU /IRQ, MAPPER /IRQ test points) to attach a probe. The core samples that signal every CPU cycle; below 60 kHz the scope switches to a real-time strip chart.
- **CPU registers + disassembly** — PC/A/X/Y/SP/P with flags and a frame counter, over a live 12-instruction disassembly from PC.
- **APU** — six waveform scopes (SQ1/SQ2/TRI/NOI/DMC/MIX, click a label to mute that channel) and a register dump of $4000–$4017.
- **WRAM dump** — double-click a byte to edit it. Changed bytes glow; bytes that change constantly (timers, RNG) are grayed out. With Super Mario Bros. loaded, hovering a byte shows its variable name and comment from [SMBDIS.ASM](https://gist.github.com/1wErt3r/4048722).

### Variable clock
Slider and Hz input under the connector, spanning **1 Hz to the stock 1.789773 MHz** (logarithmic). The main loop paces by CPU cycles, so single-digit-Hz clocks really do step — you can watch the raster crawl across a frame. Audio pitch follows the clock.

### TAS playback
Load an FCEUX **.fm2** movie with the TAS button. Playback power-cycles with the FCEUX RAM pattern and then steps frame by frame with the movie's inputs; soft-reset and power commands in the movie are honored.

### Also
- USB gamepads (Gamepad API), a touch pad for Android, fullscreen (F).
- UI in English, Japanese and Chinese; follows the browser locale by default.
- **XEVIOUS dump check** — a hidden diagnostic that compares PRG/CHR CRC32s against a known-good dump and, when the CHR doesn't match, works out which address or data line the dumper had miswired.

## URL parameters

`http://localhost:8000/?rom=<URL>&debug=1&pin=0`

| Parameter | Effect |
|-----------|--------|
| `rom=<URL>` | Fetch and boot a .NES file from a URL (the host must allow CORS — GitHub raw does) |
| `device=<IP>` | Mirror connector faults, reset and volume to an M5Stack on the LAN (needs `just serve`) |
| `debug=1` | Start with the debug panel open |
| `pin=0` / `pin=1` | Hide / show the connector panel (shown by default) |
| `clock=<Hz>` | Clock frequency, 1–1789773 |
| `tilt=<deg>` | Cartridge tilt, ±6 |
| `break=25,29` | Start with these pins disconnected |
| `mute=1` | Start muted |
| `vol=<0-150>` | Master volume in percent |
| `lang=en/ja/zh/auto` | UI language |

## Controls

| NES | Keyboard | Touch | Gamepad |
|-----|----------|-------|---------|
| D-pad | Arrow keys | on-screen pad | D-pad / left stick |
| A | X | A | right face button |
| B | Z | B | bottom face button |
| Start | Enter | START | Start |
| Select | Shift | SELECT | Select |

Hotkeys: **F** fullscreen · **R** reset (held = held in reset) · **D** debug panel

## Build

Inside the nix dev shell (or with Emscripten installed):

```sh
just build-web   # → web/nes.js + web/nes.wasm, stamps a cache-busting version into index.html
just build       # → m5stack firmware (PlatformIO, ESP32-S3)
```

## Run locally

```sh
just serve       # http://localhost:8000/ — also relays /api/* to the device
```

- Desktop: http://localhost:8000/
- With a CoreS3 on the same network: http://localhost:8000/?device=<device IP>

## Deploy (GitHub Pages)

`web/` is published as the `gh-pages` branch.

```sh
just build-web
git add -A && git commit -m "..."
git push
git subtree push --prefix web origin gh-pages
```

## Layout

```
core/     C++ emulator core (shared by web and M5Stack)
  cpu.cpp        6502 (256-entry dispatch, cpu_ops.inc is the handler table)
  ppu.cpp        PPU (dot-accurate for web, batched per-scanline for NES_EMBEDDED)
  apu.cpp        APU + per-channel scope buffers
  cartridge.cpp  iNES loader + mappers 0-4
  nes.cpp        bus, 60-pin fault model, oscilloscope probe, WASM C API
m5stack/  CoreS3 frontend (PlatformIO) — display DMA, speaker, UDP input, pin mirror
tools/    procon_udp.py (Pro Controller → UDP), serve_web.py (web server + device relay)
web/      frontend (index.html / main.js / i18n.js / audio-worklet.js) + WASM output
flake.nix / justfile   reproducible toolchain and task runner
build.sh  Emscripten build + version stamping
```

## Tests

- **CPU**: a native harness runs nestest.nes against the reference log.
- **Mappers, headers, connector faults**: unit tests with synthetic ROMs (native build).
- **Embedded vs. reference**: the `NES_EMBEDDED` batched core is verified bit-exact against the dot-accurate core frame-by-frame on the host, with pins healthy and with (PPU-side) pins broken.

No ROMs are included in this repository — bring your own .NES files (`just fetch-rom` grabs the default game).

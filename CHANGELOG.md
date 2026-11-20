# Changelog

PalanuOS releases, newest first. Each section is written from the Conventional Commits since the last tag by the release PR
(`forge/tools/version.ts`, docs/RELEASE.md); edit it in the PR to curate it.

## 0.9.0 (2026-11-14)

The first release candidate for the hhv2 board (ESP32-S31, 320x240 screen, 39-key keyboard). It is a candidate, not 1.0: the
gates in docs/RELEASE.md that need a long run or a second board are still open.

### System

- Nema kernel: per-process memory quotas and sweeps, admission by free memory, safe cancellation, board descriptor and
  capabilities. Wi-Fi and Bluetooth run together (the radio manager starts each by measured cost).
- Service supervisor with restart policy and a safe mode after repeated boots that do not reach ready.
- OTA with a health check and rollback; a bad image is refused before it is switched to.
- microSD card on the screen's SPI bus, mounted at /sd; Settings > Storage shows used and total with Mount and Eject.
- Shell with history kept across restarts, written while the prompt is idle; SSH client (libssh) with key and password login.
- Public API (level 2) generated from api/palanu, for WebAssembly apps and the built-in ones; `.papp` v2 packages with an
  installer and trust store; apps can publish key legends (`pal_legend_show`).

### Screen and interface

- Pixel interface at a UI scale of 1x to 2.5x: home, lock, launcher (four skins), Control Center, notification
  center, app switcher, dialogs, legend bar, on-screen keyboard, Settings in 45 pages, Files, Logs, Terminal.
- Menus: every row is reachable (long information blocks are read with Up and Down), disabled rows show the cursor and say
  why, word wrap, long selected text scrolls. Property-tested on 300 random pages.
- Terminal for modern text UIs: box drawing and blocks drawn to the cell size, braille spinners, wide and zero-width characters,
  three font sizes (64, 53 and 40 columns), UTF-8 locale sent over SSH.

### Remote

- Palanu Link v1: UART, TCP, BLE and WebSocket transports; pairing with a 6-digit code and token auth; shell, screen mirror,
  keys, files and OTA channels.
- Web Forge: the whole remote in the browser with no server (USB by Web Serial, Bluetooth, Wi-Fi/LAN by WebSocket).
- `palanu` command line: pair, deploy --run, shell, screen, files, ota; app project templates.

### Quality

- 63 host tests, 125 golden screens in two themes, a build-time check of code rules, the internal RAM budget and
  every stack frame (`./pal check`, `./pal build`), a board smoke tour and soak (`./pal smoke`).

### Known limits

- Palanu Link does not encrypt the data after sign-in: use a trusted network (docs/PLP.md, "Threat model").
- One board is supported. No secure boot or flash encryption yet.
- One boot, in about sixty tried, ended in an interrupt watchdog reset nine milliseconds after Wi-Fi and Bluetooth were started
  in the same instant. It did not come back in the later boots. Radio bring-up now runs one radio at a time with a pause
  between them, as a precaution; the cause is in the vendor driver and is not proven fixed (docs/RELEASE.md).
- Release builds start with developer mode off: unsigned `.papp` packages are refused until it is turned on in
  Settings > Developer or a signing key is placed in /etc/trust. Built-in apps are not affected.

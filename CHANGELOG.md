# Changelog

PalanuOS releases, newest first. Each section is written from the Conventional Commits since the last tag by the release PR
(`forge/tools/version.ts`, docs/RELEASE.md); edit it in the PR to curate it.

## 0.10.0 (2026-11-20)

### Features

- **web**: one Web Forge page for a board and the simulator, in the pixel design
- **hid**: a USB transport seam for the keyboard
- **lora**: LoRa radio modules (ADR-0020)
- **music**: tunes on the buzzer
- **power**: light sleep in Saver while the screen is off, where the board can
- **display**: turn the screen upside down
- **ui**: the 12-hour clock
- **lua**: Lua 5.4 on the device (ADR-0019)
- **hid**: the device as a Bluetooth keyboard (ADR-0018)
- **ssh**: scp and sftp (ADR-0017)
- **store**: update PalanuOS from the catalogue, and sign the catalogue (ADR-0016)
- **apps**: offline manual, calculator, printf, SSH profiles
- app catalogue, install from the browser, browser file management (ADR-0015)
- **apps**: permissions at first use, events for apps, the log on flash (ADR-0014)
- **power**: power profiles that take effect
- **apps**: a full-screen text Editor; free internal RAM headroom
- **net**: TLS on sockets, HTTP client and app API, saved networks with manual IP, ping (ADR-0013)
- **apps**: a look per desktop for Files, Logs, text viewer and Terminal (ADR-0012)
- **athena**: Trackball theme with an app row, icon menu and right-edge Options
- **athena**: Softkey theme with a softkey bar, idle home and menu grid (ADR-0011)
- **athena**: home grid with clock and calendar widgets per theme; iOS-style grouped Settings
- **api**: form and canvas screens for apps, API level 6 (ADR-0010 P6)
- **desktop**: the target picks the desktop sessions it ships
- **settings**: one settings schema with owners; Athena themes (ADR-0010 P5)
- **desktop**: shell services drawn by each session (ADR-0010 P3)
- **display**: one RGB565 screen frame owned by the system; mirroring in full colour (ADR-0010 P4)
- **desktop**: whole desktop sessions own their screens, lock screen and keys (ADR-0010 P2)
- **console**: system console under the desktop sessions (ADR-0010 P1)
- **desktop**: toolkit menus bound to their view; Athena draws Settings as a full-colour grouped list
- **desktop**: desktop environments own the status bar, global keys and screen; Athena in full colour on the board
- **desktop**: desktop environment registry, desktop command, and the Athena desktop drawn with LVGL
- **web**: browser simulator in Web Forge with live hardware, Wi-Fi, bridge, hotkeys and key sequences
- **web**: browser simulator spike (Emscripten); share the portable libraries between host and web builds

### Fixes

- **net**: the socket pump no longer loses a wake-up
- **build**: pass GCC 15's snprintf truncation checks on Linux (CI)
- **link**: the device id no longer changes, so a browser signs in without pairing again
- **settings**: the Remote pages no longer lag; the Desktop page shows the new session's settings
- uname shows the real version, curl takes joined flags; record what was proven on hhv2
- **sh**: write the history after a long command instead of spinning
- **terminal**: say why the first shell did not start instead of a blank screen
- **athena**: reach every row of a long page to read
- **athena**: show a toggle's new value at once
- **athena**: show the perf overlay and count Athena's frames
- **sh**: run a whole and-or list in the background when it ends in &
- **forge**: keep the manifest defaults when a field is undefined
- **sdk**: restore the prebuilt net-client example package
- PIN and hidden-network entry, stale Athena rows, lock on session change, uishow under Athena
- **menu**: Up wraps to the last row, hidden rows no longer cut a page
- **term**: redraw the row the cursor left, so no block stays behind
- **terminal**: exit closes the tab, and the last one closes the app
- **ram**: keep five UI buffers in PSRAM so internal statics stay in budget

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

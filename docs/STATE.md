# Project state

Updated 13 November 2026.

## Component layout
The firmware is divided into layered components, and the layer boundaries are enforced by the build (T0.1). In ESP-IDF: `nema/` (the portable kernel; it carries the POSIX backends `net_bsd` and `posix_fs`, like the host libraries), `system/` (subsystems; `system/src` is private, the public API is in `system/include`), `apps/` (built-in commands and views; they only see the public nema and system headers) and `platform/esp-idf/palanu` (the aggregator that targets REQUIRE, containing the ESP-IDF port and the hhv2 board). The host uses the libraries `nema`, `palsys` and `palapps`. An include that violates the layers fails to compile in both builds; a temporary probe that tried to include the private `nema/src` and `system/src` headers from `apps/` failed on the host and on the board.

## Working (tested)
- **Nema** (kernel): classed and owned memory with an internal RAM reserve, task gate, one wait primitive, objects and handles, pipes, processes with signals and groups, two-sided TTY, registry, VFS with ramfs and `/bin`. 8 host tests, passing (also run with random scheduling jitter).
- **Subsystems:** 8-bit indexed canvas with dirty rects, Spleen fonts, themes with replaceable components (4 themes, dark mode, 2 sizes), xterm terminal emulator (17 tests), input with an Nm/Fn layer, display, desktop with views, wall clock, a system console under the desktop sessions (boot log with service status lines, quiet splash, PIN login, Ctrl+Alt+Esc; ADR-0010 P1), desktop sessions that own the status bar and global keys (`classic`, `athena` in full colour with LVGL; `desktop use ID`, `desktop stop`, docs/DESKTOPS.md; Athena runs on hhv2). Sessions are whole: each owns its screens, lock screen and keys, and leaving a screen lands on its home (ADR-0010 P2). The screen is one RGB565 frame owned by the system, which remote mirroring reads in full colour (P4). The rest is being rebuilt (P3, P5, P6).
- **Userland:** shell (line editor, history, Tab, quoting, variables, pipes, redirection, `&&` `||` `&`, glob, alias, scripts), 30 commands, launcher, tabbed Terminal, Settings, Monitor.
- **hhv2 board (proven on the chip):** boot, 80 MHz LCD with changed-area flush, TCA8418 keyboard, backlight, buzzer, RGB LED, shell on UART.
- **Host simulator:** `./pal sim` (SDL2 window) and a script mode with screenshots.

- **Persistent storage (proven on the chip):** `/home`, `/etc`, `/data` on LittleFS (`storage` partition, 8 MB); settings (theme, size, brightness, time zone) survive a reset.
- **Networking:** sockets as kernel objects, Wi-Fi station on the board (scan, connect, status; credentials in NVS), `nc`, `curl` (no TLS), SNTP.
- **SSH:** client on top of libssh 0.12.2 with Nema sockets; `known_hosts` in `/data/ssh`; public key (`-i`, default `/home/.ssh/id_ed25519`) and password; `ssh-keygen` (proven to create a key on the board). **Proven against a real OpenSSH on a computer** (public key, shell with PTY, size and resize, close; `forge/tests/ssh_e2e.ts`). This test found and fixed an error that kept the session from ever starting. **Not yet tested from the board over Wi-Fi** (needs Wi-Fi connected by the user).
- **Firmware update (proven on the chip):** `palanu ota`, A/B slots, bootloader rollback, self-confirmation 10 seconds after boot.
- **Battery:** ADC reading exists, but the S31 voltage scale is not calibrated (there is no calibration scheme in this IDF) and this board does not carry a battery, so the result is "not detected".
- **Palanu Link (proven on the chip and in the simulator):** specification `docs/PLP.md`, device side `system/src/link.c`, TypeScript client `forge/packages/link`, CLI `bun forge/tools/palanu.ts` (`info`, `ping`, `ls`, `get`, `put`, `screen`, `key`, `shell`). Over a 115200 baud UART cable: ping 10 ms, writing 20 KB takes 2.8 s; remote shell, screen mirror (a real board screenshot), key injection. 7 bun tests against the simulator (`bun test forge/packages/link`).

- **WebAssembly apps (proven on the chip):** WAMR fast interpreter, `.papp` container (CBOR manifest, SHA-256), the `palanu` v1 import module with permissions, installed apps run as Nema processes and can be piped, instruction-limit watchdog, installation through Palanu Link or `app install`. SDK in `sdk/`, `palanu build|pack|install`, guide `docs/PAPP.md`.
- **Browser simulator (verified in Chrome-class browser, not on the board):** the whole system runs in a Web Forge page (`./pal web`, `/sim/`): keyboard with the real matrix, hotkeys, key sequences, live LEDs/sensors/pins/I2C, editable Wi-Fi, TCP bridge (verified with `nc` against the local proxy), log panel, IndexedDB persistence. `docs/SIMULATOR.md`, ADR-0008.

## Not done / not verified
- The display on a physical screen and typing on the physical keyboard have not been seen directly; the screen mirror over Palanu Link shows the canvas contents correctly.
- SSH from the board over Wi-Fi to a real server (vim, htop), HTTPS/TLS.
- Graphical WebAssembly apps (graphics/view imports), `.papp` signing and compression, SD card.
- Browser simulator on a host without the isolation headers (`coi-sw.js`, for GitHub Pages): written, not run in a real browser yet.
- Browser simulator: no Bluetooth air interface, no inbound connections, no LAN discovery; the Palanu Link servers do not listen in the browser.
- BLE: ping was tested from an iPhone; shell, files, and screen over BLE and the Forge BLE client have not been tried.

## Phases
| Phase | State |
|---|---|
| F0 Measure first, set up code guardrails | done (T0.1 to T0.6): layered components, `./pal check` enforces the layers, `perf`/`mem`/`boot`, `./pal hwtest`, golden images, 11 host test suites. Baseline numbers in `docs/SIZE-BUDGETS.md` |
| F1 Nema and simulator | done |
| F2 Graphics and UI | partial: canvas, themes, basic components, launcher icons; not yet: dialogs, text input, toast, overlay layer, gallery and golden images |
| F3 Processes, TTY, shell, terminal | done |
| F4 Built-in apps, launcher | partial: grid launcher, Terminal, single-page Settings, Monitor, persistent storage; not yet: app switcher, service manager (`svc`), Files app, network/about Settings pages, `desktop.home`, boot targets and safe mode |
| F5 Networking and SSH | done and tested against OpenSSH on a computer; the test from the board over Wi-Fi is waiting on the user |
| F6 Palanu Link | done over UART and TCP; BLE tested (ping from an iPhone) |
| F7 Dynamic apps | done for terminal apps; graphical apps not yet |
| F8 BLE, power, release | OTA and rollback done; BLE tested with a ping from an iPhone; battery is read but not calibrated |

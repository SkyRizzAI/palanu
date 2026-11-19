# Project state

Updated 19 November 2026.

## Component layout
The firmware is divided into layered components, and the layer boundaries are enforced by the build (T0.1). In ESP-IDF: `nema/` (the portable kernel; it carries the POSIX backends `net_bsd` and `posix_fs`, like the host libraries), `system/` (subsystems; `system/src` is private, the public API is in `system/include`), `apps/` (built-in commands and views; they only see the public nema and system headers) and `platform/esp-idf/palanu` (the aggregator that targets REQUIRE, containing the ESP-IDF port and the hhv2 board). The host uses the libraries `nema`, `palsys` and `palapps`. An include that violates the layers fails to compile in both builds; a temporary probe that tried to include the private `nema/src` and `system/src` headers from `apps/` failed on the host and on the board.

## Working (tested)
- **Nema** (kernel): classed and owned memory with an internal RAM reserve, task gate, one wait primitive, objects and handles, pipes, processes with signals and groups, two-sided TTY, registry, VFS with ramfs and `/bin`. 8 host tests, passing (also run with random scheduling jitter).
- **Subsystems:** 8-bit indexed canvas with dirty rects, Spleen fonts, themes with replaceable components (4 themes, dark mode, 2 sizes), xterm terminal emulator (17 tests), input with an Nm/Fn layer, display, desktop with views, wall clock, a system console under the desktop sessions (boot log with service status lines, quiet splash, PIN login, Ctrl+Alt+Esc; ADR-0010 P1), desktop sessions that own the status bar and global keys (`classic`, `athena` in full colour with LVGL; `desktop use ID`, `desktop stop`, docs/DESKTOPS.md; Athena runs on hhv2). Sessions are whole: each owns its screens, lock screen and keys, and leaving a screen lands on its home (ADR-0010 P2). The screen is one RGB565 frame owned by the system, which remote mirroring reads in full colour (P4). Shell services (dialogs, pickers, text entry, busy, on-screen keyboard, toasts) are drawn by each session, or as text by the console (P3). Every settings key has one descriptor with its owner; one Settings app shows the system's keys under every session and each session's own only under it, with Settings > Desktop to switch (P5). Athena has four themes (Cupertino, Material, Softkey with a softkey bar, idle home and menu grid, and Trackball with an app row and an icon menu; ADR-0011), a home grid with clock and calendar widgets in a version per theme, and Settings drawn as iOS-style grouped cards under Cupertino. Apps have form and canvas screens at API level 6 (P6); Monitor is a form. Built-in apps have a look per desktop (ADR-0012): Files is a form, Logs and the text viewer are documents (`pal/doc.h`), the Terminal takes the session's colours.
- **Userland:** shell (line editor, history, Tab, quoting, variables, pipes, redirection, `&&` `||` `&`, glob, alias, scripts), about 50 built-in commands and apps, launchers, tabbed Terminal, Settings, Monitor, Files, Logs, a text Editor (`edit`, a gap buffer up to 256 KB, find, go to line), `man` (the offline manual), `calc`, `printf`, SSH profiles from `/home/.ssh/config`.
- **hhv2 board (proven on the chip):** boot, 80 MHz LCD with changed-area flush, TCA8418 keyboard, backlight, buzzer, RGB LED, shell on UART.
- **Host simulator:** `./pal sim` (SDL2 window) and a script mode with screenshots.

- **Persistent storage (proven on the chip):** `/home`, `/etc`, `/data` on LittleFS (`storage` partition, 8 MB); settings (theme, size, brightness, time zone) survive a reset.
- **Networking:** sockets as kernel objects, TLS on any socket (`nema_net_tls`: mbedtls on the board, OpenSSL in the native simulator), an HTTP/1.1 client (`pal/http.h`) under `curl` and the app API (`pal_tls_start`, `pal_http_fetch`, API level 7), Wi-Fi station on the board (scan, connect, up to 8 saved networks with Auto-Join and a manual address each; kept in NVS), `nc`, `ping`, `tcping`, SNTP.
- **SSH:** `scp` and `sftp` (ADR-0017: libssh's SFTP part built beside the component; proven against OpenSSH by
  `forge/tests/sftp_e2e.ts`, not yet from the board). Client on top of libssh 0.12.2 with Nema sockets; `known_hosts` in `/data/ssh`; public key (`-i`, default `/home/.ssh/id_ed25519`) and password; `ssh-keygen` (proven to create a key on the board). **Proven against a real OpenSSH on a computer** (public key, shell with PTY, size and resize, close; `forge/tests/ssh_e2e.ts`). This test found and fixed an error that kept the session from ever starting. **Not yet tested from the board over Wi-Fi** (needs Wi-Fi connected by the user).
- **Firmware update (proven on the chip):** `palanu ota`, A/B slots, bootloader rollback, self-confirmation 10 seconds after boot.
- **Battery:** ADC reading exists, but the S31 voltage scale is not calibrated (there is no calibration scheme in this IDF) and this board does not carry a battery, so the result is "not detected".
- **Palanu Link (proven on the chip and in the simulator):** specification `docs/PLP.md`, device side `system/src/link.c`, TypeScript client `forge/packages/link`, CLI `bun forge/tools/palanu.ts` (`info`, `ping`, `ls`, `get`, `put`, `screen`, `key`, `shell`). Over a 115200 baud UART cable: ping 10 ms, writing 20 KB takes 2.8 s; remote shell, screen mirror (a real board screenshot), key injection. 7 bun tests against the simulator (`bun test forge/packages/link`).

- **Apps at runtime (19 November 2026, ADR-0014):** sensitive permissions are asked at first use and kept per app; the event queue carries FOCUS, BLUR, EXPOSE, KEY, NOTIFY and SETTING; the kernel log is saved to `/data/log/system.log` by the `log-file` service.
- **WebAssembly apps (proven on the chip):** WAMR fast interpreter, `.papp` container (CBOR manifest, SHA-256), the `palanu` v1 import module with permissions, installed apps run as Nema processes and can be piped, instruction-limit watchdog, installation through Palanu Link or `app install`. SDK in `sdk/`, `palanu build|pack|install`, guide `docs/PAPP.md`.
- **Browser simulator (verified in Chrome-class browser, not on the board):** the whole system runs in a Web Forge page (`./pal web`, `/sim/`): keyboard with the real matrix, hotkeys, key sequences, live LEDs/sensors/pins/I2C, editable Wi-Fi, TCP bridge (verified with `nc` against the local proxy), log panel, IndexedDB persistence. `docs/SIMULATOR.md`, ADR-0008.

- **Catalogue and browser tools (ADR-0015):** the Store app and `store` (a list on any web server, SHA-256 checked); Web Forge
  installs PalanuOS on a board over Web Serial (`/flash/`) and manages files (new folder, rename, delete, edit, install).
- **The system from the catalogue (ADR-0016):** `update` and Settings > Developer > Updates download the board's firmware line
  straight into the other slot (size and SHA-256 checked while streaming); the list can be signed (`index.txt.sig`, Ed25519) and a
  firmware update needs a signed list or developer mode. Proven by `test_store_fw` and in the simulator (no slot there); not yet on
  hhv2. The release key is not made yet (the owner's step, `docs/RELEASE.md`).

## Not done / not verified
- **Keyboard (ADR-0018):** the device as a Bluetooth LE keyboard (HID over GATT, bonded in NVS); proven by `test_hid` and in the
  simulator, built for hhv2, not yet paired with a real host. USB HID is not done.
- **Lua (ADR-0019):** `lua` (scripts, a prompt, the `pal` module; `docs/LUA.md`), proven by `test_lua` and in the simulator, built
  for hhv2. The image is at 87.2% of the update slot (above the 85% warning).
- Not done from the 19 November 2026 analysis: USB HID keyboard mode, music, light sleep,
  display rotation, LoRa and radio modules.
- The display on a physical screen and typing on the physical keyboard have not been seen directly; the screen mirror over Palanu Link shows the canvas contents correctly.
- SSH from the board over Wi-Fi to a real server was shown once by the owner (echo, Ctrl-C and Ctrl-D were fixed afterwards and need to be tried again); HTTPS was proven in the native simulator against real servers, not yet from the board.
- `.papp` signing and compression.
- Browser simulator on a host without the isolation headers (`coi-sw.js`, for GitHub Pages): written, not run in a real browser yet.
- Browser simulator: no Bluetooth air interface, no inbound connections, no LAN discovery; the Palanu Link servers do not listen in the browser.
- BLE: ping was tested from an iPhone; shell, files, and screen over BLE and the Forge BLE client have not been tried.
- Stored only, nothing applies them yet: display rotation, `ui.clock24`. Power profiles apply a CPU frequency range (esp_pm), proven by tests and the board build, not yet measured on hhv2. RELEASE.md section 6 lists the confirmed gaps.

## Phases
| Phase | State |
|---|---|
| F0 Measure first, set up code guardrails | done: layered components, `./pal check` enforces the layers, `perf`/`mem`/`boot`, `./pal hwtest`, golden images (166 scenes), 69 host test suites. Baseline numbers in `docs/SIZE-BUDGETS.md` |
| F1 Nema and simulator | done |
| F2 Graphics and UI | done: canvas, themes with components, dialogs, text input, toasts, the overlay layer, the on-screen keyboard, gallery and golden images; desktop sessions classic and Athena (ADR-0010 to 0012) |
| F3 Processes, TTY, shell, terminal | done |
| F4 Built-in apps, launcher | done: launchers, app switcher, Terminal, Settings with its pages, Monitor, Files, Logs, the text viewer, the service manager (`svc`), boot targets and safe mode |
| F5 Networking and SSH | done and tested against OpenSSH on a computer; shown once from the board over Wi-Fi; TLS is not done |
| F6 Palanu Link | done over UART and TCP; BLE tested (ping from an iPhone) |
| F7 Dynamic apps | done for terminal apps and for form and canvas screens (API level 6) |
| F8 BLE, power, release | OTA and rollback done; BLE tested with a ping from an iPhone; battery is read but not calibrated; power profiles are stored only |

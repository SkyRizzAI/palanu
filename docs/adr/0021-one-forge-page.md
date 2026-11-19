# ADR-0021: One Web Forge page for a board and the simulator, joined by a virtual cable

Status: accepted (20 November 2026).

## Context
Web Forge had two pages: the remote (`/`, a board over USB, Bluetooth or Wi-Fi) and the simulator (`/sim/`, PalanuOS built to
WebAssembly). They looked different and did different things: the remote had Files, a shell and firmware update; the simulator
had only its screen, a log and its hardware panels, and no way into its files or a shell from the page. The owner's design
(November 2026) shows one tool: a device area with the screen and a pad, a console with Terminal and Logs, sections Display,
Files and System, a Store, and a footer with the link and the firmware, the same whether the device is a board or the simulator.

## Decision
- **One page, two sources.** The device is "none", the simulator or a remote board. Display, Files, System and Store are the
  same code for both, because both are reached through Palanu Link; the idle screen offers Start simulator and Connect remote.
- **The simulator gets a virtual cable.** `platform/web/web_main.c` exports `pal_web_link_write`, `pal_web_link_read` and
  `pal_web_link_reset`: two lock-free single-producer, single-consumer rings between the page and a `vlink` task that feeds a
  `pal_link` whose transport is the cable (`PAL_LINK_UART`). The page polls it every 8 ms and treats it as a Transport ("Virtual
  USB"). It is trusted like a USB cable: the page that runs the system is the system's owner, so pairing would add nothing.
- **The simulator's settings are its own section,** grouped like the device's Settings (Screen, Wi-Fi, Internet, Bluetooth, Power,
  Hardware, Input), shown while the simulator runs or nothing is connected, hidden for a board (they would not change it).
- **The Store reads a catalogue in the browser** (`docs/STORE.md` format) and installs over the link: download, check the SHA-256,
  write to `/tmp`, `INSTALL`. `./pal web` serves the repository's `store/` so it works without a web host.
- **The look is pixel art with the device's own letters.** `forge/web/pixfont.ts` turns Spleen 6x12 (`system/src/font_spleen_6x12.c`)
  into a TrueType font at build time (every lit pixel a square), inlined as "Spleen 6x12": no font download, offline, the same
  letters as the screen. Dark violet by default, the device's lavender paper as the light theme.
- **Every response of `./pal web` is cross-origin isolated** (COOP, COEP), since the simulator now lives on `/`; `/sim/` redirects.

## Consequences
- The page still opens straight from disk for a board (one file, no network); the simulator needs `./pal web` (isolation and the
  WebAssembly files next to the page), and the idle screen says so.
- A board and the simulator cannot be open at the same time in one tab; connecting one ends the other.
- The native simulator's clock now starts at the process start (as a board's starts at boot), which also fixed the uptime the
  browser simulator reported (CLOCK_MONOTONIC is the epoch under Emscripten).
- Proven in the browser against the WebAssembly system (shell, files, Store install, restart with the files kept) and the native
  simulator over WebSocket (screen, keys, files, disconnect); a board over USB or Bluetooth was not re-tested with the new page.

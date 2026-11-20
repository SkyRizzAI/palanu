# Web Forge

Forge in the browser (the page is titled Palanu Forge): one page for a PalanuOS device, a board or the simulator, with no server.
All logic (protocol, pairing, crypto, screen, terminal, files, the simulator itself) runs in the page. Code: `forge/web`
(TypeScript, bundled into a single HTML file). One page for both sources: ADR-0021.

## Running

```
./pal web                       # build the browser system and the page, serve at http://localhost:5174
bun forge/web/build.ts          # build only: forge/web/dist/index.html (a single file, no network)
```

`./pal web --no-sim` skips the WebAssembly build (Emscripten). The built file opens straight from disk in Chrome or Edge for a
board; the simulator in the same page needs the page served by `./pal web`. `serve.ts` serves the page with cross-origin
isolation (COOP and COEP, for the simulator's workers), the repository's catalogue at `/store/`, and a TCP proxy for the
simulator's bridge. `/sim/` (the old simulator page) leads to `/`. localhost counts as a secure context, which Web Serial and Web
Bluetooth require.

## On GitHub Pages

`.github/workflows/pages.yml` deploys the page on every push to `main` (ADR-0022), with the catalogue and the simulators: the
build of `main` (dev) and of the last ten releases, listed in `firmware.json`. The idle screen's **Sim firmware** and Simulator >
**Firmware** pick one; the choice is remembered, and the simulator's files stay across builds. Builds are unsigned for now. Pages
cannot send the isolation headers, so the first start registers `coi-sw.js` and reloads once. From an https page only USB and
Bluetooth reach a board (browsers block `ws://`). `bun forge/web/site.ts --out _site` assembles the same site locally.

## The page

- **Header**: the device chip (name, transport, a state dot; click it to connect or to see System), and Device, Store and Install.
- **Sections**: Display, Files, System and Simulator, with Restart and Shutdown (simulator) or Restart and Disconnect (board).
- **Display**: the screen with the board label and LEDs, a pad (Back = Esc, arrows, OK = Enter, SW = switch app, HM = home, KB =
  on-screen keyboard), Keys (the hhv2 key matrix, simulator only) and Shot (a PNG). Beside it the console: **Terminal** (a shell
  over the link, xterm.js) and **Logs** (the simulator's log or a board's console lines; filter, problems only, wrap, pause, save).
  Idle, the screen offers **Start simulator** and **Connect remote**.
- **Files**: browse, download, upload, new folder, rename, delete, edit a text file in place, install a `.papp`.
- **System**: the device's info and ping; for a board, pairing (Forget) and firmware update; for the simulator, frames, uptime
  and Erase simulator data.
- **Simulator**: the simulator's settings, grouped like the device's Settings, and Firmware, the build to start (`docs/SIMULATOR.md`). Hidden while a board is connected.
- **Store**: a catalogue (`index.txt`, `docs/STORE.md`) read by the browser; Install downloads the package, checks its SHA-256,
  writes it to `/tmp` and installs it over the link, on a board or the simulator. The address is remembered; `./pal web` offers its
  own `/store/` first.
- **Footer**: state, the link's transport (`PLP: USB`, `WI-FI-LAN`, `VIRTUAL-USB`), source, firmware version, resolution, session
  time, and the Dark/Light switch (remembered; the system's choice until clicked).
- At phone width the frame fills the screen and the sections become a bottom bar (Display, Files, System, Simulator, Store).
- The look: hard 2 px lines and shadows, labels in "Spleen 6x12", the device's Spleen 6x12 font turned into a TrueType font by
  `forge/web/pixfont.ts` at build time and inlined (no font download).

## Transports

| Option | Browser API | Device | Browser |
| --- | --- | --- | --- |
| USB | Web Serial | the board's USB UART (console and PLP share the cable); trusted cable, no pairing | Chrome, Edge (computer) |
| Bluetooth | Web Bluetooth | the Palanu Link GATT service (`docs/PLP.md`) | Chrome, Edge (computer, Android) |
| Wi-Fi / LAN | WebSocket | port 7001 (`system/src/link_ws.c`) | all modern browsers |
| Virtual USB | none: memory rings | the simulator in this page (`pal_web_link_*`, ADR-0021); trusted like the cable | where the simulator runs |

Wi-Fi and Bluetooth are untrusted until AUTH. Pair once: open the pairing window on the device (Settings > Remote >
Pair, or `link pair` in its terminal), type the 6-digit code in Web Forge. The token is stored only in that browser's `localStorage`
(key `palanu.webforge.links`, by the device's id), so the next connection signs in without a code. The pairing is named after the
browser ("Chrome on macOS"), which is how the device lists it. "Forget this pairing" deletes it in the browser and, while connected,
on the device too (AUTH_FORGET); on the device, Settings > Remote > Authorized Devices shows each pairing with when it was last
used, and Enter revokes it (or `link revoke ID`).

Crypto: X25519 and HMAC-SHA256 from WebCrypto (Chrome/Edge 133, Safari 17, Firefox 130), ChaCha20-Poly1305 from
`forge/packages/link/src/chacha.ts`. The frame format is shared with the command-line client (`forge/packages/link/src/frame.ts`).

**Install** (`/flash/`): writes PalanuOS to a board over Web Serial with esptool-js, from the last `./pal build` (ADR-0015).

## Known limits

- `ws://` without TLS: https pages (e.g. GitHub Pages) are blocked by the browser; open from a file or localhost.
- One client per transport on the device; a connection that has not AUTHed is dropped after 60 seconds.
- The data channel after AUTH is not encrypted (see `docs/PLP.md`, "Threat model"): use a trusted network.
- Opening a serial port resets the board (DTR/RTS); Web Forge repeats HELLO until the device answers.
- A catalogue on another site must allow the page to read it (CORS); GitHub's raw files do.
- One device per tab: starting the simulator disconnects a board, and the other way round.

Tested (20 November 2026, ADR-0021): the simulator in the page (shell, files, Store install, restart keeping its files, settings,
phone width, light and dark) and the native simulator over WebSocket (screen, keys, files, disconnect). Earlier (2026-11-14): pairing
and re-auth over WebSocket, and the hhv2 board over Wi-Fi. A board over USB or Bluetooth has not been re-tested with this page.
Automated transport test: `forge/packages/link/test/ws.test.ts`.

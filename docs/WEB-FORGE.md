# Web Forge

A remote for PalanuOS devices straight from the browser, with no server: all logic (protocol, pairing, crypto, screen,
terminal) runs in the page. Code: `forge/web` (TypeScript, bundled into a single HTML file).

## Running

```
./pal web                       # build then serve at http://localhost:5174
bun forge/web/build.ts          # build only: forge/web/dist/web-forge.html (a single file, no network)
```

The built file can be opened straight from disk in Chrome or Edge, or copied to another computer. `serve.ts` only
serves that file (localhost counts as a secure context, a requirement of Web Serial and Web Bluetooth).

## Transports

| Option | Browser API | Device | Browser |
| --- | --- | --- | --- |
| USB | Web Serial | the board's USB UART (console and PLP share the cable); trusted cable, no pairing | Chrome, Edge (computer) |
| Bluetooth | Web Bluetooth | the Palanu Link GATT service (`docs/PLP.md`) | Chrome, Edge (computer, Android) |
| Wi-Fi / LAN | WebSocket | port 7001 (`system/src/link_ws.c`) | all modern browsers |

Wi-Fi and Bluetooth are untrusted until AUTH. Pair once: open the pairing window on the device (Settings > Remote >
Pair, or `link pair` in its terminal), type the 6-digit code in Web Forge. The token is stored only in that browser's `localStorage`
(key `palanu.webforge.links`); "Forget pairing" deletes it, `link revoke` on the device revokes it.

Crypto: X25519 and HMAC-SHA256 from WebCrypto (Chrome/Edge 133, Safari 17, Firefox 130), ChaCha20-Poly1305 from
`forge/packages/link/src/chacha.ts`. The frame format is shared with the command-line client (`forge/packages/link/src/frame.ts`).

## Features

- **Screen**: a mirror of the screen (screen channel) on a canvas; click then type, special keys Home, Switch, Esc, Keyboard.
- **Terminal**: a remote shell (tty channel) with xterm.js; the size follows the window.
- **Files**: browse, download, upload (fs channel).
- **Device**: info, ping, firmware update (ota channel), serial console (USB path).

## Known limits

- `ws://` without TLS: https pages (e.g. GitHub Pages) are blocked by the browser; open from a file or localhost.
- One client per transport on the device; a connection that has not AUTHed is dropped after 60 seconds.
- The data channel after AUTH is not encrypted (see `docs/PLP.md`, "Threat model"): use a trusted network.
- Opening a serial port resets the board (DTR/RTS); Web Forge repeats HELLO until the device answers.

Tested (2026-11-14): the simulator over WebSocket (pairing, re-auth with the stored token, screen, input, shell, upload,
ping), and the hhv2 board over Wi-Fi (pairing and screen mirror). Automated transport test: `forge/packages/link/test/ws.test.ts`.

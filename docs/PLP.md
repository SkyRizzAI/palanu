# Palanu Link Protocol (PLP), version 1

A single frame format on top of any transport that is a byte stream: UART, TCP, WebSocket, and BLE.
This specification is enough to write a client without reading the firmware code. Reference implementations: `system/src/link.c` (device)
and `forge/packages/link` (computer).

## Frame

```
+------+------+------+---------+-----------+---------+
| 0xAB | chan | type | len u16 | payload   | crc u16 |
+------+------+------+---------+-----------+---------+
  1      1      1      2 (LE)    len bytes   2 (LE)
```
- CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over `chan`, `type`, `len`, and `payload`.
- `len` is at most 1024.
- Bytes outside a frame belong to the console path: on UART, bytes that do not start with 0xAB are forwarded to the console shell, so one
  cable serves a normal terminal and PLP at the same time. 0xAB never appears as the start of valid UTF-8 text.
- A damaged frame (bad CRC) is silently dropped; the client resends a request that goes unanswered.

## Channels

| Channel | Name | Direction | Content |
|---|---|---|---|
| 0 | ctl | both | hello, device info, ping |
| 1 | tty | both | one remote shell session |
| 2 | screen | device to client | screen updates |
| 3 | input | client to device | keys |
| 4 | fs | both | list, read, write files |

### ctl (0)
| type | name | payload |
|---|---|---|
| 1 | HELLO | client name (text), from the client. Starts a new session: the device closes the shell, screen subscription, and uploads that belong to the previous client (important on UART, which never disconnects). Does not change link trust |
| 2 | INFO | JSON, from the device. Trusted link: `{"os","ver","board","chip","name","id","trusted":true,"uptime","mem_free","psram_free","slot","build","ota"}`. Untrusted link: only `{"os","ver","board","name","id","trusted":false}` |
| 3 | PING | 4 free bytes. Always answered |
| 4 | PONG | copy of the PING payload |
| 5 | PAIR_REQUEST | client to device, see "Pairing" |
| 6 | PAIR_CHALLENGE | device to client |
| 7 | PAIR_PROOF | client to device |
| 8 | PAIR_OK | device to client: encrypted token |
| 9 | AUTH_HELLO | client to device: client id (8 bytes) |
| 10 | AUTH_CHALLENGE | device to client: 16-byte nonce |
| 11 | AUTH_RESPONSE | client to device: 32-byte HMAC |
| 12 | AUTH_OK | device to client: this link is now trusted |
| 13 | AUTH_FORGET | client to device, on a link that signed in with a pairing: delete my pairing (no payload) |
| 14 | AUTH_FORGOTTEN | device to client: deleted; the device then ends the session (a link signed in as nobody, the cable, gets an error) |
| 0x7F | ERROR | `code i8` + text. Codes: see "Error codes" |

### tty (1)
| type | name | payload |
|---|---|---|
| 1 | OPEN | `cols u16`, `rows u16`. The device creates a new TTY and shell |
| 2 | DATA | raw bytes (both directions) |
| 3 | RESIZE | `cols u16`, `rows u16` |
| 4 | CLOSE | empty (both directions; the device sends it when the shell exits) |

### screen (2)
| type | name | payload |
|---|---|---|
| 1 | SUBSCRIBE | `on u8`. 0 stops. 1: the device sends the palette and the whole screen as RECT, then only the changed areas. 2: the same as RECT565 (full colour), when the device has a screen frame; a device without one answers as for 1 |
| 2 | PALETTE | `first u16`, `count u16`, then `count` little-endian RGB565 values (standard byte order, not the panel bus order) |
| 3 | RECT | `x u16`, `y u16`, `w u16`, `h u16`, then per row: `n u16` + `n` PackBits bytes over palette indices |
| 4 | REFRESH | ask for the whole screen to be resent |
| 5 | SIZE | `w u16`, `h u16` (from the device on subscribe) |
| 6 | RECT565 | as RECT, but each row is PackBits over 16-bit pixels: RGB565, little endian |

PackBits: a byte `n` 0..127 means copy `n+1` literal bytes; `n` -127..-1 means repeat the next byte `1-n` times. In RECT565 the unit is a
2-byte pixel instead of a byte. The device's screen is one RGB565 frame (docs/DESKTOPS.md); for a client that subscribed with 1 it maps
each pixel to the palette's 6x6x6 colour cube (indices 16..231).

### input (3)
| type | name | payload |
|---|---|---|
| 1 | KEY | `sym u8`, `mods u8`, `cp u32 LE` (6 bytes; `sym` = a `pal_keysym_t` value, mods bits: 1 shift, 2 ctrl, 4 alt) |

### fs (4)
A request carries an `id u8` first; every reply repeats that `id`.
| type | name | payload |
|---|---|---|
| 1 | LIST | `id`, path. Answered with several ENTRY then END |
| 2 | ENTRY | `id`, `type u8` (1 file, 2 folder), `size u32`, name |
| 3 | END | `id` |
| 4 | READ | `id`, path. Answered with several DATA then END |
| 5 | DATA | `id`, file bytes (when from the client: part of a WRITE) |
| 6 | WRITE | `id`, path. Answered with OK. Then the client sends DATA (at most 512 bytes) and waits for OK for each part (flow control without a large buffer), then END, answered with OK |
| 7 | OK | `id` |
| 8 | REMOVE | `id`, path: a file or an empty folder. Answered with OK or ERR |
| 9 | MKDIR | `id`, path. Answered with OK or ERR |
| 10 | RENAME | `id`, from, a NUL byte, to. Answered with OK or ERR |
| 11 | INSTALL | `id`, path of a `.papp` already written: installed as `app install` does. Answered with OK, or ERR with the reason as text |
| 0x7F | ERR | `id`, `code i8`, text |

### ota (5)
Firmware update. Each part is answered before the next part is sent.
| type | name | payload |
|---|---|---|
| 1 | BEGIN | `size u32` image length. The device picks the slot that is not running and erases it |
| 2 | DATA | image bytes (max 512 per frame) |
| 3 | END | empty. The device checks the image, makes it the next boot, answers OK, then restarts |
| 4 | OK | empty; the answer to BEGIN, DATA, END |
| 0x7F | ERR | `code i8` |

Device info contains `slot` (the running slot), `build` (git hash), and `ota` (true if updates are supported). The new image
must confirm itself (10 seconds after boot); if the device restarts before that, the bootloader goes back to the old image.

## Transports

- **Virtual cable** (the browser simulator, ADR-0021): Web Forge and the WebAssembly system share two memory rings (`pal_web_link_*`); the
  device side is a cable link (`PAL_LINK_UART`), so it is trusted as the cable is.
- **UART**: 115200 baud 8N1. Opening the port on macOS resets the board (DTR/RTS), so the client repeats HELLO until answered.
  On the hhv2 board one USB-UART cable serves the console and PLP at the same time.
- **TCP**: port 7000 on the device (once Wi-Fi is connected); one client at a time, the session ends when the connection drops.
  Simulator: `palanu-sim --link PORT`. `--link-open` trusts TCP without pairing (for tests only; closed by default).
  `--log` prints the device log to stdout (tests read the pairing code from there).
- **WebSocket**: port 7001 on the device (once Wi-Fi is connected), `ws://ADDRESS:7001`; simulator: `palanu-sim --ws PORT`.
  For browsers, which cannot open raw TCP: Web Forge (`forge/web`, `docs/WEB-FORGE.md`). A plain RFC 6455 handshake
  (no subprotocol, no extensions). Every message from the client (binary or text, may be split anywhere) is a
  piece of the PLP stream; every device send is one binary message. Ping is answered with pong; close ends the session. One client
  at a time and a 60-second limit without AUTH, the same as TCP. A plain HTTP request is answered with `426 Upgrade Required`.
  No TLS: a page opened over https may not open `ws://`, so Web Forge is opened from a file or localhost.
- **BLE** (board, peripheral role, one connection): the device advertises itself as `PalanuOS` with one GATT service.
  The PLP stream bytes are exactly the same as UART and TCP; the client opens one session per connection and the session ends when the connection drops.

  | Characteristic | UUID | Properties | Function |
  |---|---|---|---|
  | service | `6f1d0001-3b7a-4c1f-8f8b-52d1371c9a4e` | primary | |
  | rx | `6f1d0002-3b7a-4c1f-8f8b-52d1371c9a4e` | write, write without response | the client writes PLP bytes |
  | tx | `6f1d0003-3b7a-4c1f-8f8b-52d1371c9a4e` | notify | the device sends PLP bytes, split by MTU (MTU-3 bytes per notification) |

  A frame may be split across writes/notifications; the decoder joins them. The client must subscribe to tx before sending HELLO.
  Status: tested on the board with nRF Connect on an iPhone (connect, service discovery, subscribe to tx, PING on rx answered with the correct PONG). Not yet tested: file transfer and shell over BLE, and over BLE the bandwidth is far below UART.

Measured values (board, 115200 baud): ping 10 ms, writing a 20 KB file about 2.8 s, reading 20 KB about 1.9 s.

## Trust

Every link has a transport, and the transport decides whether the link is trusted:

| Transport | Default | Becomes trusted |
|---|---|---|
| UART (cable) | trusted | always, unless `link.uart.trust=off` |
| TCP (Wi-Fi) | untrusted | after a successful AUTH on that connection |
| BLE | untrusted | after a successful AUTH on that connection |
| WebSocket | untrusted | after AUTH |

An untrusted link only accepts HELLO, PING, and pairing/auth messages (the ctl channel). Frames on other channels (tty, screen, input,
fs, ota, app) are rejected with error `E_AUTH` (code -23) on that same channel: for fs in the form `id, code, text`, for
the others `code, text`. Frames are never forwarded to the shell, files, or OTA.

`link.uart.trust` is stored in `/data/link/config` (`key=value` lines). `link trust uart off` takes effect immediately on the cable link
that is currently connected: its session is ended and the next frame is rejected.

## Pairing

Pairing connects a new client to the device. The device opens a pairing window for 120 seconds with a random 6-digit code
(`link pair` in the device shell, or later from the Settings screen). The code is displayed in the shell and in the log. The code is never
sent as plain text.

```
client                                          device (pairing window open)
PAIR_REQUEST  nl u8, name (<= 24), cpub[32], cn[16]  -->
                                                 dsk, dpub = X25519 (ephemeral), dn[16] random
              <--  PAIR_CHALLENGE  dpub[32], dn[16]
shared = X25519(csk, dpub)
PAIR_PROOF    proof[32]                          -->   (the device knows the code; the window is checked first)
                                                 correct: create the client (8-byte id, random 32-byte token), close the window
              <--  PAIR_OK  id[8], nonce[12], ct[32], tag[16]   (ChaCha20-Poly1305, see below)
```

Key derivation (transcript `T = cpub || dpub || cn || dn`, 96 bytes):
- `prk = HMAC-SHA256(key = "palanu-pair-v1", shared || code)`
- `k_mac = HMAC-SHA256(prk, "mac" || T)`, `k_enc = HMAC-SHA256(prk, "enc" || T)`
- `proof = HMAC-SHA256(k_mac, "pair-proof" || T)`, sent by the client in PAIR_PROOF
- `ct, tag = ChaCha20-Poly1305(k_enc, nonce, aad = "palanu-token" || id, token)`, sent in PAIR_OK

Rules:
- Wrong code: the device decrements the window's attempts. There are 3 attempts per window (not per connection), so reconnecting
  does not reset the count. The reply is `E_AUTH` with the text "wrong code, N tries left". The third wrong attempt closes the
  window.
- Window closed (timeout, already used, or three wrong attempts): `E_PAIR_CLOSED`. The window is checked first, so
  after closing the answer is always "closed".
- One-time code: a successful pairing closes the window.
- Limit: 8 stored clients. The 9th client is rejected with `NEMA_ERR_FULL` (-15).
- PAIR_OK does not make the link trusted. The client must AUTH afterwards (a single path for everything).

An honest note on the strength of the code (see also "Threat model"): 6 digits with 3 online attempts is enough for pairing
on a trusted network. An attacker who is in the middle of the path (man-in-the-middle) during the 120-second window can
guess the code offline: they hold both ends of the DH, so they know `shared`, and 10^6 guesses of `prk` only need
HMAC, not an X25519 per guess. Pairing must therefore be done over the cable or on a network that is not under attack.

## Auth

After pairing, the client opens a new connection and proves its token:

```
client                                          device
HELLO                                    -->   INFO (untrusted)
AUTH_HELLO    id[8]                       -->
                                          <--  AUTH_CHALLENGE  nonce[16]
AUTH_RESPONSE mac[32]                     -->   mac = HMAC-SHA256(token, nonce || id || tag)
                                          <--  AUTH_OK         (link trusted)
```

`tag` is one transport byte: 1 cable, 2 TCP, 3 BLE, 4 WebSocket. This response is bound to the transport and to the nonce: it cannot be
replayed on another connection.
- An unknown id still gets an AUTH_CHALLENGE (it does not leak the client list), then fails at AUTH_RESPONSE.
- One challenge, one response. A response without AUTH_HELLO: `NEMA_ERR_STATE`.
- Rate limit per transport: five failed AUTH_RESPONSEs in a row start a 30-second back-off for that transport. During
  that time AUTH_HELLO and AUTH_RESPONSE are answered with `NEMA_ERR_BUSY` (-4), even with the correct token. The cable is not affected.
- A success resets the count.

## Storage

- Clients are stored in `/data/link/clients/<id hex>.cli`: one file per client. Its content is `"PLC1" || nonce[12] ||
  ct[73] || tag[16]` with ChaCha20-Poly1305 under the device key (`psys_keystore_device_key`, the same as the keystore).
  Plaintext: id(8), name(25, NUL-padded), token(32), created(4), last_used(4). AAD: `"PLC1" || id`, so a file that was moved
  or changed is rejected (`NEMA_ERR_AUTH`). Writing goes through a temporary file then a rename.
- Device id: 8 random bytes in `/data/link/device_id` (hex). Not a secret: the client uses it to look up its token.
- Computer side: `~/.config/palanu/links.json` (mode 600), key = device id, content = client id, token, name, pairing time.
  `PALANU_CONFIG_DIR` moves that directory.

## Revocation

`link revoke ID|all` (a trusted shell, so from the cable or from a client that has already AUTHed) deletes the client file and
ends its live session: that link becomes untrusted, and the TCP connection is closed on the next read iteration (about 0.5
seconds). The old token is rejected on the next AUTH. Note: a BLE session is not cut by revocation (not yet wired to
GATT disconnect); it only loses trust and cannot open any channel until it AUTHs again.

## Threat model

Protected (assuming the device and computer are free of malware):
- Without a token, there is no shell, files, OTA, or app install over TCP and BLE.
- The token is never sent as plain text; it cannot be replayed (nonce and transport tag).
- The token is stored encrypted on the device; a file that was changed or moved is rejected.
- Revocation cuts the running session (TCP) and rejects the old token.
- Online code guessing is limited: 3 attempts per window, a 120-second window, a one-time code.

NOT protected in this version:
- There is no encryption of the data channel after AUTH. Frames after AUTH (shell, files, OTA) are sent as plain text and are not
  authenticated per frame. An attacker on the Wi-Fi path (same network) can read and modify an authenticated session.
  Recommendation: use SSH (an SSH client already exists in sdk/) or only trusted networks for Palanu Link over Wi-Fi.
  A session encryption layer is planned work (see "Plans").
- A man-in-the-middle during the pairing window can steal the token (see the note in "Pairing"). Pairing over the cable is not
  affected.
- BLE has not been tested on the board with a pairing client; revocation does not cut the BLE connection (see "Revocation").
- An untrusted channel still accepts HELLO and PING from anyone; the content is only the name, board, version, and id.
- One TCP client at a time: a connection that has not AUTHed is closed after 60 seconds, so an unknown peer cannot
  hold the only slot. A connection-flooding attack can still disrupt the service (there is no per-address rate limit).

## Error codes

| Code | Name | Meaning |
|---|---|---|
| -23 | `NEMA_ERR_AUTH` (E_AUTH) | channel rejected before AUTH; wrong pairing code; wrong token |
| -27 | `PAL_LINK_E_PAIR_CLOSED` | no pairing window, or it has closed |
| -4 | `NEMA_ERR_BUSY` | back-off after five failed AUTHs |
| -15 | `NEMA_ERR_FULL` | there are already 8 clients |
| -16 | `NEMA_ERR_STATE` | message out of order (e.g. PAIR_PROOF before PAIR_REQUEST) |
| -2 | `NEMA_ERR_INVAL` | wrong payload length or format |

## Plans

- A session encryption layer after AUTH (session key from X25519 and the AUTH transcript, AEAD per frame) so that Wi-Fi no longer
  needs to be trusted.
- A pairing code verified on both sides with a screen (Settings → Remote), and pairing over BLE.
- BLE disconnect on revocation. WebSocket over TLS (wss) so that Web Forge can be hosted on https.

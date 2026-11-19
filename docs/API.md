# PalanuOS app API

Generated from `api/palanu/*.h` by `forge/packages/apigen`. Do not edit it by hand: change the definitions,
then run `bun forge/packages/apigen/src/cli.ts`. `./pal check` rejects a file that is out of sync.

API level: **7**. WebAssembly import module: `palanu`. SDK header: `sdk/palanu.h`.
The full list of names, signatures and permissions is in `api/symbols.csv`; `./pal check` enforces the change rules.

## Modules

| Module | Contents |
|---|---|
| `crypto` | Cryptography, run by the system: the heavy work is native, on the chip's AES, SHA and ECC blocks where it has them, so an app passes buffers and handles. Handles index a table of the calling process: 8 digests (hash or HMAC) and 8 ciphers (AEAD or AES-CTR). The system frees them when the app ends. Every buffer comes with its length. The lengths of fixed-size values are checked: a key of 32 bytes is 32, not "up to 32". |
| `dev` | Board devices (API level 3, pins and sensors from 4, bus transfers from 5): the LEDs, buzzers, sensors, batteries and pins the board declares, found at run time so one app runs on a board with one LED and on a board with five. A device has an index (0 to pal_dev_total() - 1; stable while the board runs), an id such as "led0", a kind, and a name. Needs the dev permission in the manifest. The same devices are on the terminal: the dev command. |
| `events` | Waiting and events. pal_wait waits on several handles at once: a file or pipe end, a terminal, or the process event queue from pal_events. Each record of the array is 8 bytes, little-endian: int32 h (the handle), uint16 want (the PAL_POLL_IN and PAL_POLL_OUT bits the app waits for) and uint16 got (what is ready, written by the call). At most 16 records. The event queue reports RESIZE (the terminal size changed) and QUIT (a stop was asked: Ctrl-C, TERM, HUP). Each event is 16 bytes, little-endian: uint32 type, a, b, c. Types KEY, FOCUS, BLUR, NOTIFY, SETTING and EXPOSE are reserved and not produced yet. A stop request ends a wait that does not include the queue, like a read; when the queue is in the wait it becomes ready instead, and reading QUIT takes the request. |
| `fs` | Files, in the app's own namespace. An app sees /data (its persistent directory, kept across runs), /tmp (private to one run, removed when the app ends), /home (needs fs.home) and /sd (needs fs.media). Any other path is refused with PAL_ERR_PERM, and ".." cannot climb out of the namespace. A relative path starts in /data. Legacy manifests (API level 1) with the "fs" permission also get /home. A handle the app opens is closed by the system when the app ends, so an app that forgets close does not leak it. |
| `io` | The standard streams. fd 0 is stdin, 1 is stdout, 2 is stderr; each may be a terminal, a pipe or a file. An app's stdio is its process handles, so pipes, redirects, Ctrl-C and ps work without code in the app. |
| `keys` | Keystore: private keys are made, imported and used inside the system. An app gets a handle and the public key, and asks for signatures; the private bytes never enter the app's memory. Keys are stored encrypted under a device key in the system's data area (/data/keystore), which no app can open through the file API. Needs the "crypto.keys" permission in the manifest. The device key comes from a random secret made at first use: on the ESP that random source is only true random while the radio runs (Wi-Fi or BLE), so make the first key after the radio has started. |
| `kv` | Per-app key-value store (API level 2). Each app keeps its own keys in /apps/<id>/kv.dat, outside its /data namespace, so the file API cannot reach them and no other app can read them. No capability is needed. Keys are 1 to 32 characters of [a-z0-9._-]; a value is up to 1024 bytes; an app holds at most 64 keys. Writes are durable: a power cut leaves the old value or the new one, and a half-written record is ignored. Native code and the built-in apps use the id "system". |
| `legend` | Key legends (API level 2): the bar at the bottom of the screen that says what the keys do. While the app's terminal is in front, the bar shows the legends the app published; the system's own apps fill the bar themselves. A legend names an action, not a key: the board prints its own key cap, so one text works on every board ("ok" is ENTER on hhv2). The text is items separated by ';', each NAME or NAME=label. NAME is one of ok, back, move, move_h, move_v, up, down, tab, menu, delete, home (any case), or a key cap in brackets for any other key: "ok=Jump;back=Quit;[SPACE]=Pause". An action the board has no key for is not shown, and items that do not fit the width are dropped from the right. The legends stay until the app changes them or ends. Showing or hiding the bar changes the terminal's rows: the app gets a RESIZE event. No permission is needed. |
| `log` | Logging. Lines go to the system log (kept in memory, and printed on the console when that is on), tagged with the app's id, so the log shows which app wrote each line. |
| `mem` | Memory. A WebAssembly app allocates from its own linear memory, with the allocator its SDK or libc brings; there is no pal_alloc in this API. The system charges the linear memory to the app and caps it at the manifest's "mem" (KB, 256 when not given): memory.grow past the cap fails, so an app never holds more than its manifest says. Native code (the system and the built-in apps) uses the kernel allocator, which has classes and owners. |
| `net` | TCP and DNS, as a client or a server (IPv4), TLS on a TCP connection, and HTTP and HTTPS requests (API level 7). A socket is an ordinary handle: pal_read, pal_write, pal_close, and pal_wait work on it, also after pal_tls_start. Connect, resolve, accept, the TLS handshake and a request take a timeout and stop at once when the app is asked to end (Ctrl-C, kill). An app's TLS always checks the server's certificate against the system's trusted roots. UDP is not in this level. |
| `notify` | Notifications and toasts (API level 2). A notification has a title (up to 31 characters) and a text (up to 95); the system keeps the last 32 and the notification center shows them. A toast is a short message the system shows on top of the screen. Posting needs the notify permission. An app posts at most 4 notifications or toasts per second; the excess is refused with PAL_ERR_BUSY. Until the notification center exists, notifications are kept and logged, and shown by the notify shell command. |
| `proc` | The app itself and its child processes: how it ends, what it was started with, the environment, and the commands it starts. A command line is argv[0] (the command name), then the arguments the user typed. Children are processes like any other: they share the app's stdin, stdout and stderr, and join its process group, so Ctrl-C reaches them too. An app may only wait for or signal the children it started. A child it neither waited for nor detached is reaped by the system when the app ends. |
| `settings` | System settings (API level 2). A setting is a text key with a text value: ui.dark (0\|1), ui.theme (index or name), ui.size (0 small, 1 medium), time.tz (minutes from UTC), display.brightness (0 to 100; the panel keeps at least 5), term.font (0 to 2), device.name, and the radio policy radio.wifi and radio.ble (read only: the wifi and ble commands change them). Reading needs the settings.read permission and changing a value needs settings.write. A change applies at once and is kept across restarts. Unknown keys give PAL_ERR_NOENT; a read-only key gives PAL_ERR_PERM. |
| `svc` | Services (API level 2). A service is a background app the supervisor starts (kind=service); it calls pal_svc_ready when it has finished starting, which makes its state RUNNING at once instead of after the start timeout. No permission is needed for this call. Starting, stopping, and reading other services is the svc shell command and, later, the svc.control permission. |
| `time` | Time: the clock since boot (for measuring), sleeping, and the calendar time (for showing). Sleeping and the wait calls stop an app that Ctrl-C or kill has asked to end. The calendar clock has no battery: it starts at the firmware's build time until the user, SNTP or a connected Palanu Link client sets it. |
| `tty` | The terminal the app talks to through stdin and stdout. The app's terminal is its controlling terminal: the first of stdin, stdout and stderr that is a terminal. A mode replaces the one before it: PAL_TTY_COOKED (0) lines are edited and echoed, and Ctrl-C stops the app. This is the default. PAL_TTY_RAW (1) every byte arrives as it is typed, Ctrl-C included (as byte 0x03; it does not stop the app). PAL_TTY_NOSIG (2) identical to RAW on this system: raw input and no signals from the keyboard. Kept for code written for terminals where raw and no-signal modes differ. PAL_TTY_NOECHO (4) cooked lines without echo, for typing passwords. When the app ends, the terminal goes back to the mode it had before the app first changed it, however the app ended. |
| `ui` | App screens (API level 6, ADR-0010 §8). An app that is not a terminal app shows one screen of one of two kinds: A form is data: a list of fields (sections, information, switches, choices, sliders, actions, editable text), each with an id the app picks. The running desktop session lays it out in its own look, so the same app looks native under every desktop. The app waits for events (a field changed, an action ran, the screen closed) with pal_form_next. A canvas is pixels: an RGB565 area the size the system gives (below the session's bar), which looks the same under every desktop. The app sends rows of pixels with pal_canvas_present and reads keys with pal_canvas_key. An app has at most one screen at a time; opening one brings it to the front. The screen goes when the app closes it or ends. The records are little endian. No permission is needed. |

## crypto

Cryptography, run by the system: the heavy work is native, on the chip's AES, SHA and ECC blocks where it has them, so an app passes buffers and handles. Handles index a table of the calling process: 8 digests (hash or HMAC) and 8 ciphers (AEAD or AES-CTR). The system frees them when the app ends. Every buffer comes with its length. The lengths of fixed-size values are checked: a key of 32 bytes is 32, not "up to 32".

### Constants

| Name | Value |
|---|---|
| `PAL_HASH_SHA1` | `1` |
| `PAL_HASH_SHA256` | `2` |
| `PAL_HASH_SHA384` | `3` |
| `PAL_HASH_SHA512` | `4` |
| `PAL_AEAD_AES128_GCM` | `1` |
| `PAL_AEAD_AES256_GCM` | `2` |
| `PAL_AEAD_CHACHA20_POLY1305` | `3` |
| `PAL_ERR_UNSUPPORTED` | `(-21)` |
| `PAL_ERR_AUTH` | `(-23)` |

### Functions

#### `pal_random`

```c
int32_t pal_random(void *buf, uint32_t len);
```

Fills buf with random bytes from the chip's TRNG. Returns 0, or a negative error.

import `random` · since 1

#### `pal_hash_open`

```c
int32_t pal_hash_open(int32_t alg);
```

Starts a hash with alg (PAL_HASH_*). Returns a handle, or a negative error (the table holds 8).

import `hash_open` · since 1

#### `pal_hmac_open`

```c
int32_t pal_hmac_open(int32_t alg, const void *key, uint32_t klen);
```

Starts an HMAC with alg and key (any length). Feed it with pal_hash_update and finish with pal_hash_final.

import `hmac_open` · since 1

#### `pal_hash_update`

```c
int32_t pal_hash_update(int32_t h, const void *d, uint32_t n);
```

Adds n bytes to a hash or HMAC started with pal_hash_open or pal_hmac_open. Returns 0, or a negative error.

import `hash_update` · since 1

#### `pal_hash_final`

```c
int32_t pal_hash_final(int32_t h, void *out, uint32_t cap);
```

Writes the digest to out (cap must hold it) and frees the handle. Returns the digest length, or a negative error.

import `hash_final` · since 1

#### `pal_aead_open`

```c
int32_t pal_aead_open(int32_t alg, const void *key, uint32_t klen);
```

Starts an AEAD with alg and a key of 16 (AES-128) or 32 bytes (AES-256, ChaCha20). Returns a handle.

import `aead_open` · since 1

#### `pal_aead_seal`

```c
int32_t pal_aead_seal(int32_t h, const void *nonce, uint32_t nlen, const void *aad, uint32_t alen, void *buf, uint32_t len, void *tag, uint32_t tlen);
```

Encrypts buf in place and writes the 16-byte tag. The nonce is 12 bytes and is never reused with a key.

import `aead_seal` · since 1

#### `pal_aead_decrypt`

```c
int32_t pal_aead_decrypt(int32_t h, const void *nonce, uint32_t nlen, const void *aad, uint32_t alen, void *buf, uint32_t len, const void *tag, uint32_t tlen);
```

Checks the 16-byte tag and, only if it matches, decrypts buf in place. Returns 0, or PAL_ERR_AUTH with buf unchanged.

import `aead_decrypt` · since 1

#### `pal_aes_ctr_open`

```c
int32_t pal_aes_ctr_open(const void *key, uint32_t klen, const void *iv, uint32_t ivlen);
```

Starts AES-CTR with a 16 or 32 byte key and a 16-byte initial counter block. Returns a handle.

import `aes_ctr_open` · since 1

#### `pal_cipher_xor`

```c
int32_t pal_cipher_xor(int32_t h, void *buf, uint32_t len);
```

XORs buf with the next keystream bytes of a pal_aes_ctr_open handle. Calls continue the stream.

import `cipher_xor` · since 1

#### `pal_chacha20_xor`

```c
int32_t pal_chacha20_xor(const void *key, uint32_t klen, const void *nonce, uint32_t nlen, uint64_t counter, const void *in, uint32_t ilen, void *out, uint32_t olen);
```

ChaCha20 keystream XOR: nlen 8 is the original 64-bit counter form (OpenSSH), 12 is RFC 8439 (32-bit counter).

import `chacha20_xor` · since 1

#### `pal_poly1305`

```c
int32_t pal_poly1305(const void *key, uint32_t klen, const void *msg, uint32_t mlen, void *tag, uint32_t tlen);
```

Poly1305 one-time authenticator: 32-byte key, 16-byte tag. The raw primitive, not the AEAD.

import `poly1305` · since 1

#### `pal_x25519`

```c
int32_t pal_x25519(void *out, uint32_t olen, const void *scalar, uint32_t slen, const void *point, uint32_t plen);
```

X25519 shared secret of scalar and point (32 bytes each). Returns NEMA_ERR_INVAL for a low-order point.

import `x25519` · since 1

#### `pal_x25519_base`

```c
int32_t pal_x25519_base(void *out, uint32_t olen, const void *scalar, uint32_t slen);
```

X25519 public key of a 32-byte scalar (the base point).

import `x25519_base` · since 1

#### `pal_ed25519_verify`

```c
int32_t pal_ed25519_verify(const void *pk, uint32_t pklen, const void *msg, uint32_t mlen, const void *sig, uint32_t slen);
```

Verifies an Ed25519 signature (64 bytes) of msg with a 32-byte public key. Returns 0, or PAL_ERR_AUTH.

import `ed25519_verify` · since 1

#### `pal_ecdsa_p256_verify`

```c
int32_t pal_ecdsa_p256_verify(const void *pub, uint32_t plen, const void *hash, uint32_t hlen, const void *sig, uint32_t slen);
```

Verifies an ECDSA P-256 signature (r || s, 64 bytes) over a SHA-256 digest with a public key X || Y (64 bytes).

import `ecdsa_p256_verify` · since 1

#### `pal_rsa_verify_pkcs1`

```c
int32_t pal_rsa_verify_pkcs1(const void *n, uint32_t nlen, const void *e, uint32_t elen, int32_t halg, const void *hash, uint32_t hlen, const void *sig, uint32_t slen);
```

Verifies an RSA PKCS#1 v1.5 signature. n and e are big-endian; halg is a PAL_HASH_* and hash its digest.

import `rsa_verify_pkcs1` · since 1

## dev

Board devices (API level 3, pins and sensors from 4, bus transfers from 5): the LEDs, buzzers, sensors, batteries and pins the board declares, found at run time so one app runs on a board with one LED and on a board with five. A device has an index (0 to pal_dev_total() - 1; stable while the board runs), an id such as "led0", a kind, and a name. Needs the dev permission in the manifest. The same devices are on the terminal: the dev command.

### Functions

#### `pal_dev_total`

```c
int32_t pal_dev_total(void);
```

How many devices the board declares (a pin or bus the board did not open to apps still counts here but answers PAL_ERR_NOENT); 0 when it has none.

import `dev_total` · since 3 · needs `dev`

#### `pal_dev_info`

```c
int32_t pal_dev_info(int32_t index, void *out, uint32_t len);
```

Writes the text 'id kind name' of device index, separated by tabs, kind being led, buzzer, battery, sensor, gpio or bus, then for a led a tab, its pixel count, a tab and its colour count (3 RGB, 1 brightness only); for a sensor a tab and its unit; for a gpio a tab, its pin and rw or r; for a bus a tab and i2c, spi or uart; NUL-terminated, into out. Returns the length without the NUL, PAL_ERR_NOENT for an index out of range, or PAL_ERR_INVAL when out is too small.

import `dev_info` · since 3 · needs `dev`

#### `pal_dev_lookup`

```c
int32_t pal_dev_lookup(const char *id);
```

The index of the device whose id is id (NUL-terminated), or PAL_ERR_NOENT.

import `dev_lookup` · since 3 · needs `dev`

#### `pal_dev_set_led`

```c
int32_t pal_dev_set_led(int32_t index, int32_t pixel, uint32_t rgb);
```

Sets one pixel of LED device index to rgb (0xRRGGBB); pixel -1 sets every pixel of it. A brightness-only LED uses the brightest channel. Returns 0, PAL_ERR_NOENT (no such device), or PAL_ERR_INVAL (not an LED, or the pixel is out of range).

import `dev_set_led` · since 3 · needs `dev`

#### `pal_dev_play_tone`

```c
int32_t pal_dev_play_tone(int32_t index, int32_t freq_hz, int32_t ms);
```

Plays a tone of freq_hz (20 to 20000) for ms milliseconds (1 to 5000) on buzzer device index. Returns 0, PAL_ERR_NOENT (no such device), or PAL_ERR_INVAL (not a buzzer, or a value out of range).

import `dev_play_tone` · since 3 · needs `dev`

#### `pal_dev_gpio_read`

```c
int32_t pal_dev_gpio_read(int32_t index);
```

Reads the level (0 or 1) of gpio device index. Returns the level, PAL_ERR_NOENT (no such device, or a pin the board did not open to apps), PAL_ERR_INVAL (not a readable pin) or PAL_ERR_IO.

import `dev_gpio_read` · since 4 · needs `dev`

#### `pal_dev_gpio_write`

```c
int32_t pal_dev_gpio_write(int32_t index, int32_t level);
```

Drives gpio device index to level (0 or non-zero). Returns 0, PAL_ERR_NOENT, PAL_ERR_INVAL (not an output pin) or PAL_ERR_IO.

import `dev_gpio_write` · since 4 · needs `dev`

#### `pal_dev_sensor_read`

```c
int32_t pal_dev_sensor_read(int32_t index, void *out, uint32_t len);
```

Reads sensor device index into out (len must be at least 4): one little-endian int32, the value times 1000 in the unit pal_dev_info reports (21500 is 21.5). Returns 0, PAL_ERR_NOENT, PAL_ERR_INVAL (not a sensor) or PAL_ERR_IO.

import `dev_sensor_read` · since 4 · needs `dev`

#### `pal_dev_read_battery`

```c
int32_t pal_dev_read_battery(int32_t index);
```

The charge of battery device index, 0 to 100, or -1 when the board cannot tell. Returns PAL_ERR_NOENT or PAL_ERR_INVAL (not a battery) otherwise.

import `dev_read_battery` · since 4 · needs `dev`

#### `pal_dev_i2c_probe`

```c
int32_t pal_dev_i2c_probe(int32_t index, int32_t addr);
```

Asks I2C bus device index whether a device answers at 7-bit address addr (3 to 119). Returns 1 when it does, 0 when not, PAL_ERR_NOENT, PAL_ERR_INVAL (not an I2C bus or a bad address) or PAL_ERR_IO.

import `dev_i2c_probe` · since 4 · needs `dev`

#### `pal_dev_bus_xfer`

```c
int32_t pal_dev_bus_xfer(int32_t index, int32_t addr, const void *tx, uint32_t txn, void *rx, uint32_t rxn);
```

One transfer on bus device index (I2C, SPI or UART): writes tx (txn bytes, may be 0) then reads rxn bytes into rx (may be 0). addr is the I2C 7-bit address or the SPI chip-select index, and ignored on UART. Returns 0, PAL_ERR_NOENT, PAL_ERR_INVAL (not a bus, no transfer, or more than 256 bytes) or PAL_ERR_IO.

import `dev_bus_xfer` · since 5 · needs `dev`

## events

Waiting and events. pal_wait waits on several handles at once: a file or pipe end, a terminal, or the process event queue from pal_events. Each record of the array is 8 bytes, little-endian: int32 h (the handle), uint16 want (the PAL_POLL_IN and PAL_POLL_OUT bits the app waits for) and uint16 got (what is ready, written by the call). At most 16 records. The event queue reports RESIZE (the terminal size changed) and QUIT (a stop was asked: Ctrl-C, TERM, HUP). Each event is 16 bytes, little-endian: uint32 type, a, b, c. Types KEY, FOCUS, BLUR, NOTIFY, SETTING and EXPOSE are reserved and not produced yet. A stop request ends a wait that does not include the queue, like a read; when the queue is in the wait it becomes ready instead, and reading QUIT takes the request.

### Constants

| Name | Value |
|---|---|
| `PAL_POLL_IN` | `0x1` |
| `PAL_POLL_OUT` | `0x2` |
| `PAL_POLL_HUP` | `0x4` |
| `PAL_POLL_ERR` | `0x8` |
| `PAL_EV_NONE` | `0` |
| `PAL_EV_KEY` | `1` |
| `PAL_EV_RESIZE` | `2` |
| `PAL_EV_FOCUS` | `3` |
| `PAL_EV_BLUR` | `4` |
| `PAL_EV_QUIT` | `5` |
| `PAL_EV_NOTIFY` | `6` |
| `PAL_EV_SETTING` | `7` |
| `PAL_EV_EXPOSE` | `8` |

### Functions

#### `pal_wait`

```c
int32_t pal_wait(void *waitfor, uint32_t n_bytes, int32_t timeout_ms);
```

Waits until a record of waitfor (n_bytes / 8 records) is ready, or timeout_ms passes (-1 waits forever, 0 only checks). Writes got into each record. Returns the number of ready records, 0 on timeout, or a negative error (n_bytes not a multiple of 8, more than 16 records, or a handle the app does not hold).

import `wait` · since 2

#### `pal_events`

```c
int32_t pal_events(void);
```

Returns the process event queue as a handle. It becomes readable when an event is pending; read the events with pal_event_next. Returns a negative error when the app has no free handle.

import `events` · since 2

#### `pal_event_next`

```c
int32_t pal_event_next(void *ev, uint32_t len, int32_t timeout_ms);
```

Reads the next event into ev (16 bytes; len must be at least 16). Returns 16 when an event was written, 0 when timeout_ms passed without one (-1 waits forever, 0 only checks), or a negative error.

import `event_next` · since 2

## fs

Files, in the app's own namespace. An app sees /data (its persistent directory, kept across runs), /tmp (private to one run, removed when the app ends), /home (needs fs.home) and /sd (needs fs.media). Any other path is refused with PAL_ERR_PERM, and ".." cannot climb out of the namespace. A relative path starts in /data. Legacy manifests (API level 1) with the "fs" permission also get /home. A handle the app opens is closed by the system when the app ends, so an app that forgets close does not leak it.

### Constants

| Name | Value |
|---|---|
| `PAL_O_READ` | `0x01` |
| `PAL_O_WRITE` | `0x02` |
| `PAL_O_CREATE` | `0x04` |
| `PAL_O_TRUNC` | `0x08` |
| `PAL_O_APPEND` | `0x10` |
| `PAL_ERR_NOMEM` | `(-1)` |
| `PAL_ERR_INVAL` | `(-2)` |
| `PAL_ERR_NOENT` | `(-3)` |
| `PAL_ERR_BUSY` | `(-4)` |
| `PAL_ERR_IO` | `(-12)` |
| `PAL_ERR_RANGE` | `(-13)` |
| `PAL_ERR_BADF` | `(-14)` |
| `PAL_ERR_FULL` | `(-15)` |
| `PAL_ERR_NOSPC` | `(-17)` |
| `PAL_ERR_ISDIR` | `(-18)` |
| `PAL_ERR_NOTDIR` | `(-19)` |
| `PAL_ERR_EXIST` | `(-11)` |
| `PAL_FILE` | `1` |
| `PAL_DIR` | `2` |
| `PAL_STAT_SIZE` | `16` |
| `PAL_DIRENT_SIZE` | `64` |
| `PAL_DIRENT_NAME` | `56` |
| `PAL_SEEK_SET` | `0` |
| `PAL_SEEK_CUR` | `1` |
| `PAL_SEEK_END` | `2` |

### Functions

#### `pal_open`

```c
int32_t pal_open(const char *path, uint32_t len, int32_t flags);
```

Opens the file at path (len bytes, not NUL-terminated) with PAL_O_ flags. Returns a handle, or a negative error.

import `open` · since 1 · needs `fs`

#### `pal_close`

```c
int32_t pal_close(int32_t fd);
```

Closes a handle that this app opened. Returns 0, or a negative error.

import `close` · since 1

#### `pal_stat`

```c
int32_t pal_stat(const char *path, void *st_out, uint32_t len);
```

Writes the 16-byte record of path into st_out: uint32 size, uint32 mtime in seconds since boot, uint32 flags (PAL_FILE or PAL_DIR), uint32 reserved; little-endian. Returns 0, or a negative error.

import `stat` · since 2 · needs `fs`

#### `pal_dir_open`

```c
int32_t pal_dir_open(const char *path);
```

Opens the directory at path for pal_dir_next. Returns a handle, or a negative error. Close it with pal_close.

import `dir_open` · since 2 · needs `fs`

#### `pal_dir_next`

```c
int32_t pal_dir_next(int32_t dh, void *ent_out, uint32_t len);
```

Writes the next entry of directory handle dh into ent_out: uint32 size, uint32 flags (PAL_FILE or PAL_DIR), then the NUL-terminated name in 56 bytes. Returns 1 for an entry, 0 at the end, or a negative error.

import `dir_next` · since 2 · needs `fs`

#### `pal_seek`

```c
int64_t pal_seek(int32_t h, int64_t off, int32_t whence);
```

Moves the position of file handle h: whence is PAL_SEEK_SET, PAL_SEEK_CUR or PAL_SEEK_END. Returns the new position, or a negative error.

import `seek` · since 2 · needs `fs`

#### `pal_mkdir`

```c
int32_t pal_mkdir(const char *path);
```

Creates the directory at path. The parent must exist. Returns 0, or a negative error.

import `mkdir` · since 2 · needs `fs`

#### `pal_remove`

```c
int32_t pal_remove(const char *path);
```

Removes the file, or the empty directory, at path. Returns 0, or a negative error.

import `remove` · since 2 · needs `fs`

#### `pal_rename`

```c
int32_t pal_rename(const char *from, const char *to);
```

Renames (moves) from to to, both inside the app's namespace. Returns 0, or a negative error.

import `rename` · since 2 · needs `fs`

#### `pal_fsync`

```c
int32_t pal_fsync(int32_t h);
```

Flushes the file of handle h to its storage. Returns 0, or a negative error.

import `fsync` · since 2 · needs `fs`

## io

The standard streams. fd 0 is stdin, 1 is stdout, 2 is stderr; each may be a terminal, a pipe or a file. An app's stdio is its process handles, so pipes, redirects, Ctrl-C and ps work without code in the app.

### Constants

| Name | Value |
|---|---|
| `PAL_ERR_AGAIN` | `(-7)` |
| `PAL_ERR_INTR` | `(-6)` |
| `PAL_ERR_PERM` | `(-9)` |

### Functions

#### `pal_write`

```c
int32_t pal_write(int32_t fd, const void *buf, uint32_t len);
```

Writes len bytes from buf to fd. Returns the bytes written, or a negative error.

import `write` · since 1

#### `pal_read`

```c
int32_t pal_read(int32_t fd, void *buf, uint32_t len);
```

Reads up to len bytes from fd into buf. Returns the bytes read, 0 at end of input, or a negative error. Blocks; Ctrl-C stops the app.

import `read` · since 1

## keys

Keystore: private keys are made, imported and used inside the system. An app gets a handle and the public key, and asks for signatures; the private bytes never enter the app's memory. Keys are stored encrypted under a device key in the system's data area (/data/keystore), which no app can open through the file API. Needs the "crypto.keys" permission in the manifest. The device key comes from a random secret made at first use: on the ESP that random source is only true random while the radio runs (Wi-Fi or BLE), so make the first key after the radio has started.

### Constants

| Name | Value |
|---|---|
| `PAL_KEY_ED25519` | `1` |
| `PAL_KEY_ECDSA_P256` | `2` |

### Functions

#### `pal_key_generate`

```c
int32_t pal_key_generate(int32_t type, const char *name, uint32_t nlen);
```

Makes a new key of type (PAL_KEY_*) under name, stores it, and returns a handle. Names are [A-Za-z0-9_-], 1 to 32.

import `key_generate` · since 1 · needs `crypto.keys`

#### `pal_key_load`

```c
int32_t pal_key_load(const char *name, uint32_t nlen);
```

Loads the stored key name into this app's key table and returns a handle.

import `key_load` · since 1 · needs `crypto.keys`

#### `pal_key_import`

```c
int32_t pal_key_import(const void *blob, uint32_t blen, const char *passphrase, uint32_t plen, const char *name, uint32_t nlen);
```

Imports an unencrypted OpenSSH ed25519 key (-----BEGIN OPENSSH PRIVATE KEY-----) under name. Other forms: PAL_ERR_UNSUPPORTED.

import `key_import` · since 1 · needs `crypto.keys`

#### `pal_key_public`

```c
int32_t pal_key_public(int32_t h, void *out, uint32_t cap);
```

Writes the public key as an SSH wire blob (ssh-ed25519 plus 32 bytes, or the ecdsa-sha2-nistp256 form). Returns its length.

import `key_public` · since 1 · needs `crypto.keys`

#### `pal_key_sign`

```c
int32_t pal_key_sign(int32_t h, const void *data, uint32_t len, void *sig, uint32_t cap);
```

Signs data with the key: ed25519 gives 64 bytes; ecdsa signs its SHA-256 digest and gives r || s (64 bytes). Returns the length.

import `key_sign` · since 1 · needs `crypto.keys`

#### `pal_key_list`

```c
int32_t pal_key_list(void *out, uint32_t cap);
```

Writes the names of the stored keys, one per line, into out. Returns the length.

import `key_list` · since 1 · needs `crypto.keys`

#### `pal_key_delete`

```c
int32_t pal_key_delete(const char *name, uint32_t nlen);
```

Deletes the stored key name. Handles already loaded from it keep working until the app ends.

import `key_delete` · since 1 · needs `crypto.keys`

## kv

Per-app key-value store (API level 2). Each app keeps its own keys in /apps/<id>/kv.dat, outside its /data namespace, so the file API cannot reach them and no other app can read them. No capability is needed. Keys are 1 to 32 characters of [a-z0-9._-]; a value is up to 1024 bytes; an app holds at most 64 keys. Writes are durable: a power cut leaves the old value or the new one, and a half-written record is ignored. Native code and the built-in apps use the id "system".

### Constants

| Name | Value |
|---|---|
| `PAL_KV_KEY_MAX` | `32` |
| `PAL_KV_VALUE_MAX` | `1024` |
| `PAL_KV_KEYS_MAX` | `64` |

### Functions

#### `pal_kv_get`

```c
int32_t pal_kv_get(const char *key, void *buf, uint32_t cap);
```

Copies the value of key into buf, as many bytes as it has. Returns its length, PAL_ERR_NOENT for an unknown key, PAL_ERR_RANGE when cap is smaller than the value, or PAL_ERR_INVAL for a bad key.

import `kv_get` · since 2

#### `pal_kv_set`

```c
int32_t pal_kv_set(const char *key, const void *val, uint32_t len);
```

Stores len bytes of val under key, replacing an old value. Returns 0, PAL_ERR_INVAL (bad key, or len over 1024), or PAL_ERR_NOSPC when the app already holds 64 keys.

import `kv_set` · since 2

#### `pal_kv_del`

```c
int32_t pal_kv_del(const char *key);
```

Deletes key. Returns 0, or PAL_ERR_NOENT when the key is not stored.

import `kv_del` · since 2

#### `pal_kv_keys`

```c
int32_t pal_kv_keys(void *out, uint32_t cap);
```

Writes the stored keys into out, each followed by a newline. Returns the length, or PAL_ERR_RANGE when cap is too small.

import `kv_keys` · since 2

## legend

Key legends (API level 2): the bar at the bottom of the screen that says what the keys do. While the app's terminal is in front, the bar shows the legends the app published; the system's own apps fill the bar themselves. A legend names an action, not a key: the board prints its own key cap, so one text works on every board ("ok" is ENTER on hhv2). The text is items separated by ';', each NAME or NAME=label. NAME is one of ok, back, move, move_h, move_v, up, down, tab, menu, delete, home (any case), or a key cap in brackets for any other key: "ok=Jump;back=Quit;[SPACE]=Pause". An action the board has no key for is not shown, and items that do not fit the width are dropped from the right. The legends stay until the app changes them or ends. Showing or hiding the bar changes the terminal's rows: the app gets a RESIZE event. No permission is needed.

### Functions

#### `pal_legend_show`

```c
int32_t pal_legend_show(const char *spec);
```

Shows the legends in spec (NUL-terminated, up to 159 bytes, at most 12 items) while the app's terminal is in front; an empty spec removes them. Returns 0, PAL_ERR_INVAL (malformed text, unknown name, cap over 15 characters), or a negative error when the app has no terminal.

import `legend_show` · since 2

## log

Logging. Lines go to the system log (kept in memory, and printed on the console when that is on), tagged with the app's id, so the log shows which app wrote each line.

### Constants

| Name | Value |
|---|---|
| `PAL_LOG_ERROR` | `0` |
| `PAL_LOG_WARN` | `1` |
| `PAL_LOG_INFO` | `2` |
| `PAL_LOG_DEBUG` | `3` |

### Functions

#### `pal_log`

```c
int32_t pal_log(int32_t level, const char *msg);
```

Writes msg (NUL-terminated) to the system log at a PAL_LOG_* level. Returns 0, or a negative error for an unknown level.

import `log` · since 2

## mem

Memory. A WebAssembly app allocates from its own linear memory, with the allocator its SDK or libc brings; there is no pal_alloc in this API. The system charges the linear memory to the app and caps it at the manifest's "mem" (KB, 256 when not given): memory.grow past the cap fails, so an app never holds more than its manifest says. Native code (the system and the built-in apps) uses the kernel allocator, which has classes and owners.

## net

TCP and DNS, as a client or a server (IPv4), TLS on a TCP connection, and HTTP and HTTPS requests (API level 7). A socket is an ordinary handle: pal_read, pal_write, pal_close, and pal_wait work on it, also after pal_tls_start. Connect, resolve, accept, the TLS handshake and a request take a timeout and stop at once when the app is asked to end (Ctrl-C, kill). An app's TLS always checks the server's certificate against the system's trusted roots. UDP is not in this level.

### Constants

| Name | Value |
|---|---|
| `PAL_ERR_TIMEOUT` | `(-5)` |
| `PAL_ERR_REFUSED` | `(-26)` |
| `PAL_ERR_RESET` | `(-24)` |
| `PAL_ERR_UNREACH` | `(-25)` |
| `PAL_HTTP_HEAD_SIZE` | `8` |
| `PAL_NET_INFO_SIZE` | `12` |

### Functions

#### `pal_tcp_connect`

```c
int32_t pal_tcp_connect(const char *host, int32_t port, int32_t timeout_ms);
```

Connects to host (a name or a dotted IPv4 address, NUL-terminated) on port, waiting up to timeout_ms (negative: no limit) for the lookup and the connect. Returns a handle, or a negative error: PAL_ERR_REFUSED, PAL_ERR_UNREACH, PAL_ERR_TIMEOUT, PAL_ERR_NOENT.

import `tcp_connect` · since 2 · needs `net`

#### `pal_tcp_listen`

```c
int32_t pal_tcp_listen(int32_t port, int32_t backlog);
```

Listens on TCP port (all addresses) with the given backlog (1 to 32). Returns a listener handle, or a negative error.

import `tcp_listen` · since 2 · needs `net.listen`

#### `pal_tcp_accept`

```c
int32_t pal_tcp_accept(int32_t listener, int32_t timeout_ms);
```

Waits up to timeout_ms (negative: no limit) for a connection on listener and returns a handle to it, or a negative error (PAL_ERR_TIMEOUT when none came).

import `tcp_accept` · since 2 · needs `net.listen`

#### `pal_dns_resolve`

```c
int32_t pal_dns_resolve(const char *host, void *addr_out, uint32_t len, int32_t timeout_ms);
```

Resolves host to an IPv4 address and writes its 4 bytes (a, b, c, d) into addr_out, which needs len >= 4. Waits up to timeout_ms. Returns 0, or a negative error.

import `dns_resolve` · since 2 · needs `net`

#### `pal_net_info`

```c
int32_t pal_net_info(void *out, uint32_t len);
```

Writes the network state into out (12 bytes, see PAL_NET_INFO_SIZE): whether the link is up, its IPv4 address and the Wi-Fi signal. Returns 0, or a negative error.

import `net_info` · since 2 · needs `net`

#### `pal_set_nonblock`

```c
int32_t pal_set_nonblock(int32_t h, int32_t on);
```

on != 0: reads and writes on handle h never wait; with no data or no room they return PAL_ERR_AGAIN. on == 0: they wait again. Works on any handle the app owns.

import `set_nonblock` · since 2

#### `pal_shutdown`

```c
int32_t pal_shutdown(int32_t h, int32_t how);
```

Shuts down a socket the app owns: how is 0 (receiving), 1 (sending) or 2 (both). Returns 0, or a negative error.

import `shutdown` · since 2

#### `pal_tls_start`

```c
int32_t pal_tls_start(int32_t h, const char *host, int32_t timeout_ms);
```

Turns the connected TCP socket h into a TLS client for host (the server name, checked against its certificate): the handshake runs now, up to timeout_ms. Afterwards pal_read and pal_write on h carry plain text. Returns 0, or a negative error: PAL_ERR_AUTH (certificate not trusted), PAL_ERR_UNSUPPORTED, PAL_ERR_TIMEOUT.

import `tls_start` · since 7 · needs `net`

#### `pal_http_fetch`

```c
int32_t pal_http_fetch(const char *method, const char *url, const char *headers, const void *body, uint32_t body_len, void *out, uint32_t cap, int32_t timeout_ms);
```

Sends an HTTP or HTTPS request and waits up to timeout_ms for each step. method is GET, POST, PUT, DELETE, HEAD or PATCH; headers are extra lines of the form Name: value, separated by newlines (empty for none); body has body_len bytes (0 for none). Redirects are followed (up to 5). Writes the status, the body's full length and as much of the body as fits into out (see PAL_HTTP_HEAD_SIZE; cap >= 8). Returns the bytes written into out, or a negative error.

import `http_fetch` · since 7 · needs `net`

## notify

Notifications and toasts (API level 2). A notification has a title (up to 31 characters) and a text (up to 95); the system keeps the last 32 and the notification center shows them. A toast is a short message the system shows on top of the screen. Posting needs the notify permission. An app posts at most 4 notifications or toasts per second; the excess is refused with PAL_ERR_BUSY. Until the notification center exists, notifications are kept and logged, and shown by the notify shell command.

### Constants

| Name | Value |
|---|---|
| `PAL_NOTIFY_ALERT` | `0x01` |

### Functions

#### `pal_toast`

```c
int32_t pal_toast(const char *text, int32_t ms);
```

Shows text (NUL-terminated, up to 95 characters) as a toast for ms milliseconds (100 to 10000). Returns 0, PAL_ERR_INVAL, PAL_ERR_BUSY (rate limit), or PAL_ERR_FULL when the toast queue of 4 is full.

import `toast` · since 2 · needs `notify`

#### `pal_notify_post`

```c
int32_t pal_notify_post(const char *title, const char *text, int32_t flags);
```

Posts a notification with title and text (NUL-terminated) and flags (PAL_NOTIFY_*). Returns its id (above 0), PAL_ERR_INVAL, or PAL_ERR_BUSY (rate limit).

import `notify_post` · since 2 · needs `notify`

#### `pal_notify_update`

```c
int32_t pal_notify_update(int32_t id, const char *title, const char *text);
```

Changes the title and text of notification id, which this app posted. Returns 0, PAL_ERR_NOENT (no such id), PAL_ERR_PERM (another app's), or PAL_ERR_INVAL (too long).

import `notify_update` · since 2 · needs `notify`

#### `pal_notify_cancel`

```c
int32_t pal_notify_cancel(int32_t id);
```

Removes notification id, which this app posted. Returns 0, PAL_ERR_NOENT (no such id), or PAL_ERR_PERM (another app's).

import `notify_cancel` · since 2 · needs `notify`

## proc

The app itself and its child processes: how it ends, what it was started with, the environment, and the commands it starts. A command line is argv[0] (the command name), then the arguments the user typed. Children are processes like any other: they share the app's stdin, stdout and stderr, and join its process group, so Ctrl-C reaches them too. An app may only wait for or signal the children it started. A child it neither waited for nor detached is reaped by the system when the app ends.

### Constants

| Name | Value |
|---|---|
| `PAL_SIG_INT` | `0x01` |
| `PAL_SIG_TERM` | `0x02` |
| `PAL_SIG_KILL` | `0x04` |
| `PAL_SIG_HUP` | `0x08` |
| `PAL_SPAWN_DETACH` | `0x01` |

### Functions

#### `pal_exit`

```c
void pal_exit(int32_t code);
```

Ends the app with the given status. The status is what the shell sees.

import `exit` · since 1

#### `pal_argc`

```c
int32_t pal_argc(void);
```

Number of command line words, including the command name.

import `argc` · since 1

#### `pal_argv`

```c
int32_t pal_argv(int32_t i, void *buf, uint32_t cap);
```

Copies word i (0 is the command name) into buf, NUL-terminated. Returns its length, or a negative error if it does not fit.

import `arg` · since 1 · old name `pal_arg`

#### `pal_spawn`

```c
int32_t pal_spawn(const char *app, const char *args, uint32_t args_len, int32_t flags);
```

Starts the registered command app as a child. args holds the argument words after the command name, each ended by NUL (args_len bytes, at most 255; 0 for none). flags is 0 or PAL_SPAWN_DETACH. Returns a child handle (usable only by this app), or a negative error.

import `spawn` · since 2 · needs `proc.spawn`

#### `pal_child_wait`

```c
int32_t pal_child_wait(int32_t child, void *code_out, uint32_t len, int32_t timeout_ms);
```

Waits for a child this app started (timeout_ms as in pal_wait). When it has ended, writes its status into code_out (4 bytes, little-endian int32) and returns 1. Returns 0 on timeout, or a negative error (not one of this app's children, or the app was stopped).

import `child_wait` · since 2

#### `pal_signal`

```c
int32_t pal_signal(int32_t child, int32_t sig);
```

Sends a PAL_SIG_* signal to a child this app started. Returns 0, or a negative error (not one of this app's children, or unknown signal).

import `signal` · since 2

#### `pal_getenv`

```c
int32_t pal_getenv(const char *key, void *buf, uint32_t cap);
```

Copies the value of the environment variable key into buf, NUL-terminated. Returns its length, or a negative error if the variable is not set or the value does not fit. Set: HOME (/home), USER (palanu), TERM (xterm-256color), PWD (the app's working directory).

import `getenv` · since 2

## settings

System settings (API level 2). A setting is a text key with a text value: ui.dark (0|1), ui.theme (index or name), ui.size (0 small, 1 medium), time.tz (minutes from UTC), display.brightness (0 to 100; the panel keeps at least 5), term.font (0 to 2), device.name, and the radio policy radio.wifi and radio.ble (read only: the wifi and ble commands change them). Reading needs the settings.read permission and changing a value needs settings.write. A change applies at once and is kept across restarts. Unknown keys give PAL_ERR_NOENT; a read-only key gives PAL_ERR_PERM.

### Functions

#### `pal_setting_get`

```c
int32_t pal_setting_get(const char *key, void *buf, uint32_t cap);
```

Copies the value of key, as text, into buf with a NUL after it. Returns the length without the NUL, PAL_ERR_NOENT for an unknown key, or PAL_ERR_RANGE when cap cannot hold the value and its NUL.

import `setting_get` · since 2 · needs `settings.read`

#### `pal_setting_set`

```c
int32_t pal_setting_set(const char *key, const char *value);
```

Sets key to the text value, checks it, applies it at once, and saves it. Returns 0, PAL_ERR_NOENT (unknown key), PAL_ERR_PERM (read-only key), or PAL_ERR_INVAL (value out of range or malformed).

import `setting_set` · since 2 · needs `settings.write`

## svc

Services (API level 2). A service is a background app the supervisor starts (kind=service); it calls pal_svc_ready when it has finished starting, which makes its state RUNNING at once instead of after the start timeout. No permission is needed for this call. Starting, stopping, and reading other services is the svc shell command and, later, the svc.control permission.

### Functions

#### `pal_svc_ready`

```c
int32_t pal_svc_ready(void);
```

Tells the supervisor that this service has finished starting. Returns 0, or PAL_ERR_NOENT when the caller is not a service the supervisor started.

import `svc_ready` · since 2

## time

Time: the clock since boot (for measuring), sleeping, and the calendar time (for showing). Sleeping and the wait calls stop an app that Ctrl-C or kill has asked to end. The calendar clock has no battery: it starts at the firmware's build time until the user, SNTP or a connected Palanu Link client sets it.

### Functions

#### `pal_now_ms`

```c
int64_t pal_now_ms(void);
```

Milliseconds since boot.

import `time_ms` · since 1 · old name `pal_time_ms`

#### `pal_sleep_ms`

```c
void pal_sleep_ms(int32_t ms);
```

Sleeps for ms milliseconds, or until the app is interrupted.

import `sleep_ms` · since 1

#### `pal_unix_time`

```c
int64_t pal_unix_time(void);
```

Seconds since 1970-01-01 00:00 UTC, from the calendar clock.

import `unix_time` · since 2

#### `pal_localtime`

```c
int32_t pal_localtime(int64_t unix_s, void *tm_out, uint32_t len);
```

Writes the local time of unix_s (seconds since 1970 UTC) into tm_out, 36 bytes (len must be at least 36): nine little-endian int32 sec, min, hour, mday, mon (0 to 11), year (years since 1900), wday (0 is Sunday), yday (0 to 365), isdst (always 0). The local time is the system timezone. Returns 0, or a negative error (outside 1970 to 2106, or the buffer is too small).

import `localtime` · since 2

## tty

The terminal the app talks to through stdin and stdout. The app's terminal is its controlling terminal: the first of stdin, stdout and stderr that is a terminal. A mode replaces the one before it: PAL_TTY_COOKED (0) lines are edited and echoed, and Ctrl-C stops the app. This is the default. PAL_TTY_RAW (1) every byte arrives as it is typed, Ctrl-C included (as byte 0x03; it does not stop the app). PAL_TTY_NOSIG (2) identical to RAW on this system: raw input and no signals from the keyboard. Kept for code written for terminals where raw and no-signal modes differ. PAL_TTY_NOECHO (4) cooked lines without echo, for typing passwords. When the app ends, the terminal goes back to the mode it had before the app first changed it, however the app ended.

### Constants

| Name | Value |
|---|---|
| `PAL_TTY_COOKED` | `0` |
| `PAL_TTY_RAW` | `1` |
| `PAL_TTY_NOSIG` | `2` |
| `PAL_TTY_NOECHO` | `4` |
| `PAL_TERM_256COLOR` | `1` |
| `PAL_TERM_UTF8` | `2` |
| `PAL_TERM_ALTSCREEN` | `4` |

### Functions

#### `pal_tty_mode`

```c
int32_t pal_tty_mode(int32_t mode);
```

Sets the terminal mode to a PAL_TTY_* value (any other bit is refused). Returns 0, or a negative error when stdin, stdout and stderr are not a terminal.

import `tty_raw` · since 1 · old name `pal_tty_raw`

#### `pal_tty_get`

```c
int32_t pal_tty_get(void);
```

Returns the current terminal mode as a PAL_TTY_* value: RAW for any raw state, NOECHO for cooked without echo. Returns a negative error when there is no terminal.

import `tty_get` · since 2

#### `pal_tty_size`

```c
int32_t pal_tty_size(void *buf, uint32_t len);
```

Writes the size of the controlling terminal into buf as 4 bytes: columns, then rows, each a little-endian uint16. Needs len >= 4. 80x24 when no terminal reports a size.

import `term_size` · since 1 · old name `pal_term_size`

#### `pal_tty_caps`

```c
int32_t pal_tty_caps(void);
```

Returns the terminal capability bits: PAL_TERM_256COLOR (1), PAL_TERM_UTF8 (2), PAL_TERM_ALTSCREEN (4). The on-device terminal has all three.

import `tty_caps` · since 2

## ui

App screens (API level 6, ADR-0010 §8). An app that is not a terminal app shows one screen of one of two kinds: A form is data: a list of fields (sections, information, switches, choices, sliders, actions, editable text), each with an id the app picks. The running desktop session lays it out in its own look, so the same app looks native under every desktop. The app waits for events (a field changed, an action ran, the screen closed) with pal_form_next. A canvas is pixels: an RGB565 area the size the system gives (below the session's bar), which looks the same under every desktop. The app sends rows of pixels with pal_canvas_present and reads keys with pal_canvas_key. An app has at most one screen at a time; opening one brings it to the front. The screen goes when the app closes it or ends. The records are little endian. No permission is needed.

### Constants

| Name | Value |
|---|---|
| `PAL_FORM_SECTION` | `1` |
| `PAL_FORM_INFO` | `2` |
| `PAL_FORM_TOGGLE` | `3` |
| `PAL_FORM_CHOICE` | `4` |
| `PAL_FORM_SLIDER` | `5` |
| `PAL_FORM_ACTION` | `6` |
| `PAL_FORM_TEXT` | `7` |
| `PAL_SYM_CHAR` | `1` |
| `PAL_SYM_ENTER` | `2` |
| `PAL_SYM_ESC` | `3` |
| `PAL_SYM_BACKSPACE` | `4` |
| `PAL_SYM_TAB` | `5` |
| `PAL_SYM_DELETE` | `6` |
| `PAL_SYM_UP` | `7` |
| `PAL_SYM_DOWN` | `8` |
| `PAL_SYM_LEFT` | `9` |
| `PAL_SYM_RIGHT` | `10` |
| `PAL_FORM_CHANGED` | `1` |
| `PAL_FORM_ACTIVATED` | `2` |
| `PAL_FORM_CLOSED` | `3` |

### Functions

#### `pal_form_open`

```c
int32_t pal_form_open(const char *title);
```

Opens the app's form screen with title (up to 31 bytes) and brings it to the front, with no fields. Returns 0, PAL_ERR_BUSY (the app has a canvas open, or every screen slot is taken), or PAL_ERR_UNSUPPORTED when there is no screen.

import `form_open` · since 6

#### `pal_form_add`

```c
int32_t pal_form_add(int32_t id, int32_t kind, const char *label, const char *value);
```

Adds a field at the end of the form: id (0 to 65535, unique in the form), kind (PAL_FORM_*), label (up to 31 bytes) and value (its text, as each kind says, up to 95 bytes). Returns 0, PAL_ERR_INVAL, PAL_ERR_EXIST (the id is taken), PAL_ERR_FULL (32 fields), or PAL_ERR_NOENT (no form open).

import `form_add` · since 6

#### `pal_form_set`

```c
int32_t pal_form_set(int32_t id, const char *value);
```

Sets the value of field id as its kind reads it: the text of an info or text field, 0 or 1 for a toggle, the index for a choice, the number for a slider. Returns 0, PAL_ERR_NOENT, or PAL_ERR_INVAL.

import `form_set` · since 6

#### `pal_form_get`

```c
int32_t pal_form_get(int32_t id, void *buf, uint32_t len);
```

Copies the value of field id as text (NUL-terminated) into buf. Returns its length, PAL_ERR_NOENT, or PAL_ERR_INVAL (buf too small).

import `form_get` · since 6

#### `pal_form_clear`

```c
int32_t pal_form_clear(void);
```

Removes every field of the app's form (to build it again). Returns 0 or PAL_ERR_NOENT.

import `form_clear` · since 6

#### `pal_form_next`

```c
int32_t pal_form_next(void *rec, uint32_t len, int32_t timeout_ms);
```

Waits up to timeout_ms (-1 for ever) for the next event and copies it into rec ({int32 id, int32 kind, int32 value}, 12 bytes). Returns 1 with an event, 0 on timeout, PAL_ERR_NOENT (no form), or PAL_ERR_INVAL (rec under 12 bytes).

import `form_next` · since 6

#### `pal_form_close`

```c
int32_t pal_form_close(void);
```

Closes the app's form screen. Returns 0 or PAL_ERR_NOENT.

import `form_close` · since 6

#### `pal_canvas_open`

```c
int32_t pal_canvas_open(const char *title);
```

Opens the app's canvas screen with title and brings it to the front. Returns its size as (width << 16) | height in pixels, PAL_ERR_BUSY (the app has a form open, or every screen slot is taken), PAL_ERR_NOMEM, or PAL_ERR_UNSUPPORTED (no full-colour screen).

import `canvas_open` · since 6

#### `pal_canvas_present`

```c
int32_t pal_canvas_present(const void *px, uint32_t len, int32_t x, int32_t y, int32_t w);
```

Shows rows of w RGB565 pixels (2 bytes each, little endian) from px at x, y on the canvas; len is a whole number of rows. Returns 0, PAL_ERR_INVAL (outside the canvas, or len not whole rows), or PAL_ERR_NOENT (no canvas).

import `canvas_present` · since 6

#### `pal_canvas_key`

```c
int32_t pal_canvas_key(void *rec, uint32_t len, int32_t timeout_ms);
```

Waits up to timeout_ms (-1 for ever) for a key on the canvas and copies it into rec ({uint8 sym, uint8 mods, uint16 0, uint32 code point}, 8 bytes; sym as pal_key events). Returns 1 with a key, 0 on timeout, 2 when the person left the screen, PAL_ERR_NOENT, or PAL_ERR_INVAL.

import `canvas_key` · since 6

#### `pal_canvas_close`

```c
int32_t pal_canvas_close(void);
```

Closes the app's canvas screen. Returns 0 or PAL_ERR_NOENT.

import `canvas_close` · since 6

## Old names (deprecated)

Still available for old sources. Use the new name.

| Old name | New name |
|---|---|
| `pal_arg` | `pal_argv` |
| `pal_time_ms` | `pal_now_ms` |
| `pal_tty_raw` | `pal_tty_mode` |
| `pal_term_size` | `pal_tty_size` |

## Removed

None.

## SDK helpers (not imports)

`pal_strlen`, `pal_puts` and `pal_putu` are in `sdk/palanu.h` as `static inline` functions on top of `pal_write`.

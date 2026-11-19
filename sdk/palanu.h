/* PalanuOS app SDK. Freestanding C for wasm32: no libc, so everything the app can do is below.
 *
 *   clang --target=wasm32 -mcpu=mvp -mbulk-memory -O2 -ffreestanding -nostdlib -c app.c -o app.o
 *   rust-lld -flavor wasm --no-entry --export=_start app.o -o app.wasm
 *   palanu pack --id myapp --name "My App" --ver 0.1 app.wasm -o myapp.papp
 *
 * or just `bun forge/tools/palanu.ts build app.c --id myapp` from the repo root (same steps).
 * The module exports `_start`; its return ends the app. */
/* GENERATED from the api/palanu headers by forge/packages/apigen. Do not edit: change the definitions and run
 * `bun forge/packages/apigen/src/cli.ts`. Reference: docs/API.md. */
#ifndef PALANU_H
#define PALANU_H

#include <stdint.h>

#define PAL_API 7
#define PAL_IMPORT(n) __attribute__((import_module("palanu"), import_name(#n)))

typedef int32_t pal_handle_t;

/* ---- crypto: Cryptography, run by the system: the heavy work is native, on the chip's AES, SHA and ECC blocks where it has them, so an app passes buffers and handles. Handles index a table of the calling process: 8 digests (hash or HMAC) and 8 ciphers (AEAD or AES-CTR). The system frees them when the app ends. Every buffer comes with its length. The lengths of fixed-size values are checked: a key of 32 bytes is 32, not "up to 32". ---- */
#define PAL_HASH_SHA1 1
#define PAL_HASH_SHA256 2
#define PAL_HASH_SHA384 3
#define PAL_HASH_SHA512 4
#define PAL_AEAD_AES128_GCM 1
#define PAL_AEAD_AES256_GCM 2
#define PAL_AEAD_CHACHA20_POLY1305 3
#define PAL_ERR_UNSUPPORTED (-21)
#define PAL_ERR_AUTH (-23)

/* pal_random (since 1): Fills buf with random bytes from the chip's TRNG. Returns 0, or a negative error. */
PAL_IMPORT(random) int32_t pal_random(void *buf, uint32_t len);

/* pal_hash_open (since 1): Starts a hash with alg (PAL_HASH_*). Returns a handle, or a negative error (the table holds 8). */
PAL_IMPORT(hash_open) int32_t pal_hash_open(int32_t alg);

/* pal_hmac_open (since 1): Starts an HMAC with alg and key (any length). Feed it with pal_hash_update and finish with pal_hash_final. */
PAL_IMPORT(hmac_open) int32_t pal_hmac_open(int32_t alg, const void *key, uint32_t klen);

/* pal_hash_update (since 1): Adds n bytes to a hash or HMAC started with pal_hash_open or pal_hmac_open. Returns 0, or a negative error. */
PAL_IMPORT(hash_update) int32_t pal_hash_update(int32_t h, const void *d, uint32_t n);

/* pal_hash_final (since 1): Writes the digest to out (cap must hold it) and frees the handle. Returns the digest length, or a negative error. */
PAL_IMPORT(hash_final) int32_t pal_hash_final(int32_t h, void *out, uint32_t cap);

/* pal_aead_open (since 1): Starts an AEAD with alg and a key of 16 (AES-128) or 32 bytes (AES-256, ChaCha20). Returns a handle. */
PAL_IMPORT(aead_open) int32_t pal_aead_open(int32_t alg, const void *key, uint32_t klen);

/* pal_aead_seal (since 1): Encrypts buf in place and writes the 16-byte tag. The nonce is 12 bytes and is never reused with a key. */
PAL_IMPORT(aead_seal) int32_t pal_aead_seal(int32_t h, const void *nonce, uint32_t nlen, const void *aad, uint32_t alen, void *buf, uint32_t len, void *tag, uint32_t tlen);

/* pal_aead_decrypt (since 1): Checks the 16-byte tag and, only if it matches, decrypts buf in place. Returns 0, or PAL_ERR_AUTH with buf unchanged. */
PAL_IMPORT(aead_decrypt) int32_t pal_aead_decrypt(int32_t h, const void *nonce, uint32_t nlen, const void *aad, uint32_t alen, void *buf, uint32_t len, const void *tag, uint32_t tlen);

/* pal_aes_ctr_open (since 1): Starts AES-CTR with a 16 or 32 byte key and a 16-byte initial counter block. Returns a handle. */
PAL_IMPORT(aes_ctr_open) int32_t pal_aes_ctr_open(const void *key, uint32_t klen, const void *iv, uint32_t ivlen);

/* pal_cipher_xor (since 1): XORs buf with the next keystream bytes of a pal_aes_ctr_open handle. Calls continue the stream. */
PAL_IMPORT(cipher_xor) int32_t pal_cipher_xor(int32_t h, void *buf, uint32_t len);

/* pal_chacha20_xor (since 1): ChaCha20 keystream XOR: nlen 8 is the original 64-bit counter form (OpenSSH), 12 is RFC 8439 (32-bit counter). */
PAL_IMPORT(chacha20_xor) int32_t pal_chacha20_xor(const void *key, uint32_t klen, const void *nonce, uint32_t nlen, uint64_t counter, const void *in, uint32_t ilen, void *out, uint32_t olen);

/* pal_poly1305 (since 1): Poly1305 one-time authenticator: 32-byte key, 16-byte tag. The raw primitive, not the AEAD. */
PAL_IMPORT(poly1305) int32_t pal_poly1305(const void *key, uint32_t klen, const void *msg, uint32_t mlen, void *tag, uint32_t tlen);

/* pal_x25519 (since 1): X25519 shared secret of scalar and point (32 bytes each). Returns NEMA_ERR_INVAL for a low-order point. */
PAL_IMPORT(x25519) int32_t pal_x25519(void *out, uint32_t olen, const void *scalar, uint32_t slen, const void *point, uint32_t plen);

/* pal_x25519_base (since 1): X25519 public key of a 32-byte scalar (the base point). */
PAL_IMPORT(x25519_base) int32_t pal_x25519_base(void *out, uint32_t olen, const void *scalar, uint32_t slen);

/* pal_ed25519_verify (since 1): Verifies an Ed25519 signature (64 bytes) of msg with a 32-byte public key. Returns 0, or PAL_ERR_AUTH. */
PAL_IMPORT(ed25519_verify) int32_t pal_ed25519_verify(const void *pk, uint32_t pklen, const void *msg, uint32_t mlen, const void *sig, uint32_t slen);

/* pal_ecdsa_p256_verify (since 1): Verifies an ECDSA P-256 signature (r || s, 64 bytes) over a SHA-256 digest with a public key X || Y (64 bytes). */
PAL_IMPORT(ecdsa_p256_verify) int32_t pal_ecdsa_p256_verify(const void *pub, uint32_t plen, const void *hash, uint32_t hlen, const void *sig, uint32_t slen);

/* pal_rsa_verify_pkcs1 (since 1): Verifies an RSA PKCS#1 v1.5 signature. n and e are big-endian; halg is a PAL_HASH_* and hash its digest. */
PAL_IMPORT(rsa_verify_pkcs1) int32_t pal_rsa_verify_pkcs1(const void *n, uint32_t nlen, const void *e, uint32_t elen, int32_t halg, const void *hash, uint32_t hlen, const void *sig, uint32_t slen);

/* ---- dev: Board devices (API level 3, pins and sensors from 4, bus transfers from 5): the LEDs, buzzers, sensors, batteries and pins the board declares, found at run time so one app runs on a board with one LED and on a board with five. A device has an index (0 to pal_dev_total() - 1; stable while the board runs), an id such as "led0", a kind, and a name. Needs the dev permission in the manifest. The same devices are on the terminal: the dev command. ---- */

/* pal_dev_total (since 3, needs the "dev" permission): How many devices the board declares (a pin or bus the board did not open to apps still counts here but answers PAL_ERR_NOENT); 0 when it has none. */
PAL_IMPORT(dev_total) int32_t pal_dev_total(void);

/* pal_dev_info (since 3, needs the "dev" permission): Writes the text 'id kind name' of device index, separated by tabs, kind being led, buzzer, battery, sensor, gpio or bus, then for a led a tab, its pixel count, a tab and its colour count (3 RGB, 1 brightness only); for a sensor a tab and its unit; for a gpio a tab, its pin and rw or r; for a bus a tab and i2c, spi or uart; NUL-terminated, into out. Returns the length without the NUL, PAL_ERR_NOENT for an index out of range, or PAL_ERR_INVAL when out is too small. */
PAL_IMPORT(dev_info) int32_t pal_dev_info(int32_t index, void *out, uint32_t len);

/* pal_dev_lookup (since 3, needs the "dev" permission): The index of the device whose id is id (NUL-terminated), or PAL_ERR_NOENT. */
PAL_IMPORT(dev_lookup) int32_t pal_dev_lookup(const char *id);

/* pal_dev_set_led (since 3, needs the "dev" permission): Sets one pixel of LED device index to rgb (0xRRGGBB); pixel -1 sets every pixel of it. A brightness-only LED uses the brightest channel. Returns 0, PAL_ERR_NOENT (no such device), or PAL_ERR_INVAL (not an LED, or the pixel is out of range). */
PAL_IMPORT(dev_set_led) int32_t pal_dev_set_led(int32_t index, int32_t pixel, uint32_t rgb);

/* pal_dev_play_tone (since 3, needs the "dev" permission): Plays a tone of freq_hz (20 to 20000) for ms milliseconds (1 to 5000) on buzzer device index. Returns 0, PAL_ERR_NOENT (no such device), or PAL_ERR_INVAL (not a buzzer, or a value out of range). */
PAL_IMPORT(dev_play_tone) int32_t pal_dev_play_tone(int32_t index, int32_t freq_hz, int32_t ms);

/* pal_dev_gpio_read (since 4, needs the "dev" permission): Reads the level (0 or 1) of gpio device index. Returns the level, PAL_ERR_NOENT (no such device, or a pin the board did not open to apps), PAL_ERR_INVAL (not a readable pin) or PAL_ERR_IO. */
PAL_IMPORT(dev_gpio_read) int32_t pal_dev_gpio_read(int32_t index);

/* pal_dev_gpio_write (since 4, needs the "dev" permission): Drives gpio device index to level (0 or non-zero). Returns 0, PAL_ERR_NOENT, PAL_ERR_INVAL (not an output pin) or PAL_ERR_IO. */
PAL_IMPORT(dev_gpio_write) int32_t pal_dev_gpio_write(int32_t index, int32_t level);

/* pal_dev_sensor_read (since 4, needs the "dev" permission): Reads sensor device index into out (len must be at least 4): one little-endian int32, the value times 1000 in the unit pal_dev_info reports (21500 is 21.5). Returns 0, PAL_ERR_NOENT, PAL_ERR_INVAL (not a sensor) or PAL_ERR_IO. */
PAL_IMPORT(dev_sensor_read) int32_t pal_dev_sensor_read(int32_t index, void *out, uint32_t len);

/* pal_dev_read_battery (since 4, needs the "dev" permission): The charge of battery device index, 0 to 100, or -1 when the board cannot tell. Returns PAL_ERR_NOENT or PAL_ERR_INVAL (not a battery) otherwise. */
PAL_IMPORT(dev_read_battery) int32_t pal_dev_read_battery(int32_t index);

/* pal_dev_i2c_probe (since 4, needs the "dev" permission): Asks I2C bus device index whether a device answers at 7-bit address addr (3 to 119). Returns 1 when it does, 0 when not, PAL_ERR_NOENT, PAL_ERR_INVAL (not an I2C bus or a bad address) or PAL_ERR_IO. */
PAL_IMPORT(dev_i2c_probe) int32_t pal_dev_i2c_probe(int32_t index, int32_t addr);

/* pal_dev_bus_xfer (since 5, needs the "dev" permission): One transfer on bus device index (I2C, SPI or UART): writes tx (txn bytes, may be 0) then reads rxn bytes into rx (may be 0). addr is the I2C 7-bit address or the SPI chip-select index, and ignored on UART. Returns 0, PAL_ERR_NOENT, PAL_ERR_INVAL (not a bus, no transfer, or more than 256 bytes) or PAL_ERR_IO. */
PAL_IMPORT(dev_bus_xfer) int32_t pal_dev_bus_xfer(int32_t index, int32_t addr, const void *tx, uint32_t txn, void *rx, uint32_t rxn);

/* ---- events: Waiting and events. pal_wait waits on several handles at once: a file or pipe end, a terminal, or the process event queue from pal_events. Each record of the array is 8 bytes, little-endian: int32 h (the handle), uint16 want (the PAL_POLL_IN and PAL_POLL_OUT bits the app waits for) and uint16 got (what is ready, written by the call). At most 16 records. The event queue reports RESIZE (the terminal size changed) and QUIT (a stop was asked: Ctrl-C, TERM, HUP). Each event is 16 bytes, little-endian: uint32 type, a, b, c. Types KEY, FOCUS, BLUR, NOTIFY, SETTING and EXPOSE are reserved and not produced yet. A stop request ends a wait that does not include the queue, like a read; when the queue is in the wait it becomes ready instead, and reading QUIT takes the request. ---- */
#define PAL_POLL_IN 0x1
#define PAL_POLL_OUT 0x2
#define PAL_POLL_HUP 0x4
#define PAL_POLL_ERR 0x8
#define PAL_EV_NONE 0
#define PAL_EV_KEY 1
#define PAL_EV_RESIZE 2
#define PAL_EV_FOCUS 3
#define PAL_EV_BLUR 4
#define PAL_EV_QUIT 5
#define PAL_EV_NOTIFY 6
#define PAL_EV_SETTING 7
#define PAL_EV_EXPOSE 8

/* pal_wait (since 2): Waits until a record of waitfor (n_bytes / 8 records) is ready, or timeout_ms passes (-1 waits forever, 0 only checks). Writes got into each record. Returns the number of ready records, 0 on timeout, or a negative error (n_bytes not a multiple of 8, more than 16 records, or a handle the app does not hold). */
PAL_IMPORT(wait) int32_t pal_wait(void *waitfor, uint32_t n_bytes, int32_t timeout_ms);

/* pal_events (since 2): Returns the process event queue as a handle. It becomes readable when an event is pending; read the events with pal_event_next. Returns a negative error when the app has no free handle. */
PAL_IMPORT(events) int32_t pal_events(void);

/* pal_event_next (since 2): Reads the next event into ev (16 bytes; len must be at least 16). Returns 16 when an event was written, 0 when timeout_ms passed without one (-1 waits forever, 0 only checks), or a negative error. */
PAL_IMPORT(event_next) int32_t pal_event_next(void *ev, uint32_t len, int32_t timeout_ms);

/* ---- fs: Files, in the app's own namespace. An app sees /data (its persistent directory, kept across runs), /tmp (private to one run, removed when the app ends), /home (needs fs.home) and /sd (needs fs.media). Any other path is refused with PAL_ERR_PERM, and ".." cannot climb out of the namespace. A relative path starts in /data. Legacy manifests (API level 1) with the "fs" permission also get /home. A handle the app opens is closed by the system when the app ends, so an app that forgets close does not leak it. ---- */
#define PAL_O_READ 0x01
#define PAL_O_WRITE 0x02
#define PAL_O_CREATE 0x04
#define PAL_O_TRUNC 0x08
#define PAL_O_APPEND 0x10
#define PAL_ERR_NOMEM (-1)
#define PAL_ERR_INVAL (-2)
#define PAL_ERR_NOENT (-3)
#define PAL_ERR_BUSY (-4)
#define PAL_ERR_IO (-12)
#define PAL_ERR_RANGE (-13)
#define PAL_ERR_BADF (-14)
#define PAL_ERR_FULL (-15)
#define PAL_ERR_NOSPC (-17)
#define PAL_ERR_ISDIR (-18)
#define PAL_ERR_NOTDIR (-19)
#define PAL_ERR_EXIST (-11)
#define PAL_FILE 1
#define PAL_DIR 2
#define PAL_STAT_SIZE 16
#define PAL_DIRENT_SIZE 64
#define PAL_DIRENT_NAME 56
#define PAL_SEEK_SET 0
#define PAL_SEEK_CUR 1
#define PAL_SEEK_END 2

/* pal_open (since 1, needs the "fs" permission): Opens the file at path (len bytes, not NUL-terminated) with PAL_O_ flags. Returns a handle, or a negative error. */
PAL_IMPORT(open) int32_t pal_open(const char *path, uint32_t len, int32_t flags);

/* pal_close (since 1): Closes a handle that this app opened. Returns 0, or a negative error. */
PAL_IMPORT(close) int32_t pal_close(int32_t fd);

/* pal_stat (since 2, needs the "fs" permission): Writes the 16-byte record of path into st_out: uint32 size, uint32 mtime in seconds since boot, uint32 flags (PAL_FILE or PAL_DIR), uint32 reserved; little-endian. Returns 0, or a negative error. */
PAL_IMPORT(stat) int32_t pal_stat(const char *path, void *st_out, uint32_t len);

/* pal_dir_open (since 2, needs the "fs" permission): Opens the directory at path for pal_dir_next. Returns a handle, or a negative error. Close it with pal_close. */
PAL_IMPORT(dir_open) int32_t pal_dir_open(const char *path);

/* pal_dir_next (since 2, needs the "fs" permission): Writes the next entry of directory handle dh into ent_out: uint32 size, uint32 flags (PAL_FILE or PAL_DIR), then the NUL-terminated name in 56 bytes. Returns 1 for an entry, 0 at the end, or a negative error. */
PAL_IMPORT(dir_next) int32_t pal_dir_next(int32_t dh, void *ent_out, uint32_t len);

/* pal_seek (since 2, needs the "fs" permission): Moves the position of file handle h: whence is PAL_SEEK_SET, PAL_SEEK_CUR or PAL_SEEK_END. Returns the new position, or a negative error. */
PAL_IMPORT(seek) int64_t pal_seek(int32_t h, int64_t off, int32_t whence);

/* pal_mkdir (since 2, needs the "fs" permission): Creates the directory at path. The parent must exist. Returns 0, or a negative error. */
PAL_IMPORT(mkdir) int32_t pal_mkdir(const char *path);

/* pal_remove (since 2, needs the "fs" permission): Removes the file, or the empty directory, at path. Returns 0, or a negative error. */
PAL_IMPORT(remove) int32_t pal_remove(const char *path);

/* pal_rename (since 2, needs the "fs" permission): Renames (moves) from to to, both inside the app's namespace. Returns 0, or a negative error. */
PAL_IMPORT(rename) int32_t pal_rename(const char *from, const char *to);

/* pal_fsync (since 2, needs the "fs" permission): Flushes the file of handle h to its storage. Returns 0, or a negative error. */
PAL_IMPORT(fsync) int32_t pal_fsync(int32_t h);

/* ---- io: The standard streams. fd 0 is stdin, 1 is stdout, 2 is stderr; each may be a terminal, a pipe or a file. An app's stdio is its process handles, so pipes, redirects, Ctrl-C and ps work without code in the app. ---- */
#define PAL_ERR_AGAIN (-7)
#define PAL_ERR_INTR (-6)
#define PAL_ERR_PERM (-9)

/* pal_write (since 1): Writes len bytes from buf to fd. Returns the bytes written, or a negative error. */
PAL_IMPORT(write) int32_t pal_write(int32_t fd, const void *buf, uint32_t len);

/* pal_read (since 1): Reads up to len bytes from fd into buf. Returns the bytes read, 0 at end of input, or a negative error. Blocks; Ctrl-C stops the app. */
PAL_IMPORT(read) int32_t pal_read(int32_t fd, void *buf, uint32_t len);

/* ---- keys: Keystore: private keys are made, imported and used inside the system. An app gets a handle and the public key, and asks for signatures; the private bytes never enter the app's memory. Keys are stored encrypted under a device key in the system's data area (/data/keystore), which no app can open through the file API. Needs the "crypto.keys" permission in the manifest. The device key comes from a random secret made at first use: on the ESP that random source is only true random while the radio runs (Wi-Fi or BLE), so make the first key after the radio has started. ---- */
#define PAL_KEY_ED25519 1
#define PAL_KEY_ECDSA_P256 2

/* pal_key_generate (since 1, needs the "crypto.keys" permission): Makes a new key of type (PAL_KEY_*) under name, stores it, and returns a handle. Names are [A-Za-z0-9_-], 1 to 32. */
PAL_IMPORT(key_generate) int32_t pal_key_generate(int32_t type, const char *name, uint32_t nlen);

/* pal_key_load (since 1, needs the "crypto.keys" permission): Loads the stored key name into this app's key table and returns a handle. */
PAL_IMPORT(key_load) int32_t pal_key_load(const char *name, uint32_t nlen);

/* pal_key_import (since 1, needs the "crypto.keys" permission): Imports an unencrypted OpenSSH ed25519 key (-----BEGIN OPENSSH PRIVATE KEY-----) under name. Other forms: PAL_ERR_UNSUPPORTED. */
PAL_IMPORT(key_import) int32_t pal_key_import(const void *blob, uint32_t blen, const char *passphrase, uint32_t plen, const char *name, uint32_t nlen);

/* pal_key_public (since 1, needs the "crypto.keys" permission): Writes the public key as an SSH wire blob (ssh-ed25519 plus 32 bytes, or the ecdsa-sha2-nistp256 form). Returns its length. */
PAL_IMPORT(key_public) int32_t pal_key_public(int32_t h, void *out, uint32_t cap);

/* pal_key_sign (since 1, needs the "crypto.keys" permission): Signs data with the key: ed25519 gives 64 bytes; ecdsa signs its SHA-256 digest and gives r || s (64 bytes). Returns the length. */
PAL_IMPORT(key_sign) int32_t pal_key_sign(int32_t h, const void *data, uint32_t len, void *sig, uint32_t cap);

/* pal_key_list (since 1, needs the "crypto.keys" permission): Writes the names of the stored keys, one per line, into out. Returns the length. */
PAL_IMPORT(key_list) int32_t pal_key_list(void *out, uint32_t cap);

/* pal_key_delete (since 1, needs the "crypto.keys" permission): Deletes the stored key name. Handles already loaded from it keep working until the app ends. */
PAL_IMPORT(key_delete) int32_t pal_key_delete(const char *name, uint32_t nlen);

/* ---- kv: Per-app key-value store (API level 2). Each app keeps its own keys in /apps/<id>/kv.dat, outside its /data namespace, so the file API cannot reach them and no other app can read them. No capability is needed. Keys are 1 to 32 characters of [a-z0-9._-]; a value is up to 1024 bytes; an app holds at most 64 keys. Writes are durable: a power cut leaves the old value or the new one, and a half-written record is ignored. Native code and the built-in apps use the id "system". ---- */
#define PAL_KV_KEY_MAX 32
#define PAL_KV_VALUE_MAX 1024
#define PAL_KV_KEYS_MAX 64

/* pal_kv_get (since 2): Copies the value of key into buf, as many bytes as it has. Returns its length, PAL_ERR_NOENT for an unknown key, PAL_ERR_RANGE when cap is smaller than the value, or PAL_ERR_INVAL for a bad key. */
PAL_IMPORT(kv_get) int32_t pal_kv_get(const char *key, void *buf, uint32_t cap);

/* pal_kv_set (since 2): Stores len bytes of val under key, replacing an old value. Returns 0, PAL_ERR_INVAL (bad key, or len over 1024), or PAL_ERR_NOSPC when the app already holds 64 keys. */
PAL_IMPORT(kv_set) int32_t pal_kv_set(const char *key, const void *val, uint32_t len);

/* pal_kv_del (since 2): Deletes key. Returns 0, or PAL_ERR_NOENT when the key is not stored. */
PAL_IMPORT(kv_del) int32_t pal_kv_del(const char *key);

/* pal_kv_keys (since 2): Writes the stored keys into out, each followed by a newline. Returns the length, or PAL_ERR_RANGE when cap is too small. */
PAL_IMPORT(kv_keys) int32_t pal_kv_keys(void *out, uint32_t cap);

/* ---- legend: Key legends (API level 2): the bar at the bottom of the screen that says what the keys do. While the app's terminal is in front, the bar shows the legends the app published; the system's own apps fill the bar themselves. A legend names an action, not a key: the board prints its own key cap, so one text works on every board ("ok" is ENTER on hhv2). The text is items separated by ';', each NAME or NAME=label. NAME is one of ok, back, move, move_h, move_v, up, down, tab, menu, delete, home (any case), or a key cap in brackets for any other key: "ok=Jump;back=Quit;[SPACE]=Pause". An action the board has no key for is not shown, and items that do not fit the width are dropped from the right. The legends stay until the app changes them or ends. Showing or hiding the bar changes the terminal's rows: the app gets a RESIZE event. No permission is needed. ---- */

/* pal_legend_show (since 2): Shows the legends in spec (NUL-terminated, up to 159 bytes, at most 12 items) while the app's terminal is in front; an empty spec removes them. Returns 0, PAL_ERR_INVAL (malformed text, unknown name, cap over 15 characters), or a negative error when the app has no terminal. */
PAL_IMPORT(legend_show) int32_t pal_legend_show(const char *spec);

/* ---- log: Logging. Lines go to the system log (kept in memory, and printed on the console when that is on), tagged with the app's id, so the log shows which app wrote each line. ---- */
#define PAL_LOG_ERROR 0
#define PAL_LOG_WARN 1
#define PAL_LOG_INFO 2
#define PAL_LOG_DEBUG 3

/* pal_log (since 2): Writes msg (NUL-terminated) to the system log at a PAL_LOG_* level. Returns 0, or a negative error for an unknown level. */
PAL_IMPORT(log) int32_t pal_log(int32_t level, const char *msg);

/* ---- mem: Memory. A WebAssembly app allocates from its own linear memory, with the allocator its SDK or libc brings; there is no pal_alloc in this API. The system charges the linear memory to the app and caps it at the manifest's "mem" (KB, 256 when not given): memory.grow past the cap fails, so an app never holds more than its manifest says. Native code (the system and the built-in apps) uses the kernel allocator, which has classes and owners. ---- */

/* ---- net: TCP and DNS, as a client or a server (IPv4), TLS on a TCP connection, and HTTP and HTTPS requests (API level 7). A socket is an ordinary handle: pal_read, pal_write, pal_close, and pal_wait work on it, also after pal_tls_start. Connect, resolve, accept, the TLS handshake and a request take a timeout and stop at once when the app is asked to end (Ctrl-C, kill). An app's TLS always checks the server's certificate against the system's trusted roots. UDP is not in this level. ---- */
#define PAL_ERR_TIMEOUT (-5)
#define PAL_ERR_REFUSED (-26)
#define PAL_ERR_RESET (-24)
#define PAL_ERR_UNREACH (-25)
#define PAL_HTTP_HEAD_SIZE 8
#define PAL_NET_INFO_SIZE 12

/* pal_tcp_connect (since 2, needs the "net" permission): Connects to host (a name or a dotted IPv4 address, NUL-terminated) on port, waiting up to timeout_ms (negative: no limit) for the lookup and the connect. Returns a handle, or a negative error: PAL_ERR_REFUSED, PAL_ERR_UNREACH, PAL_ERR_TIMEOUT, PAL_ERR_NOENT. */
PAL_IMPORT(tcp_connect) int32_t pal_tcp_connect(const char *host, int32_t port, int32_t timeout_ms);

/* pal_tcp_listen (since 2, needs the "net.listen" permission): Listens on TCP port (all addresses) with the given backlog (1 to 32). Returns a listener handle, or a negative error. */
PAL_IMPORT(tcp_listen) int32_t pal_tcp_listen(int32_t port, int32_t backlog);

/* pal_tcp_accept (since 2, needs the "net.listen" permission): Waits up to timeout_ms (negative: no limit) for a connection on listener and returns a handle to it, or a negative error (PAL_ERR_TIMEOUT when none came). */
PAL_IMPORT(tcp_accept) int32_t pal_tcp_accept(int32_t listener, int32_t timeout_ms);

/* pal_dns_resolve (since 2, needs the "net" permission): Resolves host to an IPv4 address and writes its 4 bytes (a, b, c, d) into addr_out, which needs len >= 4. Waits up to timeout_ms. Returns 0, or a negative error. */
PAL_IMPORT(dns_resolve) int32_t pal_dns_resolve(const char *host, void *addr_out, uint32_t len, int32_t timeout_ms);

/* pal_net_info (since 2, needs the "net" permission): Writes the network state into out (12 bytes, see PAL_NET_INFO_SIZE): whether the link is up, its IPv4 address and the Wi-Fi signal. Returns 0, or a negative error. */
PAL_IMPORT(net_info) int32_t pal_net_info(void *out, uint32_t len);

/* pal_set_nonblock (since 2): on != 0: reads and writes on handle h never wait; with no data or no room they return PAL_ERR_AGAIN. on == 0: they wait again. Works on any handle the app owns. */
PAL_IMPORT(set_nonblock) int32_t pal_set_nonblock(int32_t h, int32_t on);

/* pal_shutdown (since 2): Shuts down a socket the app owns: how is 0 (receiving), 1 (sending) or 2 (both). Returns 0, or a negative error. */
PAL_IMPORT(shutdown) int32_t pal_shutdown(int32_t h, int32_t how);

/* pal_tls_start (since 7, needs the "net" permission): Turns the connected TCP socket h into a TLS client for host (the server name, checked against its certificate): the handshake runs now, up to timeout_ms. Afterwards pal_read and pal_write on h carry plain text. Returns 0, or a negative error: PAL_ERR_AUTH (certificate not trusted), PAL_ERR_UNSUPPORTED, PAL_ERR_TIMEOUT. */
PAL_IMPORT(tls_start) int32_t pal_tls_start(int32_t h, const char *host, int32_t timeout_ms);

/* pal_http_fetch (since 7, needs the "net" permission): Sends an HTTP or HTTPS request and waits up to timeout_ms for each step. method is GET, POST, PUT, DELETE, HEAD or PATCH; headers are extra lines of the form Name: value, separated by newlines (empty for none); body has body_len bytes (0 for none). Redirects are followed (up to 5). Writes the status, the body's full length and as much of the body as fits into out (see PAL_HTTP_HEAD_SIZE; cap >= 8). Returns the bytes written into out, or a negative error. */
PAL_IMPORT(http_fetch) int32_t pal_http_fetch(const char *method, const char *url, const char *headers, const void *body, uint32_t body_len, void *out, uint32_t cap, int32_t timeout_ms);

/* ---- notify: Notifications and toasts (API level 2). A notification has a title (up to 31 characters) and a text (up to 95); the system keeps the last 32 and the notification center shows them. A toast is a short message the system shows on top of the screen. Posting needs the notify permission. An app posts at most 4 notifications or toasts per second; the excess is refused with PAL_ERR_BUSY. Until the notification center exists, notifications are kept and logged, and shown by the notify shell command. ---- */
#define PAL_NOTIFY_ALERT 0x01

/* pal_toast (since 2, needs the "notify" permission): Shows text (NUL-terminated, up to 95 characters) as a toast for ms milliseconds (100 to 10000). Returns 0, PAL_ERR_INVAL, PAL_ERR_BUSY (rate limit), or PAL_ERR_FULL when the toast queue of 4 is full. */
PAL_IMPORT(toast) int32_t pal_toast(const char *text, int32_t ms);

/* pal_notify_post (since 2, needs the "notify" permission): Posts a notification with title and text (NUL-terminated) and flags (PAL_NOTIFY_*). Returns its id (above 0), PAL_ERR_INVAL, or PAL_ERR_BUSY (rate limit). */
PAL_IMPORT(notify_post) int32_t pal_notify_post(const char *title, const char *text, int32_t flags);

/* pal_notify_update (since 2, needs the "notify" permission): Changes the title and text of notification id, which this app posted. Returns 0, PAL_ERR_NOENT (no such id), PAL_ERR_PERM (another app's), or PAL_ERR_INVAL (too long). */
PAL_IMPORT(notify_update) int32_t pal_notify_update(int32_t id, const char *title, const char *text);

/* pal_notify_cancel (since 2, needs the "notify" permission): Removes notification id, which this app posted. Returns 0, PAL_ERR_NOENT (no such id), or PAL_ERR_PERM (another app's). */
PAL_IMPORT(notify_cancel) int32_t pal_notify_cancel(int32_t id);

/* ---- proc: The app itself and its child processes: how it ends, what it was started with, the environment, and the commands it starts. A command line is argv[0] (the command name), then the arguments the user typed. Children are processes like any other: they share the app's stdin, stdout and stderr, and join its process group, so Ctrl-C reaches them too. An app may only wait for or signal the children it started. A child it neither waited for nor detached is reaped by the system when the app ends. ---- */
#define PAL_SIG_INT 0x01
#define PAL_SIG_TERM 0x02
#define PAL_SIG_KILL 0x04
#define PAL_SIG_HUP 0x08
#define PAL_SPAWN_DETACH 0x01

/* pal_exit (since 1): Ends the app with the given status. The status is what the shell sees. */
PAL_IMPORT(exit) void pal_exit(int32_t code);

/* pal_argc (since 1): Number of command line words, including the command name. */
PAL_IMPORT(argc) int32_t pal_argc(void);

/* pal_argv (since 1): Copies word i (0 is the command name) into buf, NUL-terminated. Returns its length, or a negative error if it does not fit. */
PAL_IMPORT(arg) int32_t pal_argv(int32_t i, void *buf, uint32_t cap);
/* pal_arg: the older name of pal_argv, kept for old sources. */
__attribute__((deprecated("use pal_argv"))) static inline int32_t pal_arg(int32_t i, void *buf, uint32_t cap) { return pal_argv(i, buf, cap); }

/* pal_spawn (since 2, needs the "proc.spawn" permission): Starts the registered command app as a child. args holds the argument words after the command name, each ended by NUL (args_len bytes, at most 255; 0 for none). flags is 0 or PAL_SPAWN_DETACH. Returns a child handle (usable only by this app), or a negative error. */
PAL_IMPORT(spawn) int32_t pal_spawn(const char *app, const char *args, uint32_t args_len, int32_t flags);

/* pal_child_wait (since 2): Waits for a child this app started (timeout_ms as in pal_wait). When it has ended, writes its status into code_out (4 bytes, little-endian int32) and returns 1. Returns 0 on timeout, or a negative error (not one of this app's children, or the app was stopped). */
PAL_IMPORT(child_wait) int32_t pal_child_wait(int32_t child, void *code_out, uint32_t len, int32_t timeout_ms);

/* pal_signal (since 2): Sends a PAL_SIG_* signal to a child this app started. Returns 0, or a negative error (not one of this app's children, or unknown signal). */
PAL_IMPORT(signal) int32_t pal_signal(int32_t child, int32_t sig);

/* pal_getenv (since 2): Copies the value of the environment variable key into buf, NUL-terminated. Returns its length, or a negative error if the variable is not set or the value does not fit. Set: HOME (/home), USER (palanu), TERM (xterm-256color), PWD (the app's working directory). */
PAL_IMPORT(getenv) int32_t pal_getenv(const char *key, void *buf, uint32_t cap);

/* ---- settings: System settings (API level 2). A setting is a text key with a text value: ui.dark (0|1), ui.theme (index or name), ui.size (0 small, 1 medium), time.tz (minutes from UTC), display.brightness (0 to 100; the panel keeps at least 5), term.font (0 to 2), device.name, and the radio policy radio.wifi and radio.ble (read only: the wifi and ble commands change them). Reading needs the settings.read permission and changing a value needs settings.write. A change applies at once and is kept across restarts. Unknown keys give PAL_ERR_NOENT; a read-only key gives PAL_ERR_PERM. ---- */

/* pal_setting_get (since 2, needs the "settings.read" permission): Copies the value of key, as text, into buf with a NUL after it. Returns the length without the NUL, PAL_ERR_NOENT for an unknown key, or PAL_ERR_RANGE when cap cannot hold the value and its NUL. */
PAL_IMPORT(setting_get) int32_t pal_setting_get(const char *key, void *buf, uint32_t cap);

/* pal_setting_set (since 2, needs the "settings.write" permission): Sets key to the text value, checks it, applies it at once, and saves it. Returns 0, PAL_ERR_NOENT (unknown key), PAL_ERR_PERM (read-only key), or PAL_ERR_INVAL (value out of range or malformed). */
PAL_IMPORT(setting_set) int32_t pal_setting_set(const char *key, const char *value);

/* ---- svc: Services (API level 2). A service is a background app the supervisor starts (kind=service); it calls pal_svc_ready when it has finished starting, which makes its state RUNNING at once instead of after the start timeout. No permission is needed for this call. Starting, stopping, and reading other services is the svc shell command and, later, the svc.control permission. ---- */

/* pal_svc_ready (since 2): Tells the supervisor that this service has finished starting. Returns 0, or PAL_ERR_NOENT when the caller is not a service the supervisor started. */
PAL_IMPORT(svc_ready) int32_t pal_svc_ready(void);

/* ---- time: Time: the clock since boot (for measuring), sleeping, and the calendar time (for showing). Sleeping and the wait calls stop an app that Ctrl-C or kill has asked to end. The calendar clock has no battery: it starts at the firmware's build time until the user, SNTP or a connected Palanu Link client sets it. ---- */

/* pal_now_ms (since 1): Milliseconds since boot. */
PAL_IMPORT(time_ms) int64_t pal_now_ms(void);
/* pal_time_ms: the older name of pal_now_ms, kept for old sources. */
__attribute__((deprecated("use pal_now_ms"))) static inline int64_t pal_time_ms(void) { return pal_now_ms(); }

/* pal_sleep_ms (since 1): Sleeps for ms milliseconds, or until the app is interrupted. */
PAL_IMPORT(sleep_ms) void pal_sleep_ms(int32_t ms);

/* pal_unix_time (since 2): Seconds since 1970-01-01 00:00 UTC, from the calendar clock. */
PAL_IMPORT(unix_time) int64_t pal_unix_time(void);

/* pal_localtime (since 2): Writes the local time of unix_s (seconds since 1970 UTC) into tm_out, 36 bytes (len must be at least 36): nine little-endian int32 sec, min, hour, mday, mon (0 to 11), year (years since 1900), wday (0 is Sunday), yday (0 to 365), isdst (always 0). The local time is the system timezone. Returns 0, or a negative error (outside 1970 to 2106, or the buffer is too small). */
PAL_IMPORT(localtime) int32_t pal_localtime(int64_t unix_s, void *tm_out, uint32_t len);

/* ---- tty: The terminal the app talks to through stdin and stdout. The app's terminal is its controlling terminal: the first of stdin, stdout and stderr that is a terminal. A mode replaces the one before it: PAL_TTY_COOKED (0) lines are edited and echoed, and Ctrl-C stops the app. This is the default. PAL_TTY_RAW (1) every byte arrives as it is typed, Ctrl-C included (as byte 0x03; it does not stop the app). PAL_TTY_NOSIG (2) identical to RAW on this system: raw input and no signals from the keyboard. Kept for code written for terminals where raw and no-signal modes differ. PAL_TTY_NOECHO (4) cooked lines without echo, for typing passwords. When the app ends, the terminal goes back to the mode it had before the app first changed it, however the app ended. ---- */
#define PAL_TTY_COOKED 0
#define PAL_TTY_RAW 1
#define PAL_TTY_NOSIG 2
#define PAL_TTY_NOECHO 4
#define PAL_TERM_256COLOR 1
#define PAL_TERM_UTF8 2
#define PAL_TERM_ALTSCREEN 4

/* pal_tty_mode (since 1): Sets the terminal mode to a PAL_TTY_* value (any other bit is refused). Returns 0, or a negative error when stdin, stdout and stderr are not a terminal. */
PAL_IMPORT(tty_raw) int32_t pal_tty_mode(int32_t mode);
/* pal_tty_raw: the older name of pal_tty_mode, kept for old sources. */
__attribute__((deprecated("use pal_tty_mode"))) static inline int32_t pal_tty_raw(int32_t mode) { return pal_tty_mode(mode); }

/* pal_tty_get (since 2): Returns the current terminal mode as a PAL_TTY_* value: RAW for any raw state, NOECHO for cooked without echo. Returns a negative error when there is no terminal. */
PAL_IMPORT(tty_get) int32_t pal_tty_get(void);

/* pal_tty_size (since 1): Writes the size of the controlling terminal into buf as 4 bytes: columns, then rows, each a little-endian uint16. Needs len >= 4. 80x24 when no terminal reports a size. */
PAL_IMPORT(term_size) int32_t pal_tty_size(void *buf, uint32_t len);
/* pal_term_size: the older name of pal_tty_size, kept for old sources. */
__attribute__((deprecated("use pal_tty_size"))) static inline int32_t pal_term_size(void *buf, uint32_t len) { return pal_tty_size(buf, len); }

/* pal_tty_caps (since 2): Returns the terminal capability bits: PAL_TERM_256COLOR (1), PAL_TERM_UTF8 (2), PAL_TERM_ALTSCREEN (4). The on-device terminal has all three. */
PAL_IMPORT(tty_caps) int32_t pal_tty_caps(void);

/* ---- ui: App screens (API level 6, ADR-0010 §8). An app that is not a terminal app shows one screen of one of two kinds: A form is data: a list of fields (sections, information, switches, choices, sliders, actions, editable text), each with an id the app picks. The running desktop session lays it out in its own look, so the same app looks native under every desktop. The app waits for events (a field changed, an action ran, the screen closed) with pal_form_next. A canvas is pixels: an RGB565 area the size the system gives (below the session's bar), which looks the same under every desktop. The app sends rows of pixels with pal_canvas_present and reads keys with pal_canvas_key. An app has at most one screen at a time; opening one brings it to the front. The screen goes when the app closes it or ends. The records are little endian. No permission is needed. ---- */
#define PAL_FORM_SECTION 1
#define PAL_FORM_INFO 2
#define PAL_FORM_TOGGLE 3
#define PAL_FORM_CHOICE 4
#define PAL_FORM_SLIDER 5
#define PAL_FORM_ACTION 6
#define PAL_FORM_TEXT 7
#define PAL_SYM_CHAR 1
#define PAL_SYM_ENTER 2
#define PAL_SYM_ESC 3
#define PAL_SYM_BACKSPACE 4
#define PAL_SYM_TAB 5
#define PAL_SYM_DELETE 6
#define PAL_SYM_UP 7
#define PAL_SYM_DOWN 8
#define PAL_SYM_LEFT 9
#define PAL_SYM_RIGHT 10
#define PAL_FORM_CHANGED 1
#define PAL_FORM_ACTIVATED 2
#define PAL_FORM_CLOSED 3

/* pal_form_open (since 6): Opens the app's form screen with title (up to 31 bytes) and brings it to the front, with no fields. Returns 0, PAL_ERR_BUSY (the app has a canvas open, or every screen slot is taken), or PAL_ERR_UNSUPPORTED when there is no screen. */
PAL_IMPORT(form_open) int32_t pal_form_open(const char *title);

/* pal_form_add (since 6): Adds a field at the end of the form: id (0 to 65535, unique in the form), kind (PAL_FORM_*), label (up to 31 bytes) and value (its text, as each kind says, up to 95 bytes). Returns 0, PAL_ERR_INVAL, PAL_ERR_EXIST (the id is taken), PAL_ERR_FULL (32 fields), or PAL_ERR_NOENT (no form open). */
PAL_IMPORT(form_add) int32_t pal_form_add(int32_t id, int32_t kind, const char *label, const char *value);

/* pal_form_set (since 6): Sets the value of field id as its kind reads it: the text of an info or text field, 0 or 1 for a toggle, the index for a choice, the number for a slider. Returns 0, PAL_ERR_NOENT, or PAL_ERR_INVAL. */
PAL_IMPORT(form_set) int32_t pal_form_set(int32_t id, const char *value);

/* pal_form_get (since 6): Copies the value of field id as text (NUL-terminated) into buf. Returns its length, PAL_ERR_NOENT, or PAL_ERR_INVAL (buf too small). */
PAL_IMPORT(form_get) int32_t pal_form_get(int32_t id, void *buf, uint32_t len);

/* pal_form_clear (since 6): Removes every field of the app's form (to build it again). Returns 0 or PAL_ERR_NOENT. */
PAL_IMPORT(form_clear) int32_t pal_form_clear(void);

/* pal_form_next (since 6): Waits up to timeout_ms (-1 for ever) for the next event and copies it into rec ({int32 id, int32 kind, int32 value}, 12 bytes). Returns 1 with an event, 0 on timeout, PAL_ERR_NOENT (no form), or PAL_ERR_INVAL (rec under 12 bytes). */
PAL_IMPORT(form_next) int32_t pal_form_next(void *rec, uint32_t len, int32_t timeout_ms);

/* pal_form_close (since 6): Closes the app's form screen. Returns 0 or PAL_ERR_NOENT. */
PAL_IMPORT(form_close) int32_t pal_form_close(void);

/* pal_canvas_open (since 6): Opens the app's canvas screen with title and brings it to the front. Returns its size as (width << 16) | height in pixels, PAL_ERR_BUSY (the app has a form open, or every screen slot is taken), PAL_ERR_NOMEM, or PAL_ERR_UNSUPPORTED (no full-colour screen). */
PAL_IMPORT(canvas_open) int32_t pal_canvas_open(const char *title);

/* pal_canvas_present (since 6): Shows rows of w RGB565 pixels (2 bytes each, little endian) from px at x, y on the canvas; len is a whole number of rows. Returns 0, PAL_ERR_INVAL (outside the canvas, or len not whole rows), or PAL_ERR_NOENT (no canvas). */
PAL_IMPORT(canvas_present) int32_t pal_canvas_present(const void *px, uint32_t len, int32_t x, int32_t y, int32_t w);

/* pal_canvas_key (since 6): Waits up to timeout_ms (-1 for ever) for a key on the canvas and copies it into rec ({uint8 sym, uint8 mods, uint16 0, uint32 code point}, 8 bytes; sym as pal_key events). Returns 1 with a key, 0 on timeout, 2 when the person left the screen, PAL_ERR_NOENT, or PAL_ERR_INVAL. */
PAL_IMPORT(canvas_key) int32_t pal_canvas_key(void *rec, uint32_t len, int32_t timeout_ms);

/* pal_canvas_close (since 6): Closes the app's canvas screen. Returns 0 or PAL_ERR_NOENT. */
PAL_IMPORT(canvas_close) int32_t pal_canvas_close(void);

/* Helpers (not imports): text output on top of pal_write. */
static inline unsigned pal_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static inline int32_t pal_puts(const char *s) { return pal_write(1, s, pal_strlen(s)); }
static inline void pal_putu(unsigned long long v) {
    char b[24];
    int i = 24;
    do b[--i] = (char)('0' + v % 10), v /= 10;
    while (v);
    pal_write(1, b + i, (unsigned)(24 - i));
}

#endif

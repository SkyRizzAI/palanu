# `.papp` apps for PalanuOS (API version 1)

An installed app is a single file `/apps/<id>.papp` that contains a WebAssembly module. The decisions and their reasons are in
`docs/adr/0003-*.md`; this file is for people who write apps.

## Making an app

```bash
# from C (libc-free, wasm32): needs clang and wasm-ld or rust-lld
bun forge/tools/palanu.ts build sdk/examples/hello.c --id hello --name "Hello"
# or from a wasm module made with Rust/Zig/etc. that exports `_start`
bun forge/tools/palanu.ts pack app.wasm --id myapp --perm fs --mem 128
# send to the board (UART or TCP) and run
bun forge/tools/palanu.ts --serial /dev/cu.usbserial-10 install hello.papp
```

In the board's shell, type `hello` (command name = `id`). `app list` shows installed apps, `app info FILE` the manifest contents,
`app remove ID` deletes the file and its command together (refused if the app is running). A file written to `/apps` by any means
(Palanu Link, `cp`, `app install`) is registered immediately if it is valid.

The full developer guide (`palanu new`, `app.toml`, `palanu build` in a project folder, `deploy --run`): `docs/DEVELOPER.md`.

## Manifest options

| Option | Meaning | Default |
|---|---|---|
| `--id` | command name, `[a-z0-9_-]`, max 23. Must not equal a built-in command | required |
| `--perm fs,net` | requested permissions. `fs`: `/data` (persistent) and `/tmp` (private, deleted when the app finishes). `fs.home`: `/home`. `fs.media`: `/sd`. `net`: TCP client and DNS. `net.listen`: listen and accept. Unknown permissions are rejected. An API level 1 manifest with `fs` still gets `/home` (compatibility). See `docs/API.md` | no permissions |
| `--mem KB` | linear memory limit: at start and `memory.grow` (multiples of 64 KB) | 256 |
| `--stack KB` | wasm stack | 16 |
| `--fuel M` | stop the app after M million instructions (the only stopper for a tight loop) | no limit |

## What an app can do

Everything goes through the `palanu` import module. The full list is in `docs/API.md`, generated from `api/palanu/*.h` (the SDK header `sdk/palanu.h` is generated from the same source): `write`/`read` on fd 0-2
and files it opens itself, `exit`, `time_ms`, `sleep_ms`, `argc`/`arg`, `tty_raw`, `term_size`, `open`/`close`.
An app runs as an ordinary Nema process: its stdio is process handles, so pipes (`echo a | count`), redirection,
Ctrl-C, and `ps` work without special code.

## File format (version 2)

Format 2 is written by `forge/packages/papp` and read by the firmware. Format 1 (a single payload, no compression) is still read.
All numbers are little-endian.

```
offset  size    content
0       4       "PLNU"
4       1       fmt = 2
5       1       flags: bit1 = signed (an SIG0 trailer is present); other bits must be 0
6       2       hdr_len = 10 + 14 * nsect
8       2       nsect (2 to 8)
10      14*n    section table: type u8, flags u8 (bit0 = raw DEFLATE), off u32, stored u32, raw u32
...     ...     sections, at the offsets the table points to
...     32      SHA-256 over all previous bytes
...     68      (only if bit1) "SIG0" + 64 bytes Ed25519 over the hash above
```

| type | content | required | `raw` limit |
|---|---|---|---|
| 1 MANIFEST | a CBOR map (see the options above) | yes, exactly once | 4 KB |
| 2 CODE | WebAssembly module | yes, exactly once | 1 MB |
| 3 ICON | icon (read by the launcher later) | no | 16 KB |
| 4 RES | resources | no | 256 KB |
| 5 AOT | reserved: rejected if present | - | - |

Other types are rejected. Every section is checked against the file size before it is read; the hash is checked before the manifest is trusted.
Compressed sections are inflated (raw DEFLATE, `third_party/miniz` tinfl) into memory owned by the installing process or the caller. The resulting
size must be exactly `raw`.

Format 1 (`fmt = 1`): a 20-byte header `PLNU, 1, flags, 20, manifest length, payload length, payload length`, manifest,
payload, SHA-256. No compression and no signature.

## Trust and installation

A valid package is one whose hash matches and which is (1) signed by a key present in `/etc/trust/*.pub` (32 raw bytes,
or 64 hex digits), or (2) developer mode is on (`dev.mode=1` in `/data/settings`).

| signature | `dev.mode=0` | `dev.mode=1` |
|---|---|---|
| trusted | accepted | accepted |
| missing, or invalid | rejected | accepted, with a warning |

A package that requests `net`, `net.listen`, or `crypto.keys` is treated the same; the rejection names that permission. The policy
is checked again every time the app runs.

Installation (`app install FILE`, or `pal_papp_install_msg`): read, parse, check the policy, write to `/apps/.staging/<id>.papp`,
read back and check again, move to `/apps/<id>/app.papp`, register. Every failure deletes what was created and
states the cause. A name equal to a built-in command is rejected; an app that is already installed must be removed first.
`app remove ID` deletes the command, the registry entry, and the whole folder `/apps/<id>/` (including `data/`, `kv.dat`, `kv.tmp`).

A file written directly to `/apps/<id>.papp` (Link, `cp`) is also checked with the same policy when it is registered.
`/apps/.staging` is ignored by the scanner.

Keys are created and signed on the computer:

```bash
bun forge/tools/palanu.ts keygen dev.key                      # writes the seed (hex) and prints the public key
bun forge/tools/palanu.ts pack app.wasm --id demo --perm net --sign dev.key -o demo.papp
```

The public key is placed on the device as `/etc/trust/<name>.pub`.

## Permissions at first use (ADR-0014)

A manifest permission is what an app may ask for; the user decides. `net`, `net.listen`, `fs.home`, `fs.media`, `proc.spawn`,
`crypto.keys` and `settings.write` are asked the first time the app uses them ("Allow Chat? Chat wants to use the network."), and
the answer is kept per app in `/data/perms`. A denied permission makes the call fail with `PAL_ERR_PERM`, as if the manifest did
not have it, and is not asked again. Settings > Apps > the app shows each one as Ask, Allow or Don't allow, and Reset permissions
asks again at first use. The other permissions (`fs`, `notify`, `dev`, `settings.read`, `svc.control`, `link`) come with the
manifest. Where nobody can be asked (no display, no desktop running), the manifest decides as before and nothing is kept.

## Proven on the chip (ESP32-S31)
`hello` (arguments, time), `count` in a pipe, `note` (`fs` permission, stored in LittleFS), `spin` (watchdog). WAMR memory
is in PSRAM (peak 18.7 KB for the example app); one app uses 12 KB of process stack.

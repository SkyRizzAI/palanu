# Developer guide: building apps for PalanuOS

This guide is for people who write apps, not people who change the system. It goes from nothing to an app running on a board:
install the tools, create a project, build, send, and run. The full API reference is in `docs/API.md` (generated from
`api/palanu/*.h`), the terminal toolkit is in `docs/TUI.md`, and the file format is in `docs/PAPP.md`.

## 1. Environment

| Tool | What it is for | How to install |
|---|---|---|
| bun | runs `palanu` and `./pal` | `brew install oven-sh/bun/bun` or from bun.sh |
| clang with the wasm32 target | compiles C to wasm | Apple clang from the Xcode Command Line Tools is enough (tested with Apple clang 21) |
| wasm-ld or rust-lld | links wasm modules | `brew install llvm` (wasm-ld is in `$(brew --prefix llvm)/bin`), or rust-lld from rustup |

`palanu build` looks for wasm-ld in PATH first, then for rust-lld in the rustup toolchain. If neither exists, the error message
names those two options. Check the environment:

```bash
bun --version && clang --version | head -1 && (which wasm-ld || ls ~/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld)
```

No extra downloads are needed. A wasi-libc mode (wasi-sdk) does not exist yet; this SDK runs with its own small C library.

## 2. Creating an app

```bash
bun forge/tools/palanu.ts new hello --kind tty      # or: alias palanu="bun /path/to/palanuos/forge/tools/palanu.ts"
cd hello
```

The result is a `hello/` folder with three files:

- `app.toml`: the manifest (see section 4).
- `main.c`: the app code. The program entry is `_start`; when `_start` returns, the app is finished.
- `README.md`: the build and deploy steps for that app.

App kinds (`--kind`):

| Kind | Template content | Notes |
|---|---|---|
| `tty` | a paltui menu: Up/Down, Enter, Esc | runs in the terminal (UART, Link, SSH). See `docs/TUI.md` |
| `gui` | a stub that prints a message | desktop apps need the frame API (task T5.2), which is not in the SDK yet |
| `service` | a loop that logs every second and finishes when asked to stop | uses `pal_events`, `pal_wait`, `pal_log`. Ctrl-C or `kill` sends a QUIT event |

Examples in `sdk/examples/`: `hello-tty` (menu), `net-client` (TCP client), `hello-service` (service), and the older examples
`hello.c`, `count.c`, `note.c`, `menu.c`, `spin.c`, which are built with the flag form.

## 3. Building

From the project folder (where `app.toml` is):

```bash
bun /path/to/palanuos/forge/tools/palanu.ts build          # writes hello.papp in that folder
bun /path/to/palanuos/forge/tools/palanu.ts build -o /tmp/hello.papp
```

The older flag form still works, and is useful for a single C file without a project:

```bash
bun forge/tools/palanu.ts build sdk/examples/hello.c --id hello --name "Hello" --perm fs --mem 128 -o hello.papp
```

The build command runs: `clang --target=wasm32 -mcpu=mvp -mbulk-memory -O2 -ffreestanding -nostdlib -Wall -I sdk -c`,
then wasm-ld with `--no-entry --export=_start --strip-all`, then wraps the module into a format 2 `.papp` through `packPapp` (`forge/packages/papp`), with a SHA-256 at the end.

Output sizes measured on a Mac (`palanu build`, format 2 package with per-section deflate): `hello` 444 bytes, `count` 503,
`note` 465, `spin` 206, `menu` 10,110, `hello-tty` 8,331, `net-client` 2,690, `hello-service` 1,997. The sizes of `hello-tty`
and `menu` include paltui; the libc library only adds what is used.

## 4. app.toml

```toml
id = "hello-tty"          # command name in the shell: a-z 0-9 _ -, at most 23 characters
name = "Hello TTY"
version = "0.1.0"
api = 2                   # API level used (currently 2)
kind = "tty"              # tty | gui | service
sources = ["main.c"]      # .c files in the project folder
caps = []                 # permissions, see section 6
stack_kb = 16             # wasm stack
mem_kb = 256              # linear memory limit
fuel_m = 0                # 0 = no instruction limit; M = stop after M million instructions
icon = "icon.pvi"         # placeholder, not packaged yet
```

The parser uses a subset of TOML: strings (`"..."` with escapes or `'...'`), integers (including `0x` and underscores),
`true`/`false`, and string arrays that may span multiple lines. `[service]` tables and similar are accepted but not used
yet. Unknown top-level keys produce a warning. Errors name the line number.

Note: `palanu build` writes a format 2 package (section table, manifest can be compressed) through `forge/packages/papp`,
and `kind` is part of the manifest. Devices with the format 1 installer (HEAD) do not run format 2 packages yet; see
troubleshooting.

## 5. Sending and running

```bash
palanu deploy hello-tty.papp --tcp 192.168.1.20:7100               # install only
palanu deploy hello-tty.papp --run --tcp 192.168.1.20:7100         # install, run, show its output
palanu deploy hello-tty.papp --run --serial /dev/cu.usbserial-10   # over UART
palanu deploy net-client.papp --run --args '192.168.1.10 9000 ping' --tcp 192.168.1.20:7100
```

- `deploy` reads the `id` from the `.papp` manifest (the file name does not determine the command name), then writes to
  `/apps/<id>.papp`. Without `--run`, type the app name in the board's shell.
- `--args 'ARGS'` builds the command line `<id> ARGS`, so quoting follows the rules of the board's shell.
- With `--run`, palanu opens a Link shell, runs the app, and shows its output until the app finishes.
  In a terminal, the keys you press are forwarded to the app. Ctrl-C is forwarded to the app as Ctrl-C.
- The app's exit code is printed to stderr as `[hello-tty exited with code N]`, and `palanu` exits with the same code.
  A command that is not found exits with 127 (`sh: ...: command not found`).
- The address can also be given through `PALANU_TARGET=tcp:host:port` or `--serial` without `--tcp`.
- Simulator on a Mac: `./pal sim --link 7100 --wait 120000`, then `--tcp 127.0.0.1:7100`. TCP is closed until paired: run `palanu pair --tcp 127.0.0.1:7100` and enter the code from `link pair` in the simulator shell; or use `--link-open` (for tests only).

## 6. Permissions

`caps` lists the permissions requested. A permission that is not requested is denied by the system: an API call that needs it fails
(for file access, error `PAL_ERR_PERM` (-9) from `docs/API.md`).

| Permission | Grants access to |
|---|---|
| `fs` | `/data` (persistent) and `/tmp` (private, deleted when the app finishes) |
| `fs.home` | `/home` |
| `fs.media` | `/sd` |
| `net` | TCP client and DNS (`pal_tcp_connect`, `pal_dns_resolve`) |
| `net.listen` | listen and accept |
| `proc.spawn`, `proc` | running child processes (see docs/API.md) |
| `dev` | board devices: LEDs, buzzers, sensors, battery, pins, buses (`pal_dev_*`, API levels 3 and 4; see section 6a) |

An unknown permission makes creating the `.papp` fail.

### 6a. Board devices (`dev` permission, API levels 3 and 4)

A board declares its devices as a list (`docs/adr/0005-board-device-list.md`): one entry per physical LED or buzzer.
An app does not guess the board; it asks:

```c
int total = pal_dev_total();                 // how many devices
pal_dev_info(i, buf, sizeof buf);            // "id<TAB>kind<TAB>name", an LED adds "<TAB>pixels<TAB>color"
pal_dev_set_led(i, -1, 0xff0000);            // all pixels of LED i red; pixels 0..n-1 for a single pixel
pal_dev_play_tone(i, 880, 150);              // an 880 Hz tone for 150 ms on buzzer i
int led = pal_dev_lookup("led2");            // index from id
```

A board with one LED and a board with five LEDs run the same app; see `sdk/examples/devices` for an example. The same devices can be
used from a terminal without an app, over UART, SSH, or `palanu run`:

```
dev                              # list board devices
dev led led2 red                 # color name, #rrggbb, 0xrrggbb, or R G B
dev led strip 3 0 0 255          # a single pixel
dev beep buzzer0 880 150
palanu --serial /dev/cu.usbserial-10 run 'dev led led0 green'     # from a computer, without touching the board
```

Sensors, battery, pins, and buses (API level 4, `docs/adr/0006-device-table-all-kinds.md`) are in the same table. Pins and buses only appear
to an app if the board marks them `app`; otherwise their index answers `PAL_ERR_NOENT`:

```c
int32_t milli; pal_dev_sensor_read(i, &milli, 4);   // value x1000 in the unit from pal_dev_info (21500 = 21.5)
pal_dev_gpio_write(i, 1);  int lv = pal_dev_gpio_read(i);
int pct = pal_dev_read_battery(i);                  // -1 if the board does not know
int present = pal_dev_i2c_probe(i, 0x48);           // 1 answers, 0 silent
```

Terminal: `dev sensor [ID]`, `dev gpio ID [0|1]`, `dev battery`, `dev i2c ID scan`.

In the simulator: `./pal sim --leds 5 --led-log` gives five fake LEDs and prints the color on every change, so the Settings pages and apps
can be tried without a board; `--sensors N`, `--gpios N`, and `--i2c` add fake sensors, pins, and a bus.

## 7. Memory and budgets

- `mem_kb` is the app's linear memory limit. `malloc` in `palanu_libc.h` returns `NULL` if the runtime refuses to grow
  memory. Check every `malloc` result.
- `stack_kb` is the wasm stack. Avoid large buffers on the stack (for example a 4 KB buffer in a recursive function).
- The paltui `tui_t` struct is about 32 KB (a 100x40 cell screen, two buffers). Declare it `static`, as the template does.
- Baseline: an installed app uses 150 to 350 KB in total in PSRAM (processed code is about 3.5 times the size of the
  `.wasm`, plus stack and linear memory). Start from 128 or 256 KB and raise it if needed.
- For long-running apps, free memory that is no longer used. The palanu_libc.h heap merges free blocks,
  but does not return pages to the runtime.

## 8. C library: palanu.h and palanu_libc.h

- `palanu.h` (generated from `api/palanu/`): all system APIs. Names are `pal_*`.
- `palanu_libc.h`: strings (`strlen`, `strcmp`, `strncpy`, `strstr`, `strtol`, and others), ctype, the `malloc` family,
  `snprintf` and `printf` (no float), `puts`, `putchar`, `qsort`, `bsearch`, `rand`, `exit`.
  One `.c` file must define `PALANU_LIBC_IMPLEMENTATION` before the include:

```c
#define PALANU_LIBC_IMPLEMENTATION
#include "palanu_libc.h"
```

- Other files just `#include "palanu_libc.h"`.
- Unsupported formats: `%f` (printed as is), `%*d`, floating-point numbers. Use integers.

## 9. Troubleshooting

The status of each error: **tried on a Mac** (no device), **tried in the simulator** (HEAD, port 7100), or **not tested**
(needs a board or an event that has not been reproduced).

**Tried on a Mac.** `undefined symbol: printf` (or `malloc`, `strlen`) during build. No file defines
`PALANU_LIBC_IMPLEMENTATION`. Add that define in one `.c` (see section 8). The palanu message mentions this too.

**Not tested** (the developer Mac only has rust-lld; the wasm-ld path has not been tried). `no wasm linker: install LLVM (wasm-ld) or rustup (rust-lld)`. Install with `brew install llvm`, or `rustup` with a
stable toolchain. Make sure `which wasm-ld` points to an existing file.

**Tried on a Mac and in the simulator.** Forgetting `-mcpu=mvp` (when calling clang directly, not through `palanu build`). Without
that flag, Apple clang 21 includes extra wasm features in the object: `sign-ext`, `mutable-globals`,
`nontrapping-fptoint`. With `-mcpu=mvp -mbulk-memory` only `bulk-memory` remains. In the simulator (WAMR from
HEAD), a module without `-mcpu=mvp` still runs (`hello from WebAssembly on PalanuOS`). But do not rely on it:
devices and later runtimes may reject those features. Always use `palanu build`, which already includes the right
flags.

**`permission denied` or `PAL_ERR_PERM`. Status: tried in the simulator.** The app calls an API whose permission was not
requested. Add the permission to `caps` in app.toml, then rebuild and redeploy. Tested example: `note` writes to
`/home/notes.txt`, and `/home` is only open with `fs.home`. Without that permission, `note hello` prints
"cannot open the notes file" and exits with code 1. With `--perm fs.home` writing and `note` (read) work.

**`instruction limit exceeded`. Status: tried in the simulator.** The output is `spin: stopped: Exception: instruction limit
exceeded` and exit code 139 (the code the shell reports for a stopped app; its origin has not been confirmed). The app
passed `fuel_m`: too many instructions without an API call. Raise `fuel_m`, or fix the loop that never
stops. Test example: `spin` with `--fuel 20`.

**Tried in the HEAD simulator.** A format 2 package is stored in `/apps/<id>.papp`, but the `<id>` command is not registered:
`sh: hello: command not found` and code 127. The installer at HEAD only understands format 1. In the working tree, registration of
format 2 is in progress (T2.9 and T2.10), so run the app in the simulator after that is finished.

**Tried on a Mac.** `no app.toml; run palanu build inside a project made by palanu new`. Run `palanu build` from the project folder
(the one that contains app.toml), or use the flag form with `--id`.

**Not tested.** `the shell closed before the app finished` on `deploy --run`. The Link dropped, or the app killed the shell.
Try `palanu --tcp ... info` to confirm the board responds, then repeat.

**Tried in the simulator.** `deploy` waits until the app finishes. Ctrl-C in the terminal (or SIGINT to palanu) is sent to the app
as Ctrl-C, and `hello-service` ends its loop with code 0. In a paltui menu, Esc closes the app.

## 10. Going further

- API reference: `docs/API.md` (generated from `api/palanu/*.h`, do not edit `sdk/palanu.h` by hand).
- Terminal toolkit: `docs/TUI.md`.
- `.papp` format and manifest: `docs/PAPP.md`.
- Example code: `sdk/examples/`.

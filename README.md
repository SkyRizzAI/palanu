# PalanuOS

**A small, board-agnostic operating system for embedded devices — written in C, built for ESP32, programmable in WebAssembly.**

PalanuOS gives a microcontroller what you expect from a real OS: a kernel with per-process memory budgets, a graphical
launcher and settings, a terminal with SSH, installable apps, over-the-air updates with rollback, and remote control from
your computer or your browser. It runs in 320x240 screens, a few hundred kilobytes of RAM, and one board file.

The first supported board is **hhv2** (ESP32-S31, 320x240 LCD, 39-key keyboard). Porting another board means describing its
hardware, not rewriting the system.

## Why PalanuOS

- **Board-agnostic by design.** A board is one descriptor: screen, keyboard, and a table of devices (LEDs, buzzers, batteries,
  storage, sensors, GPIO pins, I2C/SPI/UART buses, radios). The system never contains a board name or a pin number. Five LEDs?
  Settings shows five. A second temperature sensor? It appears in Settings, the shell and the app API with no new code.
- **Apps in WebAssembly.** Write an app once in C (or anything that compiles to Wasm), package it as a `.papp`, and install it
  over the air, over USB, or over Wi-Fi. Apps run sandboxed with manifest permissions and a versioned, additive API.
  Hardware is reachable only through the API, and only the pins a board marks as app-safe.
- **Safe by construction.** Per-process memory quotas, an admission check against free memory, a service supervisor with
  restart policy and safe mode, OTA with a health check and rollback, trust store for signed packages, developer mode off in
  release builds.
- **Remote by default.** Palanu Link (UART, TCP, BLE, WebSocket) drives a shell, screen mirror, keys, file transfer and OTA from
  the `palanu` command line or from **Web Forge**, a no-server browser app that works over Web Serial and Bluetooth.
- **A real terminal.** Box drawing, braille spinners, wide characters, three font sizes, and an SSH client with key and
  password login, so modern text UIs run on a handheld.
- **Develop without hardware.** A native simulator renders the exact UI, takes keyboard input, exposes the same remote link, and
  can fake LEDs, sensors, pins and an I2C bus (`./pal sim --leds 5 --sensors 2 --gpios 4 --i2c`). The **browser simulator** (`./pal web`)
  runs the whole system in a tab: a key-accurate keyboard, hotkeys and key sequences, live LEDs, sensors, pins and I2C devices, editable
  Wi-Fi networks, a network bridge, and a log panel (`docs/SIMULATOR.md`).
- **Rules enforced by machines.** Layering, memory rules, API compatibility, file size and generated outputs are checked by
  `./pal check`; the UI is covered by golden images in two themes.

## Quick start

```sh
./pal check          # enforce the code rules
./pal test           # host unit tests
./pal sim            # run the simulator on your computer
./pal build          # build the hhv2 firmware (needs ESP-IDF)
./pal flash          # build, flash and reset the board
./pal log            # reset the board and print its serial output
```

Talk to a device from your computer (Palanu Link over a UART cable or TCP):

```sh
bun forge/tools/palanu.ts --serial /dev/cu.usbserial-10 info
bun forge/tools/palanu.ts --serial /dev/cu.usbserial-10 run 'dev led led0 red'
bun forge/tools/palanu.ts --serial /dev/cu.usbserial-10 shell
```

```
info | ping | ls /home | get PATH [OUT] | put FILE PATH   # files
shell | screen out.png | key NAME                          # remote shell, screenshot, key press
build app.c --id x | pack app.wasm --id x | install x.papp # WebAssembly apps (docs/PAPP.md)
ota firmware.bin                                           # firmware update with rollback
```

## Your first app

```c
#include "palanu.h"

void _start(void) {
    pal_puts("hello from WebAssembly on PalanuOS\n");
}
```

Build, install and run it on a device or on the simulator:

```sh
bun forge/tools/palanu.ts build hello.c --id hello
bun forge/tools/palanu.ts deploy hello.papp --run
```

See [docs/DEVELOPER.md](docs/DEVELOPER.md) for permissions, the API modules and examples, and
[docs/API.md](docs/API.md) for the complete, generated API reference.

## Devices are data

Every device the board declares can be used from the shell and from apps:

```
dev                              # list the board's devices
dev led led2 red                 # drive an LED (name, #rrggbb, or R G B)
dev sensor                       # read every sensor
dev gpio gpio0 1                 # drive a pin the board opened to apps
dev i2c i2c0 scan                # scan an I2C bus
```

See [docs/adr/0005-board-device-list.md](docs/adr/0005-board-device-list.md) and
[docs/adr/0006-device-table-all-kinds.md](docs/adr/0006-device-table-all-kinds.md).

## Bring your own board

A board is a descriptor plus drivers: [docs/PORTING-A-BOARD.md](docs/PORTING-A-BOARD.md) and `boards/_template` walk through it.

## Repository layout

| Path | Contents |
|---|---|
| `nema/` | kernel: memory, tasks, processes, handles, TTY |
| `system/` | subsystems: display, input, desktop, terminal, shell, app runtime and API |
| `apps/` | built-in apps and shell commands |
| `platform/`, `boards/`, `targets/` | ESP-IDF and host platforms, board descriptors, firmware targets |
| `api/` | the public app API definitions (the source of truth; SDK header and docs are generated) |
| `sdk/` | app SDK headers and examples |
| `forge/` | tools for your computer (Bun monorepo): `palanu` CLI, Palanu Link client, packager, Web Forge |
| `third_party/` | bundled libraries (WAMR and others) with their licenses |
| `docs/` | architecture, ADRs, coding rules, guides |
| `tests/` | host tests, golden images, hardware bring-up tests |

Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/STATE.md](docs/STATE.md).

## Status

PalanuOS is at 0.9.0, the first release candidate for hhv2. It is a candidate, not 1.0: the release gates in
[docs/RELEASE.md](docs/RELEASE.md) that need a long soak run or a second board are still open. Palanu Link does not encrypt data after
sign-in, so use a trusted network. There is no secure boot or flash encryption yet.

## Contributing and security

See [CONTRIBUTING.md](CONTRIBUTING.md), [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) and [SECURITY.md](SECURITY.md).

## License

MIT, see [LICENSE](LICENSE). Bundled third-party components keep their own licenses: see [NOTICE](NOTICE) and
[docs/SOURCES.md](docs/SOURCES.md).

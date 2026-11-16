# ESP-IDF setup for the ESP32-S31 (hhv2)

Set up on 12 November 2026, following the board maker's advice: use the latest ESP-IDF (`master`), because S31 support in v6.1 is still a preview.

## What is installed

| Component | Location / version |
|---|---|
| ESP-IDF | `~/esp/esp-idf`, branch `master`, commit `827cc622` (11 Nov 2026), reports itself as v6.2.0 |
| Toolchain and tools | `~/.espressif` (riscv32-esp-elf esp-16.1.0, OpenOCD, esp-clang, etc.) |
| IDF Python env | `~/.espressif/python_env/idf6.2_py3.15_env` (Python 3.15 from Homebrew) |
| Install log | `~/esp/idf_install.log` |

The clone is *shallow* (`--depth 1`) without tags, so `git describe` fails and IDF reads the version from source. This is not a problem for builds.

## Usage

Activate the environment in every new terminal:

```bash
. ~/esp/esp-idf/export.sh
```

A new project for the S31:

```bash
idf.py set-target esp32s31
```

In `sdkconfig.defaults`, set the flash size to 16 MB (the stock test firmware is still built for 4 MB):

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

Flash and monitor. Use baud **230400**, because 460800 and above are unstable over this board's CH340C:

```bash
idf.py -p /dev/cu.usbserial-10 -b 230400 flash monitor
```

## Updating to the latest master

```bash
cd ~/esp/esp-idf && git pull && git submodule update --init --recursive --depth 1 && ./install.sh esp32s31
```

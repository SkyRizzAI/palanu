# Board port template

This file explains how to add a new board. The rule: the system and the apps only ask about capabilities
(`pal_has(PAL_CAP_...)`), never about a board name. So a new board needs only one descriptor and its
drivers. The full list of bits and what they mean is in `docs/PORTING-A-BOARD.md`.

`template_board.c` in this folder is a skeleton: the descriptor, the key table, the hooks and the display driver, all
still empty. It is not part of the build, so `./pal build` and `./pal test` are not affected.

## Steps

1. **Copy the skeleton.** Create `boards/<name>/` from this folder, then replace the `TEMPLATE_` prefix and the name
   `"template"` with the name of your board. Do not use a board name in `system/` or `apps/`; `./pal check` rejects
   it.

2. **Set the capabilities.** Set the `PAL_CAP_*` bits only for devices that really exist on the board:
   `DISPLAY`, `KEYBOARD`, `BATTERY`, `SD`, `BUZZER`, `RGB_LED`, `MIC`, `USB_DEVICE`, `WIFI`, `BLE`, `PSRAM`,
   `TOUCH`, `CAMERA`, `SENSORS`. A bit that is not set makes an app show a "not supported" message instead of failing silently.

3. **Fill in the display geometry.** `display_w`, `display_h` and `display_format`. A board without a display uses zero and does not
   set `PAL_CAP_DISPLAY`.

4. **Key table.** One `pal_physkey_t` per physical key, row by row, `rows * cols` entries. See
   `boards/hhv2/hhv2_keyboard.c` for a complete example (4x10, with the Nm, Shift and Fn layers). Key presses
   are sent through `pal_input_hw(row, col, pressed)` from your matrix driver.

5. **Devices and hooks.** LEDs and buzzers are declared as a `devices` list (one entry per physical device, see
   `pal/device.h`; five LEDs mean five entries or one entry with `pixels = 5`). `ops` is only for `battery_*` and
   `reboot`; `ops.beep` and `ops.rgb` are deprecated (they are registered automatically as one device). Write the drivers in a separate file (`<name>_hw.c`), then export
   their functions to the board header.

6. **Display driver.** Fill in `present()` in `pal_display_ops_t`. This function receives one rectangle of the
   8-bit framebuffer and an RGB565 palette (already byte-swapped for the SPI bus), and must return after the panel
   has accepted it. On hhv2 this uses 40-row bands so that each SPI transaction stays under 32 KB.

7. **Target.** Create `targets/<name>/main/main.c` that fills `pal_boot_cfg_t` with `.board = &<NAME>_BOARD`,
   `.display = <name>_display_ops()` and `.hw_init`, then calls `pal_boot(&cfg)`. Copy the pattern from
   `targets/hhv2/main/main.c`.

8. **Hook it into the build.** The ESP-IDF component `platform/esp-idf/palanu/CMakeLists.txt` currently globs
   `boards/hhv2/*.c` and adds `boards/hhv2` to the includes. For a second board, point that glob at your board
   folder. The simulator (`platform/host/CMakeLists.txt`) does not use this board: it uses its own descriptor
   in `platform/host/sim_main.c`.

## Checking the skeleton

To check that the skeleton still compiles (without ESP-IDF, from the repository root):

    cc -std=c11 -Wall -Wextra -fsyntax-only -Isystem/include -Inema/include boards/_template/template_board.c

To build it as a host library, add this to `platform/host/CMakeLists.txt`:

    add_library(template_board STATIC ${ROOT}/boards/_template/template_board.c)
    target_include_directories(template_board PUBLIC ${ROOT}/system/include)

## Checklist before commit

- [ ] No board name in `system/` or `apps/` (`./pal check`).
- [ ] Every `PAL_CAP_*` bit that is set has a real hook or device on the board.
- [ ] `display_w` and `display_h` match the panel; `keyboard` points to the right table.
- [ ] The ESP-IDF target still compiles (`./pal build` is run by the orchestrator, not from the port).

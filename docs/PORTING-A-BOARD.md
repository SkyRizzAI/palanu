# Board descriptor and capabilities

The system and apps never ask "which board is this". They ask "is there a battery", "is there a buzzer",
through `pal_has(PAL_CAP_...)`. The board descriptor (`pal_board_desc_t`, `system/include/pal/board.h`) is one table
that describes the board: name, chip, capabilities, display geometry, key table, and device hooks. Adding a board
means writing one descriptor and its driver. The full steps are in `boards/_template/README.md`.

## Descriptor contents

| Field | Meaning |
|---|---|
| `name`, `chip` | Board and chip name. Shown in Settings and in the link info (`send_info`). |
| `caps` | OR of `PAL_CAP_*` bits. Only bits for devices that really exist. |
| `display_w`, `display_h`, `display_format` | Display geometry. Zero when there is no display. |
| `keyboard` | Key table (`pal_keyboard_t`), or NULL when there is no keyboard. |
| `devices`, `ndevices` | Device list (`pal/device.h`): LEDs (one or many pixels, RGB or single color) and buzzers, each with an `id`, `name`, and hooks. Settings, the `dev` command, and the `dev` app API are built from this list. |
| `ops` | Optional hooks: `battery_percent`, `battery_mv`, `reboot` (`beep` and `rgb` are deprecated: used when `devices` is empty). |

## Capabilities

| Bit | Meaning |
|---|---|
| `PAL_CAP_DISPLAY` | Framebuffer panel; `pal_display_init` runs at boot. |
| `PAL_CAP_KEYBOARD` | The keys from `keyboard` are passed to `pal_input_init`. |
| `PAL_CAP_BATTERY` | The supply voltage can be measured (the battery hooks are called). |
| `PAL_CAP_SD` | A removable SD card slot. |
| `PAL_CAP_BUZZER` | There is a buzzer (computed automatically from `devices`). |
| `PAL_CAP_RGB_LED` | There is an LED (computed automatically from `devices`). |
| `PAL_CAP_MIC` | There is a microphone. |
| `PAL_CAP_USB_DEVICE` | A USB device port. |
| `PAL_CAP_WIFI` | Wi-Fi radio. |
| `PAL_CAP_BLE` | Bluetooth LE radio. |
| `PAL_CAP_PSRAM` | External RAM (`PAL_MEM_EXTERNAL`). |
| `PAL_CAP_TOUCH` | Touch screen. |
| `PAL_CAP_CAMERA` | Camera. |
| `PAL_CAP_SENSORS` | Motion, light, or environment sensors. |

## Use in code

- `pal_has(cap)` is true only when **all** bits of `cap` are set. `pal_has(0)` is always false.
- `pal_board_info()` returns the running descriptor. `pal_board()` is the same, kept for old callers.
- `pal_beep`, `pal_rgb`, `pal_battery`, and `pal_battery_mv` check their own capability. Without the capability, they
  do not call the hook and return -1 (or do nothing). The `battery` command goes through `pal_battery_mv` and therefore reports
  "not supported" on a board without a battery.
- The status bar uses `pal_battery()`: when it is -1, the battery icon is not drawn.
- Boot (`pal_boot_cfg_t.board`) uses the descriptor for the keyboard and display. The display geometry comes from the descriptor,
  not from `pal_display_ops_t.width`.

## Rules

`./pal check` rejects board names (`hhv2`, `HHV2_...`, `hhv2_...`) as strings or identifiers in `system/` and
`apps/`. Exceptions are registered in the `ALLOW` table in `forge/tools/pal.ts` with a written reason. Comments may
mention the board name.


## Devices other than LEDs and buzzers (ADR-0006)

Fill `devices` with one entry per device: `PAL_DEV_BATTERY` (`percent`, `mv`), `PAL_DEV_STORAGE` (`removable`),
`PAL_DEV_SENSOR` (`unit`, `read` which returns a value ×1000), `PAL_DEV_GPIO` (`pin`, `app`, `read`, `write`) and
`PAL_DEV_BUS` (`type`, `app`, `probe`). The `RGB_LED`, `BUZZER`, `BATTERY`, `SD` and `SENSORS` bits are computed from the list; do not write them yourself.
A pin is exposed to apps only if `app = true`. Try it with `dev` in the terminal and, in the simulator, `--sensors`, `--gpios`, `--i2c`.

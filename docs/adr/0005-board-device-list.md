# ADR-0005: Device list in the board descriptor

Date: 16 November 2026. Status: in force.

## Context
The board descriptor (`pal_board_desc_t`) expresses a device as one capability bit and one hook: `PAL_CAP_RGB_LED`
with `ops.rgb`, `PAL_CAP_BUZZER` with `ops.beep`. A board with five LEDs, two buzzers, or a monochrome LED cannot be
expressed, and the Settings > LEDs page only knows one RGB LED. The goal of the project is for other developers to install
PalanuOS on their own boards; other people's boards will almost certainly differ in how many devices they have. An API frozen with the model
"one device per kind" would have to change after release and would break other people's apps.

## Decision
1. The descriptor gets a list `const pal_device_t *devices` and `int ndevices` (`pal/device.h`). One entry = one
   physical device: a stable `id` ("led0", "status"), a `name` for the screen, a `kind`, ops, and a driver-owned `ctx`.
2. The first kinds: `PAL_DEV_LED` (pixel count and color count: 1 = brightness only, 3 = RGB) and `PAL_DEV_BUZZER`. Other kinds
   (button, sensor, GPIO, I2C/SPI bus) add an enum value and ops without changing what exists.
3. Apps and the system find devices through `pal_dev_count(kind)`, `pal_dev_get(kind, n)`, `pal_dev_find(id)`; never
   through a board name. Settings is built from that list, so five LEDs produce five rows that can each be tested.
4. Capability bits remain as a quick summary (`pal_has`) and are computed automatically from the list when a board uses the list.
5. Compatibility: an old board that only fills `ops.rgb` / `ops.beep` is registered automatically as one LED ("led0") and
   one buzzer ("buzzer0"). `pal_rgb()` lights all LEDs; `pal_beep()` uses the first buzzer. No old code breaks.
6. hhv2 and the simulator use the list; the simulator can imitate a multi-LED board (`--leds N`) to test the pages and apps.

## Consequences
- Adding a device kind means adding an enum, ops, and a Settings page; not adding a single bit.
- Runtime permissions for apps (who may touch which device) will later refer to the device `id`.
- `ops.rgb` and `ops.beep` are considered deprecated; they are kept until API 1.0.

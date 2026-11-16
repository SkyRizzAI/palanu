# ADR-0006: One device table for all device kinds

Status: accepted. Complements [ADR-0005](0005-board-device-list.md).

## Context

ADR-0005 made LEDs and buzzers dynamic. The rest still lives in the system: the `PAL_CAP_*` bits for battery, SD card and sensors
are written by hand by the board, the battery is just one hook in `ops`, and the system does not know any pin, so GPIO/I2C for apps
cannot be built without fixing pins in the system. PalanuOS must be board-agnostic: each board writes its own hardware, pins, and
capabilities, and the system manages the count and kinds.

## Decision

1. **One `devices` table** in `pal_board_desc_t` holds all kinds: `LED`, `BUZZER`, `BATTERY`, `STORAGE`, `SENSOR`, `GPIO`,
   `BUS` (I2C/SPI/UART). One entry is one physical device, with a stable `id`, a `name`, a driver-owned `ctx`, and optional hooks.
   A new kind means one enum value and one struct in the union; old boards do not change.
2. **The `PAL_CAP_*` bits determined by devices are computed from the table** (`RGB_LED`, `BUZZER`, `BATTERY`, `SD` = removable
   storage, `SENSORS`) and override the bits the board wrote. Bits that have no device kind (display, keyboard, radio,
   PSRAM, USB) are still declared by the board; radio and display will follow as device kinds when needed.
3. **Compatibility:** a board that only fills `ops.rgb`, `ops.beep`, `ops.battery_*` is registered automatically as one
   device of each kind (`led0`, `buzzer0`, `battery0`).
4. **Pins never live in the system.** GPIO and buses exist only if the board declares them, complete with read/write hooks or a
   probe that uses that board's driver. The `app` marker says a pin is free for apps; a pin without `app` (and storage) is not
   visible to apps (`pal_dev_app_visible`): through the app API its index answers `PAL_ERR_NOENT`. The terminal (`dev`) is raw
   access for the owner and sees everything that is declared.
5. **App API level 4** (permission `dev`): `pal_dev_gpio_read/write`, `pal_dev_sensor_read` (value ×1000), `pal_dev_read_battery`,
   `pal_dev_i2c_probe`; `pal_dev_info` reports the new kinds. Terminal: `dev gpio|sensor|battery|i2c`.
6. **Settings pages are built from the table.** The Sensors page reads one row per sensor; a board with three sensors
   shows three rows with no new code in the system. The battery uses the first battery device.
7. **The simulator** tests without a board: `--sensors N`, `--gpios N`, `--i2c` (besides `--leds N`).

## Consequences

- hhv2 declares `battery0`, `flash0`, `sd0`; there is no GPIO/bus yet because no pin has been declared free for apps.
  Adding them is one entry per pin in `hhv2_board.c`.
- Not done yet: SPI/UART buses for apps (only I2C probe), Settings pages per battery/storage, camera, microphone,
  touch and radio devices, full I2C read/write, and per-id permissions.

# Architecture

Short on purpose; details are in the ADRs and the per-subsystem documents.

## Layers (dependencies point down only)

```
apps/        built-in apps (terminal, Settings, launcher, shell commands)
system/      subsystems: display, input, desktop, terminal, shell, app API, papp
nema/        kernel: memory, tasks, processes, handles, TTY
port         nema/port.h and the HAL interfaces
platform/ boards/ targets/   the only places for platform code (ESP-IDF, host, hhv2)
```

`nema/`, `system/` and `apps/` are portable. The build enforces this direction (private headers are hidden from the layers above)
and `./pal check` catches violations earlier.

## Board-agnostic

A board is one descriptor (`pal_board_desc_t`): name, screen, keyboard, capability bits, and a `devices` table (LEDs, buzzers,
batteries, storage, sensors, GPIO, buses, camera, microphone, touch, radios). The system contains no board name and no pin
number. The number and kind of devices come from the table, and the capability bits that devices decide are computed from it
(ADR-0005, ADR-0006).

## Apps

Apps are WebAssembly (WAMR), packaged as `.papp` with a permission manifest, and call the `palanu` API (`api/palanu/*.h` is the
source of truth; the SDK header, the glue and docs/API.md are generated). Hardware is reachable only through the API; a pin or
bus is visible to an app only when the board marks it as app-safe.

## Tools on your computer

`forge/` (Bun monorepo): the `palanu` CLI, the Palanu Link client (UART/TCP), the `.papp` packager and the API generator.

## Testing

Host tests in `tests/unit/`, golden images in `tests/golden/`, simulator tests, and `./pal smoke` on the board.

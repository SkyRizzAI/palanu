# ADR-0008: The simulator also runs in the browser, inside Web Forge

Status: accepted.

## Context
The native simulator needs a build, SDL2 and a computer that has them. Trying PalanuOS, showing it, and setting up the hardware an app expects
(LEDs, sensors, pins, I2C devices, Wi-Fi networks) should need only a browser, and the simulated board should be configurable at run time instead of by flags.

## Decision
- Build the same portable libraries with Emscripten (`platform/web`) and run the system in a page of Web Forge (`forge/web/src/sim`). Nema tasks are pthreads,
  which become Web Workers; the page is cross-origin isolated for the shared memory. No part of `nema/`, `system/` or `apps/` changes.
- Move the simulated hardware (devices, Wi-Fi, radios) out of `sim_main.c` into `platform/host/sim_hw.c`, used by both simulators, with setters the page
  calls while the system runs. The native simulator's flags and behaviour stay as they were; the golden images are unchanged.
- WAMR is built for the `linux` platform with its C native-call path, and the link uses function-pointer cast emulation (WAMR's native calls cast
  function pointers; WebAssembly traps on a signature mismatch). The link step stays at `-O1`: `wasm-opt -O3` fails validation after that pass.
- Sockets: the browser opens only WebSockets. A page-level bridge maps each connect to a direct WebSocket or to a proxy that forwards raw TCP
  (`forge/web/serve.ts`, same-origin pages only, this computer only). No credentials are stored.
- Persistence: `/data` is IndexedDB (IDBFS), flushed every three seconds and when the page hides.
- Emscripten is a developer tool installed into the ignored `refs/emsdk` by `./pal web setup`; nothing from it is committed.

## Consequences
- A second target to build and keep working (CI builds it). Code that is not portable (an OS or board header in `system/`) now fails twice.
- The page can show a screen, keys and every simulated device; features that need an air interface (Bluetooth peripherals, LAN discovery) stay out of reach.
- Static hosting without the isolation headers cannot run the simulator; Web Forge's Remote page does not need them and still opens from disk.

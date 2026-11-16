# Coding rules

Enforced by `./pal check` (also run by `./pal build` and `./pal test`).

## Language and style
- Code, API names, and log messages are in English. Internal documents are in English.
- C11. No exceptions, no RTTI, no hidden allocations.
- Prefixes: `nema_` for the kernel, `pal_` for subsystems and the app API. Errors: `nema_err_t` (negative = failure).
- One file is at most about 800 lines (hard limit 1000). Split before that happens.
- Format: `.clang-format` and `.editorconfig` at the repo root.

## Layer boundaries
- `nema/`, `system/`, `apps/` are portable: they must not include ESP-IDF, FreeRTOS, or SDL headers. Everything platform-dependent goes through `nema/port.h` and the HAL interfaces.
- `platform/`, `boards/`, `targets/` are the only places for platform code.
- Dependencies point downward only: `apps` -> `system` -> `nema` -> `port`. The build enforces this: `apps` cannot see the private headers in `nema/src` or `system/src` (ESP-IDF through components and `PRIV_INCLUDE_DIRS`, host through the PUBLIC/PRIVATE include scope of the CMake targets), so a violating include fails at compile time. `./pal check` catches the same violations earlier.

## Memory
- No `malloc`, `calloc`, `realloc`, `free`, or `heap_caps_*` outside `platform/`. Use `nema_alloc(class, owner, size)` and `nema_free()`.
- Every allocation names a class (`INTERNAL`, `DMA`, `EXTERNAL`, `ANY`) and an owner (a subsystem or a process id).
- All tasks are created through `nema_task_create()`. Stacks are declared up front and always live in internal RAM.
- Internal RAM must not drop below the reserve (default 32 KB) because of ordinary allocations.

## Provenance
- Every feature and every adapted file has a standalone written reason. "It already exists elsewhere" is not a reason.
- Code from `refs/` is adopted only when it meets the five reuse conditions (see `docs/SOURCES.md`) and is recorded in that file in the same commit.
- From GPLv3 projects only concepts may be taken, never code or assets.

## Tests
- Every portable module has a host test in `tests/unit/`. Visible behavior (screens, terminal) has a golden image.
- A bug found on a board gets a host test when it can be reproduced on the host.

# AGENTS.md — PalanuOS

A C11 operating system on ESP-IDF for ESP32 handhelds (first board: hhv2, ESP32-S31, 320x240 screen, 39-key keyboard). It is
board-agnostic: each board declares its own hardware, pins and capabilities. For humans and agents; read the map first.

## Map
| Need | Read |
|---|---|
| Architecture and dependency direction | docs/ARCHITECTURE.md |
| Code rules enforced by machines | docs/CODING-RULES.md |
| Why a decision was made | docs/adr/ |
| Writing apps for the device | docs/DEVELOPER.md, docs/API.md, docs/PAPP.md |
| Bringing up a new board | docs/PORTING-A-BOARD.md, boards/_template |
| Origin and licenses of third-party code | docs/SOURCES.md |
| Release gates | docs/RELEASE.md |
| Current status | docs/STATE.md |

## Commands
`./pal check` (code rules) · `./pal test` (host tests, about 6 min) · `./pal golden check|update <prefix>` · `./pal sim` · `./pal build` · `./pal flash` · `./pal smoke` · `./pal release`

## Hard rules
1. Docs and code change in the same commit; a significant design decision becomes an ADR.
2. `nema/`, `system/`, `apps/` are portable: no ESP-IDF/FreeRTOS/SDL headers, no board names, no pin numbers. A board reaches the system through `pal_board_desc_t` and its `devices` table.
3. No `malloc`/`free` outside `platform/`; use `nema_alloc(class, owner, size)`. Large arrays go to `NEMA_EXT_BSS` (PSRAM); watch `ramcheck`.
4. Never edit generated files: `sdk/palanu.h`, `system/src/api_wasm_glue.c`, `system/src/api_impl.h`, `api/symbols.csv`, `docs/API.md`. Edit `api/palanu/*.h`, then run `bun forge/packages/apigen/src/cli.ts`.
5. Update golden images only with `./pal golden update` for scenes that really changed, after looking at the picture.
6. The app API only grows: a new function gets `PAL_SINCE(n)` and raises `PAL_API_LEVEL` and `PAL_PAPP_API`; released functions are never changed or removed.
7. No GPL code. Adapted files are recorded in docs/SOURCES.md in the same commit.
8. Done = `./pal check` clean, `./pal test` passing, and evidence in the report. "Proven on the board" means it ran on hhv2.
9. Everything in the repository is English. Commits follow Conventional Commits, with no Co-Authored-By trailer. Never push or tag without the owner's approval.
10. Only PalanuOS, Nema, Forge and Palanu Link are named things; everything else has a generic name. Never ask for or store a user's Wi-Fi password.

## Gotchas
- Resetting the board can trigger safe mode; test over serial, and do not touch a board someone else is testing.
- Internal RAM is tight with Wi-Fi: static internal 74.3/80 KB, PalanuOS 39.6/40 KB. A large addition will probably fail `ramcheck`.
- `./pal build` builds only the firmware; build the simulator with `./pal sim`, or you run a stale binary.
- An unquoted `#` in the device shell starts a comment.
- The firmware image is about 84% of the OTA slot; the release warns at 85% and fails at 95%.

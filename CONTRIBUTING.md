# Contributing to PalanuOS

Thanks for helping. This page is short; the details live in `docs/`.

## Set up

- ESP-IDF (master) for the board, Bun for the tools in `forge/`, and a C compiler with CMake for the simulator.
- `./pal check` enforces the code rules (`docs/CODING-RULES.md`); `./pal test` runs the host tests (about six minutes);
  `./pal golden check` compares screenshots with the references in `tests/golden/ref`.
- No board needed: `./pal sim`. The simulator takes `--leds N`, `--sensors N`, `--gpios N` and `--i2c` to try board devices.

## Before you open a pull request

1. `./pal check` is clean, `./pal test` passes, and `./pal build` succeeds. Act on `ramcheck` warnings; do not skip them.
2. If you changed what is drawn, run `./pal golden check`, look at the differences, then `./pal golden update <prefix>` only for
   scenes that really changed, and say so in the PR.
3. If you changed the app API, edit `api/palanu/*.h`, run `bun forge/packages/apigen/src/cli.ts` and commit the generated files
   (`sdk/palanu.h`, `docs/API.md`, `api/symbols.csv`, the glue). `./pal check` rejects stale output.
4. Write an ADR in `docs/adr/` for any significant design decision (context, decision, consequences).

## App API rules

- A new function carries `PAL_SINCE(n)` and raises `PAL_API_LEVEL` (and `PAL_PAPP_API`); the level only goes up.
- A released function keeps its signature and is never removed; deprecated ones are marked in the docs and keep working.
- A function that touches hardware or user data needs a manifest permission (`PAL_CAP`).

## New boards

A board writes its own descriptor, including the `devices` table (LEDs, buzzers, batteries, sensors, pins, buses, ...). The
system must not contain a board name or a pin number. See `docs/PORTING-A-BOARD.md`, `boards/_template` and ADR-0005/0006.

## Style

C11. Names are generic (only PalanuOS, Nema, Forge and Palanu Link are named things). Comments say why, not what. Everything in
the repository is English: code, identifiers, comments, docs, commit messages. Commits and PR titles follow Conventional Commits
(`feat(devices): ...`, `fix(sim): ...`), one change per commit, no `Co-Authored-By` trailers. See `docs/adr/0007-repo-conventions.md`.

By contributing you agree that your contribution is licensed under the MIT license, like the project.

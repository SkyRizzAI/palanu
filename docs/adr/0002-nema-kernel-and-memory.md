# ADR-0002: Nema, kernel and memory management

Status: accepted (13 November 2026)

## Context
An earlier prototype stopped because internal RAM ran out: Wi-Fi, BLE, DMA, and dozens of task stacks fought over a single ~230 KB pool with no clear owner. This chip (ESP32-S31) has 512 KB of internal RAM (459 KB free on hhv2) and 16 MB of PSRAM that can be used for DMA. There is no MMU.

## Decision
One small kernel, Nema, that owns memory, tasks, processes, handles, and TTYs. Its rules:

1. **Every allocation has a class and an owner.** Classes: `INTERNAL`, `DMA`, `EXTERNAL`, `ANY` (prefer PSRAM). An 8-byte header per block stores the size, class, owner, and distance to the start of the block, so usage per owner can always be reported (`free`, `top`).
2. **A 32 KB internal RAM reserve.** Ordinary allocations and task creation are refused when free internal RAM would drop below the reserve. Only critical paths (boot, radio) may break through it.
3. **Cache reclaimers.** Before refusing, the kernel calls the registered reclaimers (in priority order) to release memory that can be rebuilt.
4. **One task gate.** All tasks go through `nema_task_create()`: the stack is declared up front, always lives in internal RAM (flash operations disable the PSRAM cache), and is recorded per owner. A process task is refused when the reserve would be exceeded.
5. **One wait primitive.** `nema_wait_on(objs, pred)`: register first, test the predicate, then sleep. A wake-up cannot be lost. `read`, `write`, `poll`, `wait`, and the registry are all built on it, including the signal rules.
6. **Objects and handles.** Pipes, TTYs, and later files and sockets are `nema_obj_t` with operations that never block. Atomic lock-free refcounts so they are safe to call under any lock (a reversed lock order between the pipe lock and the wait lock was a real bug found through testing).
7. **Processes.** Every running app is a process: id, arguments, three stdio handles, signals (INT, TERM, KILL, HUP, WINCH), exit code. Built-in processes are cooperative: a signal makes a blocking call return `NEMA_ERR_INTR`. Isolation for third-party apps comes from the sandboxed runtime (ADR-0003), not from the kernel.
8. **Two-sided TTY.** The process side (ordinary handles) and the terminal side (an on-screen emulator, a link channel, or a UART) can replace each other. Line mode (echo, editing, Ctrl-C, Ctrl-D, `\n` becomes `\r\n`) and raw mode.
9. **Leak reporting.** When a process exits, its leftover allocations are reported.

## Why the alternatives were rejected
- *A single heap with no owners* (as before): causes the problem avoided here.
- *Stacks in PSRAM*: forbidden for tasks that touch flash; a uniform rule is simpler.
- *A task per feature*: the number of tasks is controlled by the gate, and subsystems share a loop when they do not need their own task.

## Consequences
- All code outside `platform/` is forbidden from calling `malloc`/`free`/`heap_caps_*`; `./pal check` rejects it.
- The internal RAM budget can be audited: `free` prints a table per owner.
- Synchronization is tested with random scheduling jitter (`NEMA_HOST_JITTER`). ThreadSanitizer on macOS hangs on the TTY tests (the debugging tooling hangs too), so it is not yet a test tool; recorded as follow-up work.

## Update: quotas, sweeps, and admission (T1.1, T1.2, T1.4)

**Per-process quotas.** The owner of a process slot (32..63, one per slot; 32 slots since T1.8) has a PSRAM quota (`mem_quota_ext`, default 2 MB) and an internal RAM heap quota (`mem_quota_int`, default 64 KB), set at spawn (`nema_spawn_t`, constants in `proc.h`). Stacks do not count toward the quota; they still go through the task gate. An allocation that exceeds the quota returns NULL without triggering a reclaimer. `ANY` falls back to the other class when the PSRAM quota is used up. Quotas and peaks are reset when a slot is reused. `ps` and `free` show the quotas.

**Sweep on exit.** Every heap block of a process owner is put on a per-owner linked list. The link is stored just below the 8-byte header, so kernel and subsystem owners pay nothing. Allocation and free are O(1) under the same lock as admission. When a process exits (normally, killed, or by signal), `proc_finish` frees the remaining blocks before the slot can be reused, then records the number of bytes swept. Pipes and TTYs are forced to the `ipc` subsystem owner (`nema_mem_shared_owner`) because both are shared through reference counts: the shell puts a background pipe under its own process owner (`apps/sh.c:342`), and a sweep would free a pipe still used by another process.

**Admission.** The quota, floor, and largest block are checked, then the allocation is made, all under one lock. For non-critical INTERNAL/DMA, the floor is checked again after the real allocation, then rolled back if violated, because the port charges a size that malloc may have rounded. A request of 4 KB or more must also fit in the largest free block (2 KB margin). A floor, block, or heap refusal runs the reclaimers once, outside the lock, then tries again. The task stack set-aside (`nema_mem_task_reserve`) also counts against the floor until the port actually takes the stack.

**Terminal history reclaimer.** `pal_term_trim_history()` (system/src/term.c) frees the scrollback history of all active terminals. The history is locked with a binary semaphore. The reclaimer only polls, so it never blocks an allocation and does not deadlock when the same task is already holding the lock. Note: scrollback is allocated `ANY`, so on the board it usually sits in PSRAM; this reclaimer helps PSRAM and internal RAM admission only when the scrollback is there.

**The SYSTEM role and NEMA_ALLOC_CRITICAL (audit).** The SYSTEM role is only for subsystems created at boot (the floor is not guarded for them). Audit results as of 13 November 2026:

| Location | User | Decision | Reason |
|---|---|---|---|
| `system/src/desktop.c:260` | desktop task | stays SYSTEM | created at boot (`apps/boot.c:108`); without a UI the device cannot be used |
| `boards/hhv2/hhv2_hw.c:275` | keys task | stays SYSTEM | created at boot (`pal_input_init`, `apps/boot.c:95`); without input it cannot be recovered |
| `system/src/link.c:441` | reboot task after OTA | **change**: run `reboot_later(NULL)` directly in the worker, without a task | neither boot nor radio; an extra task is unnecessary and can fail silently |
| `platform/net_bsd/net_bsd.c:151` | radio pump (lwIP) | stays SYSTEM | radio; this file is handled by another agent, not re-audited yet |
| `platform/esp-idf/palanu/console_uart.c:117,121,124` | UART console | stays SYSTEM | boot diagnosis path; this file is handled by another agent |
| `tests/unit/test_pipe.c:92`, `test_net.c`, `test_registry.c` | tests | stays | tests, not production code |
| `nema/src/wait.c:25` | task context (`nema_tctx`) | stays CRITICAL | every waiting task needs these 16-32 bytes; without it a task cannot wait at all |
| `boards/hhv2/hhv2_hw.c:212` | LCD DMA band buffer | **change**: remove `NEMA_ALLOC_CRITICAL` | allocated at boot while the floor is still intact; it does not need to cross the floor |
| `tests/unit/test_mem*.c`, `test_proc.c` | floor tests | stays | deliberately test the critical path |

The two changes marked **change** have not been made in this tree because `link.c` and `boards/hhv2/hhv2_hw.c` were being changed by another agent. Both are one-line changes that must be applied once those files are stable.

## Update: kernel limits, cancellation (T1.7, T1.8)

**Limits.** 32 process slots (`NEMA_PROC_MAX`), 64 owners (6 header bits: kernel 0, subsystems 1..31, processes 32..63), 16 wait
objects (`NEMA_WAIT_OBJS_MAX`), 48 tasks (`MAX_TASKS`: 32 processes plus boot services). The command registry grows
on demand in PSRAM (no fixed limit); `nema_app_unregister()` removes a command, and is refused with
`NEMA_ERR_BUSY` while that command is running. In `ps` and `top`, the process list is read from the heap, not from the app stack.

**TERM and KILL.** TERM makes a blocking call return `NEMA_ERR_INTR`; if the process is still running
`NEMA_TERM_GRACE_MS` (3 seconds) later, the reaper sends KILL. The reaper is a service task that only exists while there is a TERM
waiting, so an idle system has no extra task. KILL marks the process (`killed`): every blocking call,
including read and write, returns `NEMA_ERR_CANCELED` immediately; the hooks (wasm instance) are run; the exit
code is reported as 137. A task is never forcibly deleted, because it may be holding a lock. A process that is stuck without
calling an API cannot be stopped from outside; for wasm, the fuel limit is the only stopper (ADR-0003).

**Defer.** `nema_cancel_defer()` / `nema_cancel_allow()` hold signals (nestable). File writes (`NEMA_OBJ_ATOMIC_WRITE`,
set by RAM and flash files) are wrapped in defer, so a signal that arrives in the middle of a write takes effect after it.


## Update: internal RAM guarded at build time (2026-11-14)

The incident: after the UI work, `wifi on` was refused because free internal RAM was down to 43 KB. The cause was not allocation at
run time but static data: every screen carried its own 2.4 KB canvas, tables, and menu store in internal `.bss`
(178 KB). This is the same mistake as in the earlier prototype: memory used up bit by bit with nobody refusing, until Wi-Fi and
BLE no longer fit together.

The decision:

1. Static data that is zero at the start and 1 KB or larger in `system`, `apps`, `nema`, `platform`, `boards` is written
   with `NEMA_EXT_BSS` (`nema/include/nema/types.h`). On hhv2, that data lives in PSRAM
   (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`); on the host, the macro is empty.
2. Data with initial values cannot go to PSRAM. Its initial values are moved into code (for example in `enter()`), or its large part
   (the canvas) is split into a separate variable.
3. Only what has a written reason stays in internal RAM: kernel tables, kv (the flash write path), the WAMR native table,
   and the virtual keyboard state. The list is in `ALLOWED` in `forge/tools/ramcheck.ts`.
4. `./pal build` and `./pal flash` run `forge/tools/ramcheck.ts` after linking. The build fails when there is any static data
   of ours ≥ 1 KB in internal RAM that is not registered, the total static data of ours exceeds 40 KB, or the total of all
   internal `.bss` + `.data` exceeds 80 KB. The budget may only be raised with a measurement in `docs/SIZE-BUDGETS.md` that
   shows Wi-Fi and BLE still run together.

Result on the board (Wi-Fi and BLE on together): internal RAM free 120 KB, minimum 80 KB, largest block 44 KB.
The Wi-Fi driver, lwip, and wpa_supplicant also put their `.bss` in PSRAM (an ESP-IDF linker rule); scan and connect have been
tested and work.

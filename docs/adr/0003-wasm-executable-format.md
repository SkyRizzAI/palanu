# ADR-0003: Executable format: WebAssembly on WAMR in a `.papp` wrapper

Status: accepted (13 November 2026)

## Context
Installed apps must be isolated (a broken app must not bring down the system), use a versioned system API whose permissions are checked, run in the simulator with the same binary, and be buildable with C, C++, Rust, or Zig. Research was done with real builds for the ESP32-S31 (not only by reading documentation). All results are build tests and host tests; nothing has been run on the chip yet.

## Evidence (verified by building/running on a Mac)
- **WAMR builds for esp32s31** with four small patches (selecting the `ilp32f` ABI, two headers, one duplicate `nanosleep` stub). Fast interpreter: about 55 KB of flash, about 2 KB of internal RAM; the app linear memory can be in PSRAM. The official package in the registry does not list the S31 yet, so the upstream source is used directly (commit `ffc2288`).
- **Native ELF through `elf_loader` 2.1.0** registers the S31 and can be built, but runs in M-mode with no address limits. An ELF app can read and write the whole system; executing from PSRAM requires turning off PSRAM memory protection. It cannot be confined.
- **Component Model / WASI 0.2-0.3:** there is no realistic runtime for this MCU in 2026.
- **Toolchain:** the clang bundled with Apple tools has a wasm32 backend and `rust-lld` ships with rustup, so a 248-byte `hello.wasm` was built without installing anything.
- **Speed (measured on a Mac, a rough estimate for the S31):** the fast interpreter is about 4-7x slower than native on loops; redrawing one screen pixel by pixel inside wasm takes roughly 20-50 ms on the S31. That is why apps use host primitives (blit, fill, text, TTY) for heavy work.

## Decision
1. Installed apps are **wasm32 core modules** run by the **WAMR fast interpreter**, with a versioned `palanu` import module and a WASI-like layer whose permissions are checked.
2. AOT, ELF, and the Component Model are **not** used in version 1. ELF only for trusted built-ins if it is ever needed.
3. A **`.papp` wrapper** (not only a wasm custom section), because installation must decide accept/reject before the module arrives, and a signature over the exact wasm bytes is simpler when the manifest is outside the wasm:

| Field | Size | Contents |
|---|---|---|
| magic `PLNU` | 4 | |
| format version | 1 | |
| flags | 1 | bit0 compressed, bit1 signed |
| header_len | 2 | |
| manifest_len | 4 | |
| stored payload_len | 4 | |
| original payload_len | 4 | |
| manifest | CBOR | id, version, minimum API version, permissions, memory and stack limits, kind (fg/bg), icon |
| payload | wasm | may be compressed; processed in 4 KB pieces |
| SHA-256 | 32 | over header + manifest + stored payload |
| Ed25519 | 64 | optional, over the hash |

Install flow: read the header and manifest, reject early if the API version or permissions do not match, stream the payload to a staging area while computing the hash, verify the hash (and signature), validate the module with the loader, and only then mark it installed. A copy of the manifest in a wasm custom section may exist for tools, but the wrapper manifest is authoritative.
4. Isolation is not left to WAMR alone: every import checks permissions and arguments itself, linear memory is limited through the manifest, and there is an execution watchdog.

## Risks and fallbacks
- S31 support in ESP-IDF is still a preview; the WAMR patches are local. Fallback: `elf_loader` for trusted built-ins only.
- Instruction limiting and the watchdog on the S31 are not yet proven. To be tested in T7.2 on the board.
- The app path is slow when drawing per pixel; the mitigation is the host primitive API.
- Not yet verified: execution on a real RV32, placement of WAMR allocations in PSRAM, the behavior of `wasm_runtime_terminate`.

## Research artifacts
The patches and the test project are in the research session scratchpad; the WAMR patch (`wamr-esp32s31.patch`) was copied into the repo when Phase 7 started.

## Update: memory and cancellation in WAMR (T1.3, T1.7)

**Per-process allocation.** WAMR is built with `WASM_MEM_ALLOC_WITH_USAGE=1` (host: `platform/host/CMakeLists.txt`; ESP:
`third_party/wamr/build-scripts/esp-idf/wamr/CMakeLists.txt`). Without it WAMR maps linear memory with `os_mmap`
outside all owners. With it, one hook takes all allocations (module, instance, exec env, linear memory) and picks the
owner from `nema_self()` at the time of the call. Outside a process (boot, tests) the owner is `wasm`. Linear memory is zeroed
(WebAssembly requires new pages to be empty). `memory.grow` is limited by `mem` from the manifest, in 64 KB pages (rounded
down, at least one page). Modules are loaded with `wasm_binary_freeable`, and the `.papp` file is freed right after
load: only one copy while the app runs. The exec env stack is allocated only once (in `create_exec_env`, manifest size);
instantiation uses 4 KB, which is only used if the module has a `start` function.

WAMR limits on the host: darwin defines `OS_ENABLE_HW_BOUND_CHECK` (guard pages around linear memory). Linear
memory now comes from the malloc hook, without those guards, so the host is built with software bounds checking
(`WAMR_DISABLE_HW_BOUND_CHECK=1`, the same as ESP-IDF, which has no hardware check). Tested by
`an_out_of_bounds_store_traps_inside_the_sandbox`.

Note: linear memory growth is a realloc (new block, copy, free the old one), so for a moment both are counted.
An increase within the quota can fail when the quota is almost full.

**Cancellation.** Tested on the host (`tests/unit/test_cancel.c`, `test_wasm.c`):
- KILL of a wasm app that is inside an import (`read`, `sleep_ms`) stops immediately: the import checks `nema_canceled()`.
- **Solved with WAMR patch no. 5** (13 Nov 2026, see `third_party/README.md`): a `kill_requested` flag in the exec env is checked at the interpreter metering points, so KILL stops a tight loop with no imports (tested in `test_wasm`: `kill_stops_a_pure_wasm_loop_without_fuel`). Note on the initial finding: KILL of a wasm app in a tight loop with no imports **did not stop** with `wasm_runtime_terminate` alone: tested (`wasm_runtime_terminate` was called, the process was still
  running 2 seconds later). The cause in the WAMR 2.4.3 source (`ffc2288`):
  - `wasm_runtime_terminate` only sets an exception (`wasm_set_exception`), which is not checked inside the fast interpreter loop.
  - The interpreter checks the `WASM_SUSPEND_FLAG_TERMINATE` flag (`CHECK_SUSPEND_FLAGS`) only when `WASM_ENABLE_THREAD_MGR`
    is active, and that flag is only set by `wasm_runtime_interrupt_blocking_op`, which is not called in this tree and needs
    `OS_ENABLE_WAKEUP_BLOCKING_OP` (POSIX; not available in ESP-IDF).
  - So enabling the thread manager adds nothing without changing WAMR. Not done.
- The stopper for a loop with no imports is fuel (`--fuel`, the instruction limit). An app without `fuel` in a tight loop cannot be
  stopped. This is a known limitation; the decision on a default fuel for installed apps has not been made.
- Not yet verified on the chip: everything above was tested on the host. On the S31, WAMR allocation in PSRAM and fuel behavior have not been measured.

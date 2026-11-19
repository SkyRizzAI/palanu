# Third-party libraries

## lua (5.4.7)
MIT (the notice is at the end of `lua.h`). Origin: the official archive `https://www.lua.org/ftp/lua-5.4.7.tar.gz`, archive SHA-256
`9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30` (as lua.org lists it). Files: everything in `src/` except the
stand-alone programs `lua.c` and `luac.c`, copied **unchanged**. The builds compile the core and the base, string, table, math, utf8
and coroutine libraries (`apps/CMakeLists.txt`, `platform/host/libs.cmake`) with `LUAI_MAXCCALLS=64`. Used by `apps/lua_rt.c`
(ADR-0019). Tests: `tests/unit/test_lua.c`.

## monocypher (4.0.2)
BSD-2-Clause or CC0-1.0 (BSD-2-Clause chosen; `LICENCE.md` copied whole). Origin: the official archive
`https://monocypher.org/download/monocypher-4.0.2.tar.gz`, archive SHA-256 `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`.
Files: `src/monocypher.c`, `src/monocypher.h`, `src/optional/monocypher-ed25519.c` and `.h` (moved up one level so that their includes need no extra path).
All copied **unchanged**; `monocypher-ed25519` carries its own SHA-512.
Used by `system/src/crypto_prim.c` and `crypto_mac.c`. Tests: `tests/unit/test_crypto.c` (RFC 8439, 7748, 8032).

## wamr (WebAssembly Micro Runtime 2.4.3, commit ffc2288)
Apache-2.0 with the LLVM exception. Copied whole from upstream without git history, with the unused platforms and libraries removed (only `darwin`, `linux`,
`esp-idf`, `common`, `include` and the interpreter core are kept). Changes against upstream:

1. `build-scripts/esp-idf/wamr/CMakeLists.txt`: the `ilp32f` ABI for the ESP32-S31, `-Wno-undef`, the instruction limit and bulk memory turned on.
2. `core/shared/platform/esp-idf/espidf_file.c`, `espidf_platform.c`: the `sys/stat.h` header, the `renameat` declaration, the `nanosleep` stub removed (IDF 6 provides it).
3. `core/shared/platform/esp-idf/espidf_memmap.c`: `MALLOC_CAP_EXEC` is treated as absent when IDF hides it.
4. `core/shared/platform/esp-idf/espidf_thread.c`: `os_self_thread()` uses the FreeRTOS task handle, not `pthread_self()` (which fails on a plain task and took the board down
   when the first app ran).
5. `core/iwasm/common/wasm_exec_env.{h,c}`, `wasm_runtime_common.{h,c}`, `core/iwasm/include/wasm_export.h`,
   `core/iwasm/interpreter/wasm_interp_{fast,classic}.c`: `kill_requested` in the exec env, `wasm_runtime_request_kill()`,
   and a check of it in `CHECK_INSTRUCTION_LIMIT()`. `wasm_runtime_terminate()` alone does not stop a loop without
   import calls (the interpreter looks at the terminate flag only with the thread manager, which needs OS support the
   board lacks), and the instruction counter is copied to a local variable at function entry, so changing the limit from
   outside has no effect on a running loop. Cost: one volatile load per metering point.

## miniz (3.1.0, tag 3.1.0 of github.com/richgel999/miniz)
Only `tinfl` (DEFLATE decompression, RFC 1951). The files `tinfl.c` (= `miniz_tinfl.c` of that tag), `miniz_tinfl.h` and
`miniz_common.h` are copied **unchanged**. `miniz.h` and `miniz_export.h` are rewritten as a minimal entry that only
defines `MINIZ_NO_MALLOC`, `MINIZ_NO_STDIO`, `MINIZ_NO_TIME` and the endianness, then includes the two headers above.
No heap: the `tinfl_decompressor` is allocated through `nema_alloc` by the caller (`system/src/papp.c`). License in `LICENSE`.
Built with `-w` (like Monocypher) in `platform/host/CMakeLists.txt` and `system/CMakeLists.txt`.
Tests: `tests/unit/test_papp.c` (sections compressed by zlib, stored blocks, mismatched sizes, and truncation at every length).

## lvgl (9.2.2)
MIT (`LICENCE.txt` copied whole). Origin: `https://github.com/lvgl/lvgl/archive/refs/tags/v9.2.2.tar.gz`, archive SHA-256
`129b4e00e06639fa79d7e8a6cab3c1ecce2445b1a246652ccd34f22e7b17ad6f`. Kept: `src/` (minus the bundled DejaVu, SimSun and Source Han font data and the Montserrat sizes the configuration does not use; Montserrat 12, 16, 20 and 48 stay), `lvgl.h` and `lv_version.h`. All kept files are **unchanged**; the configuration is `system/src/athena/lv_conf.h`.
Built for the Athena desktop (docs/DESKTOPS.md) by `system/CMakeLists.txt` (firmware) and `platform/host/libs.cmake` (simulators); the configuration keeps only plain objects and labels. Built with `-w`. The Montserrat fonts inside (`src/font/lv_font_montserrat_*.c`) are SIL OFL 1.1 (Copyright 2011 The Montserrat Project Authors, https://github.com/JulietaUla/Montserrat); the Unscii fonts (`lv_font_unscii_*.c`) are public domain.

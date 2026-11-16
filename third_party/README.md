# Pustaka pihak ketiga

## monocypher (4.0.2)
BSD-2-Clause atau CC0-1.0 (dipilih BSD-2-Clause; `LICENCE.md` disalin utuh). Asal: arsip resmi
`https://monocypher.org/download/monocypher-4.0.2.tar.gz`, SHA-256 arsip `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`.
Berkas: `src/monocypher.c`, `src/monocypher.h`, `src/optional/monocypher-ed25519.c` dan `.h` (dipindah satu tingkat ke folder ini
agar include-nya tidak butuh jalur tambahan). Semua disalin **tanpa perubahan**; `monocypher-ed25519` membawa SHA-512 sendiri.
Dipakai oleh `system/src/crypto_prim.c` dan `crypto_mac.c`. Uji: `tests/unit/test_crypto.c` (RFC 8439, 7748, 8032).

## wamr (WebAssembly Micro Runtime 2.4.3, commit ffc2288)
Apache-2.0 dengan pengecualian LLVM. Disalin utuh dari upstream tanpa riwayat git, dengan platform dan pustaka yang tidak
dipakai dibuang (hanya `darwin`, `linux`, `esp-idf`, `common`, `include`, dan inti interpreter). Perubahan atas upstream:

1. `build-scripts/esp-idf/wamr/CMakeLists.txt`: ABI `ilp32f` untuk ESP32-S31, `-Wno-undef`, pembatasan instruksi dan bulk
   memory dinyalakan.
2. `core/shared/platform/esp-idf/espidf_file.c`, `espidf_platform.c`: header `sys/stat.h`, deklarasi `renameat`, stub
   `nanosleep` dihapus (IDF 6 sudah menyediakannya).
3. `core/shared/platform/esp-idf/espidf_memmap.c`: `MALLOC_CAP_EXEC` dianggap tidak ada bila IDF menyembunyikannya.
4. `core/shared/platform/esp-idf/espidf_thread.c`: `os_self_thread()` memakai handle task FreeRTOS, bukan `pthread_self()`
   (yang gagal pada task biasa dan menjatuhkan papan saat app pertama dijalankan).
5. `core/iwasm/common/wasm_exec_env.{h,c}`, `wasm_runtime_common.{h,c}`, `core/iwasm/include/wasm_export.h`,
   `core/iwasm/interpreter/wasm_interp_{fast,classic}.c`: `kill_requested` in the exec env, `wasm_runtime_request_kill()`,
   and a check of it in `CHECK_INSTRUCTION_LIMIT()`. `wasm_runtime_terminate()` alone does not stop a loop without
   import calls (the interpreter looks at the terminate flag only with the thread manager, which needs OS support the
   board lacks), and the instruction counter is copied to a local variable at function entry, so changing the limit from
   outside has no effect on a running loop. Cost: one volatile load per metering point.

## miniz (3.1.0, tag 3.1.0 dari github.com/richgel999/miniz)
Hanya `tinfl` (dekompresi DEFLATE RFC 1951). Berkas `tinfl.c` (= `miniz_tinfl.c` dari tag itu), `miniz_tinfl.h`, dan
`miniz_common.h` disalin **tanpa perubahan**. `miniz.h` dan `miniz_export.h` ditulis ulang sebagai entri minimal: hanya
mendefinisikan `MINIZ_NO_MALLOC`, `MINIZ_NO_STDIO`, `MINIZ_NO_TIME`, dan endianness, lalu meng-include dua header di atas.
Tanpa heap: `tinfl_decompressor` dialokasi lewat `nema_alloc` oleh pemanggil (`system/src/papp.c`). Lisensi di `LICENSE`.
Dibangun dengan `-w` (seperti Monocypher) di `platform/host/CMakeLists.txt` dan `system/CMakeLists.txt`.
Uji: `tests/unit/test_papp.c` (seksi terkompresi dari zlib, blok stored, ukuran yang tidak cocok, dan pemotongan di setiap panjang).

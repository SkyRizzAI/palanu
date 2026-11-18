# The portable libraries (nema, wamr, palsys, palapps) shared by the native simulator and the browser simulator
# (platform/web). The includer sets ROOT first.
find_package(Threads REQUIRED)

# ---- Nema kernel + host port ----
file(GLOB NEMA_SRC CONFIGURE_DEPENDS ${ROOT}/nema/src/*.c)
add_library(nema STATIC ${NEMA_SRC} ${CMAKE_CURRENT_LIST_DIR}/port_host.c ${CMAKE_CURRENT_LIST_DIR}/../net_bsd/net_bsd.c ${CMAKE_CURRENT_LIST_DIR}/../posix_fs/posix_fs.c)
target_include_directories(nema PUBLIC ${ROOT}/nema/include PRIVATE ${ROOT}/nema/src)
target_compile_options(nema PRIVATE -Wall -Wextra -Wshadow -Werror -Wno-unused-parameter)
target_link_libraries(nema PUBLIC Threads::Threads)

# ---- WebAssembly runtime (WAMR fast interpreter), see docs/adr/0003 ----
set(WAMR_ROOT_DIR ${ROOT}/third_party/wamr)
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_MINI_LOADER 0)
set(WAMR_BUILD_INSTRUCTION_METERING 1)
if(EMSCRIPTEN)
  # no native-call assembly for wasm: WAMR's C version, which calls through casted function pointers
  # (the web target links with -sEMULATE_FUNCTION_POINTER_CASTS)
  set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 1)
endif()
# Linear memory comes from the allocator hook (no guard pages), so bounds are checked in software. The ESP port has no
# hardware check either (docs/adr/0003).
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
if(EMSCRIPTEN)
  # its WASI type header clashes with Emscripten's libc; blocking operations are not used (no WASI, no wasm threads)
  list(FILTER WAMR_RUNTIME_LIB_SOURCE EXCLUDE REGEX "posix_blocking_op")
endif()
add_library(wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE})
if(EMSCRIPTEN)
  # Emscripten's libc already declares the WASI types (wasi/api.h); WAMR's own copy would clash, and WASI is off here
  target_compile_definitions(wamr PRIVATE _PLATFORM_WASI_TYPES_H)
endif()
target_include_directories(wamr PUBLIC ${IWASM_DIR}/include ${UTILS_SHARED_DIR} ${PLATFORM_SHARED_DIR} ${PLATFORM_SHARED_DIR}/../include ${IWASM_COMMON_DIR})
target_compile_options(wamr PRIVATE -w)
# Linear memory goes through the allocator hook (charged to the process), which needs the usage argument: system/src/wasm_rt.c
target_compile_definitions(wamr PUBLIC WASM_MEM_ALLOC_WITH_USAGE=1)

# ---- System: subsystems. system/src is private; the public API is system/include ----
file(GLOB SYSTEM_SRC CONFIGURE_DEPENDS ${ROOT}/system/src/*.c ${ROOT}/system/src/kui/*.c)
file(GLOB SYSTEM_API_SRC CONFIGURE_DEPENDS ${ROOT}/system/api/*.c)
# Monocypher (third_party/monocypher, vendored unmodified): no warnings-as-errors for it.
set(MONOCYPHER_SRC ${ROOT}/third_party/monocypher/monocypher.c ${ROOT}/third_party/monocypher/monocypher-ed25519.c)
set_source_files_properties(${MONOCYPHER_SRC} PROPERTIES COMPILE_OPTIONS "-w")
# tinfl (miniz 3.1.0, third_party/miniz): DEFLATE inflate for .papp format 2 sections. Vendored unmodified; see its README.
set(MINIZ_SRC ${ROOT}/third_party/miniz/tinfl.c)
set_source_files_properties(${MINIZ_SRC} PROPERTIES COMPILE_OPTIONS "-w")
# Same renamed symbols as the board build (see system/CMakeLists.txt): the chip ROM has its own miniz.
set(TINFL_RENAME TINFL_RENAMED
    tinfl_decompress=palanu_tinfl_decompress
    tinfl_decompress_mem_to_heap=palanu_tinfl_to_heap
    tinfl_decompress_mem_to_mem=palanu_tinfl_to_mem
    tinfl_decompress_mem_to_callback=palanu_tinfl_to_callback)
add_library(palsys STATIC ${SYSTEM_SRC} ${SYSTEM_API_SRC} ${MONOCYPHER_SRC} ${MINIZ_SRC} ${ROOT}/boards/hhv2/hhv2_keyboard.c ${ROOT}/boards/hhv2/hhv2_legend.c)
target_include_directories(palsys PUBLIC ${ROOT}/system/include PRIVATE ${ROOT}/system/src)
target_compile_options(palsys PRIVATE -Wall -Wextra -Wshadow -Werror -Wno-unused-parameter)
target_link_libraries(palsys PUBLIC nema wamr m)
target_compile_definitions(palsys PRIVATE WASM_ENABLE_INSTRUCTION_METERING=1 ${TINFL_RENAME})

# ---- Apps: built-in commands and views. Sees nema/include and system/include only, never the private dirs ----
file(GLOB APPS_SRC CONFIGURE_DEPENDS ${ROOT}/apps/*.c)
add_library(palapps STATIC ${APPS_SRC})
target_include_directories(palapps PUBLIC ${ROOT}/apps)
target_compile_options(palapps PRIVATE -Wall -Wextra -Wshadow -Werror -Wno-unused-parameter)
target_link_libraries(palapps PUBLIC palsys)

# ---- Athena: the LVGL desktop (system/src/athena). The firmware builds it in system/CMakeLists.txt ----
file(GLOB_RECURSE LVGL_SRC CONFIGURE_DEPENDS ${ROOT}/third_party/lvgl/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC ${ROOT}/third_party/lvgl ${ROOT}/system/src/athena)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w) # vendored unmodified
add_library(palathena STATIC ${ROOT}/system/src/athena/athena.c)
target_compile_options(palathena PRIVATE -Wall -Wextra -Wshadow -Werror -Wno-unused-parameter)
target_link_libraries(palathena PUBLIC palsys lvgl)

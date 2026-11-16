cmake_minimum_required(VERSION 3.22)
project(palanu_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

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
# Linear memory comes from the allocator hook (no guard pages), so bounds are checked in software. The ESP port has no
# hardware check either (docs/adr/0003).
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE})
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
# Optional: the SSH client on the host, for testing it against a local sshd. Build libssh from the component source
# with OpenSSL, then configure with -DPAL_LIBSSH_BUILD=<its build dir> -DPAL_LIBSSH_SRC=<its source dir>.
# cmd_ssh.c compiles its SSH commands only when libssh/libssh.h is found (__has_include).
if(PAL_LIBSSH_BUILD AND PAL_LIBSSH_SRC)
  set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
  find_package(OpenSSL REQUIRED)
  target_include_directories(palapps PRIVATE ${PAL_LIBSSH_SRC}/include ${PAL_LIBSSH_BUILD}/include)
  target_link_libraries(palapps PUBLIC ${PAL_LIBSSH_BUILD}/src/libssh.a OpenSSL::Crypto z)
endif()

# ---- Unit tests ----
enable_testing()
file(GLOB TESTS CONFIGURE_DEPENDS ${ROOT}/tests/unit/test_*.c)
foreach(t ${TESTS})
  get_filename_component(name ${t} NAME_WE)
  add_executable(${name} ${t})
  target_include_directories(${name} PRIVATE ${ROOT}/tests/unit ${ROOT}/sdk)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wshadow -Wno-unused-parameter)
  # Tests of apps (the shell, the Files, text viewer and Logs models) get the apps library; every other test links the system library only.
  if(name MATCHES "^test_(sh_|files|textview|logs)" AND TARGET palapps)
    target_link_libraries(${name} PRIVATE palapps)
  elseif(TARGET palsys)
    target_link_libraries(${name} PRIVATE palsys)
  else()
    target_link_libraries(${name} PRIVATE nema)
  endif()
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endforeach()

# ---- Simulator ----
if(TARGET palapps)
  add_executable(palanu-sim ${CMAKE_CURRENT_LIST_DIR}/sim_main.c)
  target_link_libraries(palanu-sim PRIVATE palapps)
  target_compile_options(palanu-sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
  # the simulator's board has no battery: `battery` must report it as unsupported (tests/sim)
  add_test(NAME sim_battery_unsupported COMMAND sh ${ROOT}/tests/sim/battery_unsupported.sh $<TARGET_FILE:palanu-sim>)
  set_tests_properties(sim_battery_unsupported PROPERTIES TIMEOUT 30)
  # every Settings page opens in the simulator (tests/sim/settings_walk.sh; about 1 s a page)
  add_test(NAME sim_settings_walk COMMAND sh ${ROOT}/tests/sim/settings_walk.sh $<TARGET_FILE:palanu-sim> ${CMAKE_CURRENT_BINARY_DIR}/settings-walk)
  set_tests_properties(sim_settings_walk PROPERTIES TIMEOUT 400)
  execute_process(COMMAND sdl2-config --cflags OUTPUT_VARIABLE SDL_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND sdl2-config --libs OUTPUT_VARIABLE SDL_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(SDL_LIBS)
    separate_arguments(SDL_CFLAGS_LIST UNIX_COMMAND "${SDL_CFLAGS}")
    separate_arguments(SDL_LIBS_LIST UNIX_COMMAND "${SDL_LIBS}")
    target_compile_definitions(palanu-sim PRIVATE HAVE_SDL=1)
    target_compile_options(palanu-sim PRIVATE ${SDL_CFLAGS_LIST})
    target_link_libraries(palanu-sim PRIVATE ${SDL_LIBS_LIST})
    message(STATUS "palanu-sim: SDL2 window enabled")
  else()
    message(STATUS "palanu-sim: SDL2 not found, scripted mode only")
  endif()
endif()

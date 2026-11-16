cmake_minimum_required(VERSION 3.22)
project(palanu_web C)

# The browser simulator: the same nema, system and apps libraries as the native simulator, built with Emscripten.
# Threads become Web Workers (the page must be cross-origin isolated: forge/web/serve.ts sets the headers).
#   source refs/emsdk/emsdk_env.sh && emcmake cmake -S platform/web -B build/web && cmake --build build/web
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# WAMR has no Emscripten platform in third_party/wamr; its POSIX layer (linux) builds on Emscripten's libc.
set(WAMR_BUILD_PLATFORM linux)
add_compile_options(-pthread)
add_link_options(-pthread)
include(${ROOT}/platform/host/libs.cmake)

add_executable(palanu-web ${CMAKE_CURRENT_LIST_DIR}/web_main.c)
target_link_libraries(palanu-web PRIVATE palapps)
target_compile_options(palanu-web PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(palanu-web PROPERTIES SUFFIX ".js")
target_link_options(palanu-web PRIVATE
  -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=16 -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=128MB -sSTACK_SIZE=1MB
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8 -sEXPORTED_FUNCTIONS=_main,_malloc,_free -sMODULARIZE=1 -sEXPORT_NAME=PalanuModule
  -sEMULATE_FUNCTION_POINTER_CASTS=1 "-sBINARYEN_EXTRA_PASSES=--pass-arg=max-func-params@21" -sENVIRONMENT=web,worker)

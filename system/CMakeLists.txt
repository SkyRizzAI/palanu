# Component "system": subsystems (display, gfx, desktop, input, term, link, papp, ...). system/src is private;
# the public API is system/include (pal/*.h). system/api holds the palanu API implementations; the generated glue
# is system/src/api_wasm_glue.c. Depends on nema's public headers and on wamr.
file(GLOB SYSTEM_SRC ${CMAKE_CURRENT_LIST_DIR}/src/*.c ${CMAKE_CURRENT_LIST_DIR}/src/kui/*.c)
file(GLOB SYSTEM_API_SRC ${CMAKE_CURRENT_LIST_DIR}/api/*.c)
# Monocypher (ChaCha20, Poly1305, X25519, Ed25519; third_party/monocypher). Vendored unmodified: warnings are off for it.
set(MONOCYPHER_SRC ${CMAKE_CURRENT_LIST_DIR}/../third_party/monocypher/monocypher.c
                   ${CMAKE_CURRENT_LIST_DIR}/../third_party/monocypher/monocypher-ed25519.c)
# tinfl (miniz 3.1.0, third_party/miniz): DEFLATE inflate for .papp format 2 sections. Vendored unmodified.
set(MINIZ_SRC ${CMAKE_CURRENT_LIST_DIR}/../third_party/miniz/tinfl.c)

# The chip ROM links its own, older miniz at the same symbol names (esp32s31.rom.ld: tinfl_decompress = 0x2f800100) and the
# linker script wins over our object: the firmware then calls a decompressor whose state struct has another layout, and a
# dynamic-Huffman stream overwrites the heap. Our copy is therefore renamed (found on the board).
set(TINFL_RENAME TINFL_RENAMED
    tinfl_decompress=palanu_tinfl_decompress
    tinfl_decompress_mem_to_heap=palanu_tinfl_to_heap
    tinfl_decompress_mem_to_mem=palanu_tinfl_to_mem
    tinfl_decompress_mem_to_callback=palanu_tinfl_to_callback)

# Athena, the LVGL desktop (system/src/athena, docs/DESKTOPS.md): LVGL 9.2.2 vendored unmodified in third_party/lvgl, its pool in PSRAM.
file(GLOB_RECURSE LVGL_SRC ${CMAKE_CURRENT_LIST_DIR}/../third_party/lvgl/src/*.c)
file(GLOB ATHENA_SRC ${CMAKE_CURRENT_LIST_DIR}/src/athena/*.c)

idf_component_register(
    SRCS ${SYSTEM_SRC} ${SYSTEM_API_SRC} ${MONOCYPHER_SRC} ${MINIZ_SRC} ${LVGL_SRC} ${ATHENA_SRC}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include
    REQUIRES nema
    PRIV_REQUIRES wamr mbedtls
)
set_source_files_properties(${MONOCYPHER_SRC} ${MINIZ_SRC} ${LVGL_SRC} PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../third_party/lvgl ${CMAKE_CURRENT_LIST_DIR}/src/athena)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE)
# Display strings are cut to fit on purpose (snprintf into a screen-sized buffer); GCC -O2 flags each one.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fstack-usage) # stack frames checked by forge/tools/ramcheck.ts
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${TINFL_RENAME})
# `PAL_RELEASE=1 ./pal build` (what `./pal release` runs): a release build, whose developer mode is off by default (trust.h).
if("$ENV{PAL_RELEASE}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PAL_RELEASE_BUILD)
endif()

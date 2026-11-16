# Component "nema": the portable kernel. nema/src is private to this component; apps and system see only nema/include.
# platform/net_bsd and platform/posix_fs are the POSIX backends of nema's net and posixfs APIs. They are compiled here,
# as the host nema library already does, because apps and system call them and the IDF linker cannot resolve a call
# from a lower component to a symbol defined only in the top platform component.
get_filename_component(PAL_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

file(GLOB NEMA_SRC ${CMAKE_CURRENT_LIST_DIR}/src/*.c)
file(GLOB NEMA_POSIX_SRC ${PAL_ROOT}/platform/net_bsd/*.c ${PAL_ROOT}/platform/posix_fs/*.c)

idf_component_register(
    SRCS ${NEMA_SRC} ${NEMA_POSIX_SRC}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include
    PRIV_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/src
    PRIV_REQUIRES lwip vfs # vfs: the socket pump's wake fd is an eventfd on the board (platform/net_bsd)
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter -fstack-usage) # stack frames checked by forge/tools/ramcheck.ts
# NEMA_EXT_BSS (nema/types.h): big zero-initialised UI data goes to PSRAM. PUBLIC, so system and apps see it too.
if(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC NEMA_HAVE_EXT_BSS)
endif()

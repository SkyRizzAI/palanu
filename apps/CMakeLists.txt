# Component "apps": built-in commands, views and the boot composition. Sees only the public headers of nema and
# system (never nema/src or system/src). Its headers are public only to the platform component (palanu).
file(GLOB APPS_SRC ${CMAKE_CURRENT_LIST_DIR}/*.c)

idf_component_register(
    SRCS ${APPS_SRC}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}
    REQUIRES nema system
    PRIV_REQUIRES libssh mbedtls # mbedtls: the headers of libssh's SFTP library below
)
# Display strings are cut to fit on purpose (snprintf into a screen-sized buffer); GCC -O2 flags each one.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fstack-usage) # stack frames checked by forge/tools/ramcheck.ts

# scp and sftp (cmd_sftp.c, ADR-0017): libssh's SFTP client part, which the libssh component leaves out of its build. Its sources
# are compiled with WITH_SFTP into a library of their own (LGPL-2.1, like the rest of libssh: kept apart from this component's code),
# against that component's headers; nothing in the component changes.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  idf_component_get_property(PAL_SSH_DIR david-cermak__libssh COMPONENT_DIR)
  idf_component_get_property(PAL_SSH_LIB david-cermak__libssh COMPONENT_LIB)
  idf_component_get_property(PAL_TLS_LIB mbedtls COMPONENT_LIB)
  set(PAL_SFTP_SRC ${PAL_SSH_DIR}/libssh-mirror/src/sftp.c ${PAL_SSH_DIR}/libssh-mirror/src/sftp_common.c
                   ${PAL_SSH_DIR}/libssh-mirror/src/sftp_aio.c)
  if(EXISTS ${PAL_SSH_DIR}/libssh-mirror/src/sftp.c)
    add_library(libssh_sftp STATIC ${PAL_SFTP_SRC})
    target_link_libraries(libssh_sftp PRIVATE ${PAL_SSH_LIB} ${PAL_TLS_LIB})
    target_compile_definitions(libssh_sftp PRIVATE WITH_SFTP=1 LIBSSH_STATIC)
    target_compile_options(libssh_sftp PRIVATE -Wno-format -Wno-int-conversion -Wno-char-subscripts -Wno-sign-compare)
    target_link_libraries(${COMPONENT_LIB} PRIVATE libssh_sftp)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PAL_HAVE_SFTP=1)
  endif()
endif()

# Lua 5.4 (third_party/lua, ADR-0019) for the lua command: the core and the libraries it opens, in a library of its own, built for size.
set(PAL_LUA_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/lua)
set(PAL_LUA_FILES lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm
    lundump lvm lzio lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib)
list(TRANSFORM PAL_LUA_FILES PREPEND ${PAL_LUA_DIR}/)
list(TRANSFORM PAL_LUA_FILES APPEND .c)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  add_library(palua STATIC ${PAL_LUA_FILES})
  target_include_directories(palua PUBLIC ${PAL_LUA_DIR})
  target_compile_definitions(palua PUBLIC LUAI_MAXCCALLS=64) # nested C calls and parser levels: within the lua command's stack
  target_compile_options(palua PRIVATE -Os -w)
  target_link_libraries(${COMPONENT_LIB} PRIVATE palua)
endif()

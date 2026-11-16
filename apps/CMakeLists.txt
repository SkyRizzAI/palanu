# Component "apps": built-in commands, views and the boot composition. Sees only the public headers of nema and
# system (never nema/src or system/src). Its headers are public only to the platform component (palanu).
file(GLOB APPS_SRC ${CMAKE_CURRENT_LIST_DIR}/*.c)

idf_component_register(
    SRCS ${APPS_SRC}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}
    REQUIRES nema system
    PRIV_REQUIRES libssh
)
# Display strings are cut to fit on purpose (snprintf into a screen-sized buffer); GCC -O2 flags each one.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fstack-usage) # stack frames checked by forge/tools/ramcheck.ts

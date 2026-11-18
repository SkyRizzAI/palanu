idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES palanu)

# The desktop sessions this firmware ships, in order of preference (the first is the default): any of classic, athena; empty ships none
# and the system console is the whole user interface. `PALANU_DESKTOPS="athena" ./pal build` picks them (docs/DESKTOPS.md). A session
# that is not listed is not linked: its code and its libraries (LVGL for athena) leave the image.
set(PALANU_DESKTOPS "classic;athena" CACHE STRING "Desktop sessions in this firmware, first = default; empty = console only")
set(PAL_DESKTOP_LIST "")
foreach(d IN LISTS PALANU_DESKTOPS)
  if(NOT d MATCHES "^(classic|athena)$")
    message(FATAL_ERROR "PALANU_DESKTOPS: unknown desktop session '${d}' (classic, athena)")
  endif()
  string(TOUPPER "${d}" D)
  string(APPEND PAL_DESKTOP_LIST "&PAL_DESKTOP_${D},")
endforeach()
target_compile_definitions(${COMPONENT_LIB} PRIVATE "PAL_DESKTOP_LIST=${PAL_DESKTOP_LIST}")
message(STATUS "PalanuOS desktop sessions: ${PALANU_DESKTOPS}")

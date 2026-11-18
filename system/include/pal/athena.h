/* Athena: an optional desktop environment whose home screen is drawn with LVGL (third_party/lvgl). It is built only into the
 * simulators (platform/host, platform/web): LVGL does not fit the internal RAM budget of a board yet. A platform that links it calls
 * pal_athena_register() once before pal_boot; `desktop use athena` then switches to it (docs/DESKTOPS.md). */
#ifndef PAL_ATHENA_H
#define PAL_ATHENA_H

void pal_athena_register(void);

#endif

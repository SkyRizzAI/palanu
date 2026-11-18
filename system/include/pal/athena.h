/* Athena: a desktop environment drawn in full colour with LVGL (third_party/lvgl): its own status bar, keys, sheets and home. A platform
 * whose panel driver takes RGB565 images (present_rgb565) calls pal_athena_register() once before pal_boot; `desktop use athena` then
 * switches to it (docs/DESKTOPS.md). */
#ifndef PAL_ATHENA_H
#define PAL_ATHENA_H

void pal_athena_register(void);

#endif

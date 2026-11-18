/* LVGL configuration for Athena (docs/DESKTOPS.md): software rendering into an RGB565 buffer, LVGL's own allocator on a pool the
 * session takes at its start and gives back at its stop, the Montserrat fonts (SIL OFL), and no optional extras. Everything not set here keeps LVGL's default. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (192 * 1024)
#define LV_MEM_ADR 0
#define LV_MEM_POOL_INCLUDE "athena_pool.h"
#define LV_MEM_POOL_ALLOC athena_pool /* the pool is taken at the session's start (PSRAM) and given back at its stop */
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 4
#define LV_USE_DRAW_SW 1
#define LV_USE_FLOAT 0
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_SYSMON 0
#define LV_USE_SNAPSHOT 0
#define LV_USE_VECTOR_GRAPHIC 0
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 1
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 32
#define LV_USE_THEME_MONO 0

/* Athena draws plain objects and labels only: the other widgets stay out of the image (the firmware's OTA slot is nearly full) */
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 0
#define LV_USE_BAR 0
#define LV_USE_BUTTON 0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LINE 1 /* the hands of the analog clock widget (athena_widgets.c) */
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_LOTTIE 0

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT (&lv_font_montserrat_12)

#endif

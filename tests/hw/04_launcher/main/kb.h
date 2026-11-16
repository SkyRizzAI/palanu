// TCA8418 keyboard as the LVGL keypad input.
// Letters are sent as ASCII characters (lower case, upper case while shift is held).
// ENTER -> LV_KEY_ENTER, DEL -> LV_KEY_BACKSPACE, ESC -> LV_KEY_ESC, SPACE -> ' '.
#pragma once
#include "lvgl.h"

// Call while holding the LVGL lock
lv_indev_t *kb_init(void);

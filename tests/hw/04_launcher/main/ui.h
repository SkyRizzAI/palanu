#pragma once
#include "lvgl.h"

// Builds the launcher. Call while holding the LVGL lock.
void ui_init(lv_display_t *disp, lv_indev_t *keyboard);

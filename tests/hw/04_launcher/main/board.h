// hhv2 board hardware functions used by the UI
#pragma once
#include <stdbool.h>

void board_set_brightness(int percent);   // 5..100, backlight PWM GPIO10
void board_click(void);                    // short click sound on the buzzer
void board_set_click_enabled(bool en);
bool board_click_enabled(void);

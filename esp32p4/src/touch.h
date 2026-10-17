#pragma once

#include <lvgl.h>

bool touch_begin (void);                // GT911, false when it does not answer
void touch_indev (lv_display_t *disp);  // LVGL pointer input device

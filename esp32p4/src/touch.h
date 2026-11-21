#pragma once

#include <lvgl.h>

bool touch_begin (void);                // GT911, false when it does not answer
void touch_indev (lv_display_t *disp);  // LVGL pointer input device
// asked at the start of each touch: false swallows the whole touch until the finger is lifted (touch lock)
void touch_set_filter (bool (*allow)(lv_point_t p));

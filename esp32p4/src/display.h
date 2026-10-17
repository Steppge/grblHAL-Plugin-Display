#pragma once

#include <lvgl.h>

#define LCD_W   1024
#define LCD_H    600

lv_display_t *display_begin (void);     // panel, backlight and the LVGL display, NULL on failure
void display_backlight (uint8_t pct);

#pragma once

#include <lvgl.h>

#define LCD_W   1024
#define LCD_H    600

lv_display_t *display_begin (void);     // panel, backlight and the LVGL display, NULL on failure
void display_backlight (uint8_t pct);
bool display_resync (void);             // restarts the video output (picture shifted sideways), from loop() only
void display_poll (void);               // from loop(): automatic resync after an underrun
void display_report (void);             // console: underruns, resyncs, buffers

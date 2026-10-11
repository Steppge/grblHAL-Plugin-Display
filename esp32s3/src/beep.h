#pragma once

#include <Arduino.h>

void beep_begin (void);
void beep (uint8_t count, uint16_t ms = 120, uint16_t gap_ms = 100);   // count tones, non-blocking
void beep_click (void);                                                 // short click for a button press
void beep_poll (void);                                                  // call from loop()

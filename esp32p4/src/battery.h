#pragma once

#include <stdint.h>

// Battery voltage on J7 pin 14 (GPIO53, ADC2): divider 1 MOhm from battery + / 1 MOhm to GND, 100 nF at the pin.
// Console: "batt" shows the voltage, "batt 3.95" calibrates to the voltage measured with a meter, "batt 0" resets.

void battery_begin (void);
void battery_poll (void);               // from loop(), measures every 2 s
bool battery_present (void);            // a battery is connected (more than 1 V at the divider)
float battery_volts (void);
int battery_percent (void);             // 0..100
void battery_console (const char *arg);

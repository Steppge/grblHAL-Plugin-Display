#pragma once

#include <stdint.h>

#define BATT_FULL_V     4.19f   // discharging: 100 % from here on
#define BATT_CHARGE_V   4.20f   // above only on the charger
#define BATT_CHARGED_V  4.20f  // charging: 100 % from here on (the charger stops), 99 % at most below
#define BATT_GREEN_V    4.21f  // clock screen: green, really full

#define BATT_X  720    // the battery line in the CNC status bar and on the dimmed clock screen
#define BATT_Y  18

// Battery voltage on J7 pin 14 (GPIO53, ADC2): divider 1 MOhm from battery + / 1 MOhm to GND, 100 nF at the pin.
// Console: "batt" shows the voltage, "batt 3.95" calibrates to the voltage measured with a meter, "batt 0" resets.

void battery_begin (void);
void battery_poll (void);               // from loop(), measures every 2 s
bool battery_present (void);            // a battery is connected (more than 1 V at the divider)
float battery_volts (void);
int battery_percent (void);             // 0..100, share of the run time left
int battery_minutes (void);             // remaining run time, -1 while charging
bool battery_charging (void);           // the level rose for some minutes, until it drops again
void battery_console (const char *arg);

// Discharge log for the remaining time: "battlog on" starts a new log in the flash (LittleFS /battlog.csv),
// a line per minute, it goes on after a restart until "battlog off". "battlog" shows the state,
// "battlog dump" prints the file. The display does not dim meanwhile, so the load stays the same.
bool battery_logging (void);
void battlog_console (const char *arg);

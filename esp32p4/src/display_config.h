#pragma once

// Settings of the ESP32-P4 display firmware (Elecrow CrowPanel Advanced 9", 1024x600).
//
// Change the values here, or better: put your own values into my_display_config.h next to this file.
// That file is read first if it exists and is not part of the repository (.gitignore), so your
// settings survive an update. Example my_display_config.h:
//
//   #define BIGDECK 1
//   #define BATTERY 0
//
// The fixed pins of the board (panel, touch, speaker, WiFi chip) are in pins.h.

#if __has_include("my_display_config.h")
#include "my_display_config.h"
#endif

// ---------------------------------------------------------------- PC-Deck mode

// 0 = CNC/laser display only (default).
// 1 = adds the "PC" key in the status bar: the display then works as a deck of the PC-Deck app
//     (tiles, volume, over the USB-C "UART" or the home network). Only useful with the PC-Deck app.
#ifndef BIGDECK
#define BIGDECK 0
#endif

// ---------------------------------------------------------------- clock

// 1 = time in the status bar, job end as a time of day, clock on the dimmed screen.
//     The time comes over NTP from the home network (enter it on the WiFi screen) or from the PC-Deck app.
// 0 = no clock: the job line shows the time left, the dimmed screen stays dark.
#ifndef CLOCK
#define CLOCK 1
#endif

// ---------------------------------------------------------------- rotary encoder

// EC11 encoder on J7: turning = feed override, push = reset to 100 %.
// A, B and the push button to GND, internal pull-ups:
//   J7 pin 20 GPIO50 A    J7 pin 18 GPIO51 B    J7 pin 15 GPIO5 button    J7 pin 21 GND
#ifndef ENCODER
#define ENCODER 1
#endif
#ifndef PIN_ENC_A
#define PIN_ENC_A       50
#endif
#ifndef PIN_ENC_B
#define PIN_ENC_B       51
#endif
#ifndef PIN_ENC_SW
#define PIN_ENC_SW      5
#endif
#ifndef ENC_STEPS
#define ENC_STEPS       4       // edges per detent
#endif
#ifndef ENC_REVERSE
#define ENC_REVERSE     0       // 1 = other turning direction
#endif

// ---------------------------------------------------------------- battery

// Battery voltage (e.g. IP5306 power bank board in the housing): 1 MOhm from battery + / 1 MOhm to GND,
// 100 nF to GND, at J7 pin 14 GPIO53 (ADC2, free: the WiFi runs on the ESP32-C6).
// 0 = no battery: no battery symbol and run time.
#ifndef BATTERY
#define BATTERY 1
#endif
#ifndef PIN_BATT
#define PIN_BATT        53
#endif
#ifndef BATT_DIV
#define BATT_DIV        2.0f    // divider ratio, 1 MOhm / 1 MOhm; fine tuning with the console command "batt <volts>"
#endif

// ---------------------------------------------------------------- gamepad receiver

// D1 Mini ESP32 with the gamepad firmware in the display housing, passed through to the controller:
//   J7 pin 9 GPIO2 TX -> D1 IO16 (RX)    J7 pin 11 GPIO3 RX <- D1 IO17 (TX)    J7 pin 1 3.3 V, pin 7 GND
// 0 = no gamepad receiver at the display.
#ifndef GAMEPAD
#define GAMEPAD 1
#endif
#ifndef PIN_GAMEPAD_TX
#define PIN_GAMEPAD_TX  2
#endif
#ifndef PIN_GAMEPAD_RX
#define PIN_GAMEPAD_RX  3
#endif
#ifndef GAMEPAD_BAUD
#define GAMEPAD_BAUD    460800  // as UART_BAUD of the gamepad receiver (D1 Mini, sketch 1.7 or newer)
#endif

// ---------------------------------------------------------------- map, clock, battery run time

// Directions of the map seen from the front of the machine: MAP_X_LEFT 1 = +X points to the left,
// MAP_Y_DOWN 1 = +Y points to the operator (front). The defaults suit a machine homing to the back right.
#ifndef MAP_X_LEFT
#define MAP_X_LEFT      1
#endif
#ifndef MAP_Y_DOWN
#define MAP_Y_DOWN      1
#endif

// 3D view of CNC files (oblique view of the depth map from the controller, display_link.c
// DISPLAY_PREVIEW_3D): a third step of the view button. 0: preview as before, without 3D.
#ifndef PREVIEW_3D
#define PREVIEW_3D      1
#endif

// Time zone of the clock as a POSIX TZ string, e.g. "GMT0BST,M3.5.0/1,M10.5.0" (UK), "EST5EDT,M3.2.0,M11.1.0" (US East)
#ifndef TIME_TZ
#define TIME_TZ         "CET-1CEST,M3.5.0,M10.5.0/3"    // Central Europe with summer time
#endif

// Battery: run time in minutes of a full charge, display on (battery.cpp). The voltage table in battery.cpp
// is the discharge curve of a 1S Li-ion pack (6 x 18650 here); log your own with the console command
// "battlog on" (one line a minute) and "battlog dump" if your pack differs.
#ifndef BATT_RUN_MIN
#define BATT_RUN_MIN    488
#endif

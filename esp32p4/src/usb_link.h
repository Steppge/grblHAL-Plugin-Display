#pragma once

#include <Arduino.h>

// Link to the Octopus-Bridge over USB-C "USB 2.0": a USB device with two CDC channels,
// 0 = display link (a Stream for link_use()), 1 = gamepad, passed through to the D1 Mini UART.
// Without the cable over the machine network of the bridge (TCP link, UDP gamepad).

enum link_path_t { Path_None, Path_Usb, Path_Wifi };

void usb_link_begin (void);
bool usb_link_mounted (void);       // the bridge has opened the link channel
link_path_t usb_link_path (void);   // the way the link takes right now
void usb_link_report (void);        // state and byte counts on the console
void usb_link_gamepad_hold (bool on);
extern bool gp_log;                 // $LG lines of the receiver (sketch 1.11) on the console (gplog on|off)   // the task leaves the D1 UART alone (gamepad update while running)

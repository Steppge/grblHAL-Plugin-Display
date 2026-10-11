#pragma once

#include <Arduino.h>

// WiFi with firmware updates over the air: ArduinoOTA (PlatformIO environment "ota") and an upload
// page at http://<host>/update. Settings are stored in NVS and entered on the display.

#define WIFI_HOST "grblhal-display"

enum wifi_state_t { Wifi_Off, Wifi_Connecting, Wifi_Connected };

struct wifi_cfg_t {
    bool enabled;
    char ssid[33];
    char pass[65];
    char ota[33];           // OTA and upload page password, OTA is off while empty
};

extern wifi_cfg_t wifi_cfg;

void wifi_begin (void (*progress)(unsigned pct));  // progress: update running, 0..100
void wifi_poll (bool updates_allowed);
void wifi_apply (void);                     // store wifi_cfg and (re)start or stop WiFi
wifi_state_t wifi_state (void);
const char *wifi_ip (void);

#define WIFI_SCAN_MAX 16

void wifi_scan_start (void);
int wifi_scan_result (void);                // -1 while scanning, else the number of networks found
const char *wifi_scan_ssid (int i);         // sorted by signal strength, no duplicates

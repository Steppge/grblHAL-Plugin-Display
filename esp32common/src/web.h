#pragma once

#include <WebServer.h>

// Web interface: a page for any browser (phone, tablet, PC) with the machine state, the map and the
// main controls. Commands go through the same queue as the touch buttons.
//
//   GET  /               the page (web/index.html, gzipped at build time)
//   GET  /api/status     machine state as JSON, polled by the page
//   GET  /api/preview    G-code preview bitmap of the loaded file
//   GET  /api/files      SD card file list, POST /api/files asks the controller for it again
//   POST /api/login      pass=<OTA password>: answers the token for the commands
//   POST /api/cmd        a=<action> (+ arguments), needs the header X-Token
//   POST /update         firmware upload (wlan.cpp), with the OTA password or the header X-Token
//
// Web mode (set on the display, WiFi screen): 0 off, 1 view only, 2 control. Control needs the
// OTA password once in the browser, the token stays valid until the password changes.

enum { Web_Off = 0, Web_View, Web_Control };

void web_begin (WebServer &server);
bool web_may_control (void);    // the current request carries a valid token (also accepted by /update)
void web_poll (void);

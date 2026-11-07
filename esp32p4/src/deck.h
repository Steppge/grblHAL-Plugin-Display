#pragma once

#include <lvgl.h>

// PC-Deck mode: the display as a deck of the PC-Deck app (F:\VisualCode\PC-Deck), same line protocol as
// Deck 1/2 over the console UART (USB-C "UART", CH340) and over the home network (TCP 5555, search UDP 5556).
// The console is read here as well: lines starting with a lower case letter go to the console handler.

void deck_begin (void (*console)(char *line));
void deck_poll (void);                  // protocol, network, encoder; from loop()
void deck_ui (lv_obj_t *root, int y);   // the deck page below the status bar, hidden
void deck_set_mode (bool on);           // PC-Deck mode on/off (home network, deck page)
bool deck_mode (void);
bool deck_app_connected (void);         // the PC-Deck app talks to the deck

#pragma once

// Firmware update from /firmware.bin on the display's microSD card, at start before wifi_begin().
// Restarts after a successful update, otherwise returns (no card, no file, or a bad file).
// show: progress on the screen (display running)
void sd_update_check (bool show);

// Console "gpupdate <size> <crc32 hex>": the gamepad receiver firmware over the console (tools/gp_update.py)
// into PSRAM, then over the gamepad UART to the receiver as from the card, while the display runs.
void gp_update_console (const char *arg);

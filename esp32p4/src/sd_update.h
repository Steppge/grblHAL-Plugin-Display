#pragma once

// Firmware update from /firmware.bin on the display's microSD card, at start before wifi_begin().
// Restarts after a successful update, otherwise returns (no card, no file, or a bad file).
// show: progress on the screen (display running)
void sd_update_check (bool show);

#pragma once

#define DISPLAY_VERSION "1.3"       // shown with the build date in Settings > Display firmware

void ui_init (void);
void ui_update (void);  // call every 100 ms
void ui_ota_progress (unsigned pct);
void ui_select_file (const char *name);     // load an SD file (preview, START), "" unloads
void ui_set_deck_mode (bool on);            // like the PC/CNC key (console "mode pc|cnc")
const char *ui_selected_file (void);
void ui_touch_lock_toggle (void);           // CNC: touch locked except Hold/Stop/Reset (short knob press)

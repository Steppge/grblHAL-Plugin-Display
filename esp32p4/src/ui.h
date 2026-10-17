#pragma once

void ui_init (void);
void ui_update (void);  // call every 100 ms
void ui_ota_progress (unsigned pct);
void ui_select_file (const char *name);     // load an SD file (preview, START), "" unloads
const char *ui_selected_file (void);

#pragma once

#include <Arduino.h>

#define MAP_Y_DOWN      1       // 1: +Y points to the operator (front), drawn downwards; 0: +Y upwards
#define MAP_X_LEFT      1       // 1: +X points to the left (seen from the front), 0: +X to the right

// What has been burned/cut: the tool path while the spindle/laser was on, in machine coordinates.

struct map_view_t {
    float x0, y0;       // machine position (mm) of the left/bottom edge
    float scale;        // pixels per mm
    int w, h;           // pixels
};

void map_init (void);
void map_record (void);                 // call for each new status line
bool map_changed (void);                // something new to draw since the last render
void map_extent (float *x0, float *y0, float *x1, float *y1);
void map_view_full (map_view_t *v, int w, int h);
void map_view_zoom (map_view_t *v, int w, int h);
void map_view_preview (map_view_t *v, int w, int h);   // fits the G-code preview, full view without one
void map_render (uint16_t *buf, const map_view_t *v, bool fine_grid);
bool map_to_px (const map_view_t *v, float mx, float my, int *px, int *py);

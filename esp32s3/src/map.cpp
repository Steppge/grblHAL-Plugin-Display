// Workpiece map: live trace of the tool path while the spindle/laser is on

#include "map.h"
#include "link.h"
#include "theme.h"

#define TRACE_RES       2       // trace cells per mm
#define MAP_Y_DOWN      1       // 1: +Y points to the operator (front), drawn downwards; 0: +Y upwards
#define MAP_X_LEFT      1       // 1: +X points to the left (seen from the front), 0: +X to the right
#define ZOOM_MARGIN    10.0f    // mm around the burned area in the zoom view
#define ZOOM_EMPTY     40.0f    // mm around the tool when nothing has been burned yet

static uint8_t *trace = NULL;
static int tw = 0, th = 0;              // trace size in cells
static float ex0, ey0, ex1, ey1;        // machine extent the trace covers (mm)
static float bx0, by0, bx1, by1;        // bounds of the burned area (mm)
static bool burned = false, changed = true;
static bool last_on = false;
static float last_x, last_y;
static uint32_t last_update = 0, last_state = STATE_IDLE;
static uint16_t preview_seq = 0;

#define PREVIEW_MARGIN  5.0f    // mm around the G-code preview

// Work offset: the preview is in work coordinates, the map in machine coordinates.
static float wco_x (void) { return mc.mpos[0] - mc.wpos[0]; }
static float wco_y (void) { return mc.mpos[1] - mc.wpos[1]; }

// Preview bit at a machine position, -1 outside the preview area
static int preview_at (float mx, float my)
{
    if(!preview.valid)
        return -1;

    float wx = mx - wco_x(), wy = my - wco_y();
    if(wx < preview.x0 || wx > preview.x1 || wy < preview.y0 || wy > preview.y1)
        return -1;

    // same scale as on the controller (display_link.c, pass 1)
    float dx = max(preview.x1 - preview.x0, 0.1f), dy = max(preview.y1 - preview.y0, 0.1f);
    float s = min((PREVIEW_W_MAX - 1) / dx, (PREVIEW_H_MAX - 1) / dy);
    int px = (int)((wx - preview.x0) * s), py = (int)((preview.y1 - wy) * s);

    if(px < 0 || py < 0 || px >= preview.w || py >= preview.h)
        return 0;

    return (preview.bits[py * (preview.w / 8) + px / 8] >> (7 - (px & 7))) & 1;
}

// Machine area in machine coordinates. grblHAL usually has negative machine space (homing to
// the max. end), the work envelope says so once homed, before that the position decides.
void map_extent (float *x0, float *y0, float *x1, float *y1)
{
    float tx = mc.travel[0] > 1.0f ? mc.travel[0] : 300.0f, ty = mc.travel[1] > 1.0f ? mc.travel[1] : 300.0f;

    if(mc.env_valid) {
        *x0 = mc.env_min[0]; *x1 = mc.env_max[0];
        *y0 = mc.env_min[1]; *y1 = mc.env_max[1];
    } else {
        *x0 = mc.mpos[0] > 0.5f ? 0.0f : -tx; *x1 = *x0 + tx;
        *y0 = mc.mpos[1] > 0.5f ? 0.0f : -ty; *y1 = *y0 + ty;
    }
}

static void trace_alloc (void)
{
    float x0, y0, x1, y1;

    map_extent(&x0, &y0, &x1, &y1);
    if(trace && x0 == ex0 && y0 == ey0 && x1 == ex1 && y1 == ey1)
        return;

    free(trace);
    ex0 = x0; ey0 = y0; ex1 = x1; ey1 = y1;
    tw = (int)((ex1 - ex0) * TRACE_RES) + 1;
    th = (int)((ey1 - ey0) * TRACE_RES) + 1;
    trace = (uint8_t *)ps_calloc((size_t)tw * th, 1);
    burned = false;
    changed = true;
}

static void trace_clear (void)
{
    if(trace)
        memset(trace, 0, (size_t)tw * th);
    burned = false;
    changed = true;
}

static void trace_point (float x, float y)
{
    int cx = (int)((x - ex0) * TRACE_RES), cy = (int)((y - ey0) * TRACE_RES);

    if(cx < 0 || cy < 0 || cx >= tw || cy >= th)
        return;

    trace[cy * tw + cx] = 1;

    if(!burned) {
        bx0 = bx1 = x;
        by0 = by1 = y;
        burned = true;
    } else {
        bx0 = min(bx0, x); bx1 = max(bx1, x);
        by0 = min(by0, y); by1 = max(by1, y);
    }
}

static void trace_line (float xa, float ya, float xb, float yb)
{
    float dx = xb - xa, dy = yb - ya;
    int n = (int)(max(fabsf(dx), fabsf(dy)) * TRACE_RES) + 1;

    for(int i = 0; i <= n; i++)
        trace_point(xa + dx * i / n, ya + dy * i / n);

    changed = true;
}

void map_init (void)
{
    trace_alloc();
}

void map_record (void)
{
    if(mc.updated == last_update)
        return;
    last_update = mc.updated;

    if(mc.info)
        trace_alloc();
    if(trace == NULL)
        return;

    // a new job (idle -> cycle) starts with an empty map
    if(last_state == STATE_IDLE && (mc.state & STATE_CYCLE))
        trace_clear();
    last_state = mc.state;

    bool on = (mc.state & STATE_CYCLE) && (mc.flags & DL_FLAG_SPINDLE_ON);
    float x = mc.mpos[0], y = mc.mpos[1];

    if(on && last_on)
        trace_line(last_x, last_y, x, y);
    else if(on)
        trace_point(x, y);

    last_on = on;
    last_x = x;
    last_y = y;
    changed = true;     // the tool moves
}

bool map_changed (void)
{
    if(preview.seq != preview_seq) {
        preview_seq = preview.seq;
        changed = true;
    }

    return changed;
}

static void view_fit (map_view_t *v, int w, int h, float x0, float y0, float x1, float y1)
{
    float sx = w / (x1 - x0), sy = h / (y1 - y0);

    v->w = w;
    v->h = h;
    v->scale = min(sx, sy);
    v->x0 = (x0 + x1) / 2.0f - w / 2.0f / v->scale;
    v->y0 = (y0 + y1) / 2.0f - h / 2.0f / v->scale;
}

void map_view_full (map_view_t *v, int w, int h)
{
    float x0, y0, x1, y1;

    map_extent(&x0, &y0, &x1, &y1);
    view_fit(v, w, h, x0, y0, x1, y1);
}

void map_view_zoom (map_view_t *v, int w, int h)
{
    if(preview.valid)
        map_view_preview(v, w, h);
    else if(burned)
        view_fit(v, w, h, bx0 - ZOOM_MARGIN, by0 - ZOOM_MARGIN, bx1 + ZOOM_MARGIN, by1 + ZOOM_MARGIN);
    else
        view_fit(v, w, h, mc.mpos[0] - ZOOM_EMPTY, mc.mpos[1] - ZOOM_EMPTY, mc.mpos[0] + ZOOM_EMPTY, mc.mpos[1] + ZOOM_EMPTY);
}

void map_view_preview (map_view_t *v, int w, int h)
{
    if(preview.valid)
        view_fit(v, w, h, preview.x0 + wco_x() - PREVIEW_MARGIN, preview.y0 + wco_y() - PREVIEW_MARGIN,
                  preview.x1 + wco_x() + PREVIEW_MARGIN, preview.y1 + wco_y() + PREVIEW_MARGIN);
    else
        map_view_full(v, w, h);
}

bool map_to_px (const map_view_t *v, float mx, float my, int *px, int *py)
{
#if MAP_X_LEFT
    *px = v->w - 1 - (int)lroundf((mx - v->x0) * v->scale);
#else
    *px = (int)lroundf((mx - v->x0) * v->scale);
#endif
#if MAP_Y_DOWN
    *py = (int)lroundf((my - v->y0) * v->scale);
#else
    *py = v->h - 1 - (int)lroundf((my - v->y0) * v->scale);
#endif

    return *px >= 0 && *py >= 0 && *px < v->w && *py < v->h;
}

static inline uint16_t rgb565 (uint32_t c)
{
    return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F);
}

static inline uint32_t mix (uint32_t a, uint32_t b, float t)
{
    uint32_t r = 0;

    for(int s = 0; s < 24; s += 8) {
        float ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        r |= (uint32_t)(ca + (cb - ca) * t) << s;
    }

    return r;
}

void map_render (uint16_t *buf, const map_view_t *v, bool fine_grid)
{
    const uint16_t c_out = rgb565(COL_MAP_OUT), c_bg = rgb565(COL_MAP_BG);
    const uint16_t c_grid = rgb565(COL_MAP_GRID), c_grid2 = rgb565(COL_MAP_GRID2);
    const uint16_t c_work = rgb565(COL_MAP_WORK), c_todo = rgb565(COL_MAP_TODO), c_frame = rgb565(COL_MAP_FRAME);
    const float frame = 1.6f / v->scale;     // frame width, about 2 pixels
    uint16_t c_trace[16];

    for(int i = 0; i < 16; i++)
        c_trace[i] = rgb565(mix(COL_RING1, COL_RING2, i / 15.0f));

    float step = 1.0f / v->scale, grid = fine_grid ? 10.0f : 50.0f;
    float span = burned && by1 > by0 ? by1 - by0 : 1.0f;

    for(int py = 0; py < v->h; py++) {
#if MAP_Y_DOWN
        float my = v->y0 + py * step;
#else
        float my = v->y0 + (v->h - 1 - py) * step;
#endif
        int cy = (int)((my - ey0) * TRACE_RES);
        bool gy = fmodf(fabsf(my - ey0) + step / 2, grid) < step;
        bool gy50 = fmodf(fabsf(my - ey0) + step / 2, 50.0f) < step;
        uint16_t tc = c_trace[constrain((int)((my - by0) / span * 15.0f), 0, 15)];
        uint16_t *row = buf + py * v->w;

        for(int px = 0; px < v->w; px++) {
#if MAP_X_LEFT
            float mx = v->x0 + (v->w - 1 - px) * step;
#else
            float mx = v->x0 + px * step;
#endif
            int pv = preview_at(mx, my);    // the preview is drawn even outside the machine area

            if(mx < ex0 || mx > ex1 || my < ey0 || my > ey1) {
                row[px] = pv == 1 ? c_todo : (pv == 0 ? c_work : c_out);
                continue;
            }

            int cx = (int)((mx - ex0) * TRACE_RES);
            bool hit = false;

            // with fewer than one pixel per cell look at all cells under the pixel
            int n = step * TRACE_RES > 1.0f ? (int)(step * TRACE_RES) : 1;
            for(int j = 0; j < n && !hit; j++)
                for(int i = 0; i < n && !hit; i++) {
                    int x = cx + i, y = cy + j;
                    hit = x < tw && y < th && trace[y * tw + x];
                }

            if(hit)
                row[px] = tc;
            else if(pv == 1)
                row[px] = c_todo;
            else if(pv == 0)
                row[px] = c_work;
            else if(mx - ex0 < frame || ex1 - mx < frame || my - ey0 < frame || ey1 - my < frame)
                row[px] = c_frame;
            else {
                bool gx = fmodf(mx - ex0 + step / 2, grid) < step;
                bool gx50 = fmodf(mx - ex0 + step / 2, 50.0f) < step;
                row[px] = (gx50 || gy50) && fine_grid ? c_grid2 : (gx || gy ? c_grid : c_bg);
            }
        }
    }

    // G54 origin: red X and green Y arm
    int ox, oy;
    float wx = mc.mpos[0] - mc.wpos[0], wy = mc.mpos[1] - mc.wpos[1];
    int arm = fine_grid ? 26 : 14;

    if(map_to_px(v, wx, wy, &ox, &oy)) {
        for(int t = -1; t <= 1; t++) {
            for(int i = 0; i <= arm; i++) {
                int xi = MAP_X_LEFT ? ox - i : ox + i;     // X arm points to +X
                if(xi >= 0 && xi < v->w && oy + t >= 0 && oy + t < v->h)
                    buf[(oy + t) * v->w + xi] = rgb565(COL_X);
                int yi = MAP_Y_DOWN ? oy + i : oy - i;     // Y arm points to +Y
                if(yi >= 0 && yi < v->h && ox + t >= 0 && ox + t < v->w)
                    buf[yi * v->w + ox + t] = rgb565(COL_Y);
            }
        }
    }

    changed = false;
}

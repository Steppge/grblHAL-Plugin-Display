// Workpiece map: live trace of the tool path while the spindle/laser is on

#include "map.h"
#include "link.h"
#include "theme.h"

#define TRACE_RES       2       // trace cells per mm
#define ZOOM_MARGIN    10.0f    // mm around the burned area in the zoom view
#define ZOOM_EMPTY     40.0f    // mm around the tool when nothing has been burned yet

static uint8_t *trace = NULL;
static int tw = 0, th = 0;              // trace size in cells
static float ex0, ey0, ex1, ey1;        // machine extent the trace covers (mm)
static float bx0, by0, bx1, by1;        // bounds of the burned area (mm)
static bool burned = false, changed = true;
static bool last_on = false;
static float last_x, last_y;
static float last_wx, last_wy;         // G54 origin of the last status line
static float rx, ry;                    // tool position at the last render
static uint32_t last_update = 0, last_state = STATE_IDLE;
static uint16_t preview_seq = 0;

#define PREVIEW_MARGIN  5.0f    // mm around the G-code preview

// Work offset: the preview is in work coordinates, the map in machine coordinates.
static float wco_x (void) { return mc.mpos[0] - mc.wpos[0]; }
static float wco_y (void) { return mc.mpos[1] - mc.wpos[1]; }

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

    if(mc.info && !map_render_busy())  // a running render reads the trace, a new size waits for its end
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

    // a new render only for new content: the trace or the G54 origin (the tool cross is its own object);
    // a render takes about 200 ms and stops the status lines meanwhile
    if(on || wco_x() != last_wx || wco_y() != last_wy)
        changed = true;
    last_wx = wco_x();
    last_wy = wco_y();
}

bool map_zoom_moved (void)
{
    return !preview.valid && !burned && (mc.mpos[0] != rx || mc.mpos[1] != ry);
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
    v->d3 = false;
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
        view_fit(v, w, h, preview.x0 - preview_tool_r() + wco_x() - PREVIEW_MARGIN,
                  preview.y0 - preview_tool_r() + wco_y() - PREVIEW_MARGIN,
                  preview.x1 + preview_tool_r() + wco_x() + PREVIEW_MARGIN,
                  preview.y1 + preview_tool_r() + wco_y() + PREVIEW_MARGIN);
    else
        map_view_full(v, w, h);
}

void map_view_3d (map_view_t *v, int w, int h)
{
    v->w = w;
    v->h = h;
    v->x0 = v->y0 = 0.0f;
    v->scale = 1.0f;
    v->d3 = true;
}

bool map_to_px (const map_view_t *v, float mx, float my, int *px, int *py)
{
    if(v->d3)
        return false;       // the 3D view shows no machine positions

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

#define RENDER_W_MAX    1024    // widest render (the zoom view, full screen)

// grid and frame flags of one column or row, worked out once per render instead of for each pixel
#define G_GRID      0x01    // grid line (10 or 50 mm)
#define G_GRID50    0x02    // 50 mm line
#define G_FRAME     0x04    // frame of the machine area
#define G_OUT       0x08    // outside the machine area

static float col_mx[RENDER_W_MAX];
static int col_cx[RENDER_W_MAX];
static uint8_t col_g[RENDER_W_MAX];

static uint8_t grid_flags (float m, float e0, float e1, float step, float grid, float frame, bool known)
{
    uint8_t g = 0;
    float d = fabsf(m - e0) + step / 2;

    if(fmodf(d, grid) < step)
        g |= G_GRID;
    if(fmodf(d, 50.0f) < step)
        g |= G_GRID50;
    if(known && (m - e0 < frame || e1 - m < frame))
        g |= G_FRAME;
    if(known && (m < e0 || m > e1))
        g |= G_OUT;

    return g;
}

// Preview bit at a machine position: -1 outside the stock, 3 its frame, 0/1 the bitmap (0 also for the
// stock around the path); scale, offset and stock once per render
struct preview_map_t {
    bool valid;
    float wco_x, wco_y, s;
    float r, edge;      // tool radius (stock = path + r), frame width (mm)
};

static inline int preview_bit (const preview_map_t *p, float mx, float my)
{
    if(!p->valid)
        return -1;

    float wx = mx - p->wco_x, wy = my - p->wco_y;
    float sx0 = preview.x0 - p->r, sx1 = preview.x1 + p->r, sy0 = preview.y0 - p->r, sy1 = preview.y1 + p->r;
    if(wx < sx0 || wx > sx1 || wy < sy0 || wy > sy1)
        return -1;
    if(wx - sx0 < p->edge || sx1 - wx < p->edge || wy - sy0 < p->edge || sy1 - wy < p->edge)
        return 3;
    if(wx < preview.x0 || wx > preview.x1 || wy < preview.y0 || wy > preview.y1)
        return 0;

    int px = (int)((wx - preview.x0) * p->s), py = (int)((preview.y1 - wy) * p->s);
    if(px < 0 || py < 0 || px >= preview.w || py >= preview.h)
        return 0;

    return (preview.bits[py * (preview.w / 8) + px / 8] >> (7 - (px & 7))) & 1;
}

#if PREVIEW_3D

static inline void fill_col (uint16_t *buf, int w, int h, int x0, int x1, int y0, int y1, uint16_t c)
{
    if(y0 < 0) y0 = 0;
    if(y1 > h) y1 = h;
    if(x0 < 0) x0 = 0;
    if(x1 > w) x1 = w;
    for(int y = y0; y < y1; y++) {
        uint16_t *p = buf + y * w;
        for(int x = x0; x < x1; x++)
            p[x] = c;
    }
}

#define TOOL3D_LEN      30.0f   // shaft length drawn (mm)

static struct {
    bool on;
    float x, y, z, d;
} tool3d;

void map_3d_tool (bool on, float wx, float wy, float wz, float d)
{
    tool3d = { on, wx, wy, wz, d };
}

// The tool as a cylinder in the same oblique view as the workpiece: round bottom edge (front half of the
// tip circle), shaded body with flutes at the lower end, the top face as a full ellipse.
// x, y: center of the tip; tw: width in pixels, eh: height of the tip ellipse, th: shaft length in pixels.
static void draw_tool (uint16_t *buf, int w, int h, int x, int y, int tw, int eh, int th, int flute)
{
    float r = tw / 2.0f, a = eh / 2.0f;
    int ytop = y - th;

    for(int i = 0; i < tw; i++) {
        float n = (i + 0.5f - r) / r;                       // -1 left edge .. 1 right edge
        float e = a * sqrtf(max(0.0f, 1.0f - n * n));       // ellipse half-height in this column
        float light = constrain(0.25f + 0.75f * (1.0f - fabsf(n + 0.35f)), 0.0f, 1.0f);   // light from the left
        uint16_t c_body = rgb565(mix(0x003848, COL_RING1, light));
        uint16_t c_flute = rgb565(mix(0x001820, mix(0x003848, COL_RING1, light), 0.45f));
        int px = x - (int)r + i, yb = y + (int)lroundf(e), yf = y - flute;
        // shank down to the flutes, then the flutes with dark helix grooves
        fill_col(buf, w, h, px, px + 1, ytop, yf, c_body);
        for(int yy = max(yf, ytop); yy < yb; yy++) {
            bool groove = ((yy - y + (int)lroundf(n * r * 1.2f)) % max(tw / 2, 6) + max(tw / 2, 6)) % max(tw / 2, 6) < max(tw / 8, 1);
            fill_col(buf, w, h, px, px + 1, yy, yy + 1, groove ? c_flute : c_body);
        }
        // top face
        uint16_t c_top = rgb565(mix(COL_RING1, 0xFFFFFF, 0.35f + 0.25f * (1.0f - fabsf(n))));
        fill_col(buf, w, h, px, px + 1, ytop - (int)lroundf(e), ytop + (int)lroundf(e), c_top);
    }
}

// Oblique view from the front: the depth map as columns, back rows first, so the rows in front cover
// what lies behind them. The material top is at max(z1, 0), the slab ends a bit below the deepest cut.
static void render_3d (uint16_t *buf, const map_view_t *v)
{
    const uint16_t c_bg = rgb565(COL_MAP_BG);
    const float cy = 0.6f, sz = 0.8f;       // depth axis foreshortened, Z height drawn at 0.8
    int w = v->w, h = v->h;

    for(int i = 0; i < w * h; i++)
        buf[i] = c_bg;

    int dw = preview.hw, dh = preview.hh;
    if(!preview.has_depth || dw < 2 || dh < 2)
        return;

    float ztop = max(preview.z1, 0.0f), zbot = preview.z0 - max(2.0f, (ztop - preview.z0) * 0.1f);
    float zr = ztop - zbot, mmw = preview.hx1 - preview.hx0, mmh = preview.hy1 - preview.hy0;
    float pxmm = min((w - 40) / mmw, (h - 70) / (mmh * cy + zr * sz));
    float s = pxmm * mmw / dw, sv = pxmm * mmh / dh * cy, zpx = pxmm * sz;
    float ox = (w - dw * s) / 2.0f, oy = (h - (dh * sv + zr * zpx)) / 2.0f + 20.0f;

    // the tool: drawn after the rows behind its center, the rows in front cover what is below their top
    int t_row = -2, tx = 0, ty = 0, tw = 0, th = 0, eh = 0, fl = 0;
    if(tool3d.on) {
        float col = (tool3d.x - preview.hx0) / mmw * dw, row = (preview.hy1 - tool3d.y) / mmh * dh;
        float u = MAP_X_LEFT ? dw - col : col, vr = MAP_Y_DOWN ? dh - row : row;
        tx = (int)lroundf(ox + u * s);
        ty = (int)lroundf(oy + vr * sv + (ztop - tool3d.z) * zpx);
        tw = max((int)lroundf(tool3d.d * pxmm), 6);
        th = max((int)lroundf(TOOL3D_LEN * zpx), 30);
        eh = max((int)lroundf(tw * cy), 3);                 // circle seen at the same tilt as the workpiece
        fl = min((int)lroundf(tool3d.d * 2.0f * zpx), th * 2 / 3);   // flutes: twice the diameter
        // after the rows under its footprint: the material there is not higher than the tip, only rows
        // further in front may cover it
        t_row = constrain((int)floorf(vr + tool3d.d * 0.5f / mmh * dh), -1, dh - 1);
    }
    if(t_row == -1)
        draw_tool(buf, w, h, tx, ty, tw, eh, th, fl);

    // per level: height below the top in pixels, colour of the top face and of the walls
    int dz[16];
    uint16_t c_top[16], c_wall[16];
    for(int l = 0; l < 16; l++) {
        float z = l == 0 ? ztop : preview.z1 - (l - 1) / 14.0f * (preview.z1 - preview.z0);
        float t = (ztop - z) / zr;
        dz[l] = (int)lroundf((ztop - z) * zpx);
        uint32_t top = mix(0xB4BAD8, 0x2A2C4C, t);
        c_top[l] = rgb565(top);
        c_wall[l] = rgb565(mix(top, 0x000000, 0.45f));
    }
    int ybase = (int)lroundf(zr * zpx);

    // the stock in cells (the depth map has 1 mm more around it): -1 outside
    float r = preview_tool_r();
    float c_lo = (preview.x0 - r - preview.hx0) / mmw * dw, c_hi = (preview.x1 + r - preview.hx0) / mmw * dw;
    float r_lo = (preview.hy1 - (preview.y1 + r)) / mmh * dh, r_hi = (preview.hy1 - (preview.y0 - r)) / mmh * dh;
    const uint16_t c_edge = rgb565(COL_MAP_STOCK);

    auto level = [&](int vr, int u) -> int {
        if(vr < 0 || vr >= dh || u < 0 || u >= dw)
            return -1;
        int row = MAP_Y_DOWN ? dh - 1 - vr : vr, col = MAP_X_LEFT ? dw - 1 - u : u;
        if(col + 0.5f < c_lo || col + 0.5f > c_hi || row + 0.5f < r_lo || row + 0.5f > r_hi)
            return -1;
        uint8_t b = preview.depth[(row * dw + col) >> 1];
        return col & 1 ? b & 0x0F : b >> 4;
    };

    for(int vr = 0; vr < dh; vr++) {
        float yrow = oy + vr * sv;
        int t0 = (int)lroundf(yrow), t1 = (int)lroundf(yrow + sv);
        if(t1 <= t0)
            t1 = t0 + 1;
        for(int u = 0; u < dw; u++) {
            int x0 = (int)(ox + u * s), x1 = (int)(ox + (u + 1) * s);
            if(x1 <= x0)
                x1 = x0 + 1;
            int l = level(vr, u);
            if(l < 0)
                continue;
            int yt = t0 + dz[l];
            // the wall down to the top of the cell in front, which covers the rest; the front row down to the base
            int lf = level(vr + 1, u);
            int yw = lf >= 0 ? (int)lroundf(oy + (vr + 1) * sv) + dz[lf] : t0 + ybase + (t1 - t0);
            // uncut cells at the edge of the stock: light frame
            bool edge = l == 0 && (lf < 0 || level(vr - 1, u) < 0 || level(vr, u - 1) < 0 || level(vr, u + 1) < 0);
            fill_col(buf, w, h, x0, x1, yt, yt + (t1 - t0), edge ? c_edge : c_top[l]);
            if(yw > yt + (t1 - t0))
                fill_col(buf, w, h, x0, x1, yt + (t1 - t0), yw, c_wall[l]);
        }
        if(vr == t_row)
            draw_tool(buf, w, h, tx, ty, tw, eh, th, fl);
    }
}

#endif

static void render (uint16_t *buf, const map_view_t *v, bool fine_grid)
{
#if PREVIEW_3D
    if(v->d3) {
        render_3d(buf, v);
        return;
    }
#endif
    const uint16_t c_out = rgb565(COL_MAP_OUT), c_bg = rgb565(COL_MAP_BG);
    const uint16_t c_grid = rgb565(COL_MAP_GRID), c_grid2 = rgb565(COL_MAP_GRID2);
    const uint16_t c_work = rgb565(COL_MAP_WORK), c_todo = rgb565(COL_MAP_TODO), c_frame = rgb565(COL_MAP_FRAME);
    const uint16_t c_out_grid = rgb565(mix(COL_MAP_OUT, COL_MAP_GRID, 0.5f));     // grid outside the machine area
    const float frame = 1.6f / v->scale;     // frame width, about 2 pixels
    uint16_t c_trace[16];

    for(int i = 0; i < 16; i++)
        c_trace[i] = rgb565(mix(COL_RING1, COL_RING2, i / 15.0f));

    float step = 1.0f / v->scale, grid = fine_grid ? 10.0f : 50.0f;
    float span = burned && by1 > by0 ? by1 - by0 : 1.0f;
    bool known = mc.info || mc.env_valid;   // machine size from the controller: frame and outside area,
                                            // else only the grid (the 300 mm default would draw a wrong frame)
    int w = min(v->w, RENDER_W_MAX);
    // with fewer than one pixel per cell look at all cells under the pixel
    int n = step * TRACE_RES > 1.0f ? (int)(step * TRACE_RES) : 1;

    preview_map_t pm = { preview.valid, wco_x(), wco_y(), 0.0f, preview_tool_r(), 1.6f / v->scale };
    const uint16_t c_stock = rgb565(COL_MAP_STOCK);
    if(pm.valid) {
        // same scale as on the controller (display_link.c, pass 1)
        float dx = max(preview.x1 - preview.x0, 0.1f), dy = max(preview.y1 - preview.y0, 0.1f);
        pm.s = min((PREVIEW_W_MAX - 1) / dx, (PREVIEW_H_MAX - 1) / dy);
    }

    for(int px = 0; px < w; px++) {
#if MAP_X_LEFT
        float mx = v->x0 + (v->w - 1 - px) * step;
#else
        float mx = v->x0 + px * step;
#endif
        col_mx[px] = mx;
        col_cx[px] = (int)((mx - ex0) * TRACE_RES);
        col_g[px] = grid_flags(mx, ex0, ex1, step, grid, frame, known);
    }

    for(int py = 0; py < v->h; py++) {
#if MAP_Y_DOWN
        float my = v->y0 + py * step;
#else
        float my = v->y0 + (v->h - 1 - py) * step;
#endif
        int cy = (int)((my - ey0) * TRACE_RES);
        uint8_t gy = grid_flags(my, ey0, ey1, step, grid, frame, known);
        bool row_in = !(gy & G_OUT);
        uint16_t tc = c_trace[constrain((int)((my - by0) / span * 15.0f), 0, 15)];
        uint16_t *row = buf + py * v->w;

        for(int px = 0; px < w; px++) {
            uint8_t gx = col_g[px];
            int pv = preview_bit(&pm, col_mx[px], my);    // the preview is drawn even outside the machine area

            if((gx | gy) & G_OUT) {
                row[px] = pv == 1 ? c_todo : (pv == 0 ? c_work : (pv == 3 ? c_stock : ((gx | gy) & G_GRID ? c_out_grid : c_out)));
                continue;
            }

            bool hit = false;
            if(row_in && trace) {
                int cx = col_cx[px];
                for(int j = 0; j < n && !hit; j++)
                    for(int i = 0; i < n && !hit; i++) {
                        int x = cx + i, y = cy + j;
                        hit = x >= 0 && y >= 0 && x < tw && y < th && trace[y * tw + x];
                    }
            }

            if(hit)
                row[px] = tc;
            else if(pv == 1)
                row[px] = c_todo;
            else if(pv == 3)
                row[px] = c_stock;
            else if(pv == 0)
                row[px] = c_work;
            else if((gx | gy) & G_FRAME)
                row[px] = c_frame;
            else
                row[px] = ((gx | gy) & G_GRID50) && fine_grid ? c_grid2 : ((gx | gy) & G_GRID ? c_grid : c_bg);
        }
    }

    // G54 origin: red X and green Y arm
    int ox, oy;
    float wx = wco_x(), wy = wco_y();
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
}

// what is drawn from now on: changes after this point lead to the next render
static void render_begin (void)
{
    rx = mc.mpos[0];
    ry = mc.mpos[1];
    changed = false;
}

void map_render (uint16_t *buf, const map_view_t *v, bool fine_grid)
{
    render_begin();
    render(buf, v, fine_grid);
}

// Background render

static TaskHandle_t r_task = NULL;
static volatile int r_state = 0;        // 0 idle, 1 running, 2 done
static uint16_t *r_buf;
static const uint16_t *r_shown;
static map_view_t r_v;
static map_area_t r_area;
static bool r_fine;
static int r_tag;

// the smallest rectangle in which the new render differs from the shown one
static void diff (const uint16_t *a, const uint16_t *b, int w, int h, map_area_t *r)
{
    r->x1 = w; r->y1 = h; r->x2 = -1; r->y2 = -1;

    if(b == NULL) {
        r->x1 = r->y1 = 0; r->x2 = w - 1; r->y2 = h - 1;
        return;
    }

    for(int y = 0; y < h; y++) {
        const uint16_t *ra = a + y * w, *rb = b + y * w;
        if(!memcmp(ra, rb, w * 2))
            continue;
        if(y < r->y1)
            r->y1 = y;
        r->y2 = y;
        int x = 0;
        while(x < w && ra[x] == rb[x])
            x++;
        if(x < r->x1)
            r->x1 = x;
        x = w - 1;
        while(x >= 0 && ra[x] == rb[x])
            x--;
        if(x > r->x2)
            r->x2 = x;
    }
}

static void render_job (void)
{
    render(r_buf, &r_v, r_fine);
    diff(r_buf, r_shown, r_v.w, r_v.h, &r_area);
}

static void render_task (void *arg)
{
    for(;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        render_job();
        __atomic_store_n(&r_state, 2, __ATOMIC_RELEASE);
    }
}

bool map_render_start (uint16_t *buf, const uint16_t *shown, const map_view_t *v, bool fine_grid, int tag)
{
    if(__atomic_load_n(&r_state, __ATOMIC_ACQUIRE) != 0)
        return false;

    render_begin();
    r_buf = buf;
    r_shown = shown;
    r_v = *v;
    r_fine = fine_grid;
    r_tag = tag;

    // core 0 like the USB link task, that one has the higher priority
    if(r_task == NULL && xTaskCreatePinnedToCore(render_task, "map", 4096, NULL, 1, &r_task, 0) != pdPASS) {
        r_task = NULL;
        render_job();       // no task: here and now
        r_state = 2;
        return true;
    }

    __atomic_store_n(&r_state, 1, __ATOMIC_RELEASE);
    xTaskNotifyGive(r_task);

    return true;
}

int map_render_done (map_view_t *v, map_area_t *changed_area)
{
    if(__atomic_load_n(&r_state, __ATOMIC_ACQUIRE) != 2)
        return -1;

    *v = r_v;
    *changed_area = r_area;
    __atomic_store_n(&r_state, 0, __ATOMIC_RELEASE);

    return r_tag;
}

bool map_render_busy (void)
{
    return __atomic_load_n(&r_state, __ATOMIC_ACQUIRE) == 1;
}

void map_render_wait (void)
{
    while(map_render_busy())
        delay(1);
}

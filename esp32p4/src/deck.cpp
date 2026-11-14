// PC-Deck mode, "Big-Deck": the display works as a deck of the PC-Deck app, with the protocol of the deck firmware
// (F:\VisualCode\PC-Deck\pcdeck\firmware, FW 37). The app sends the layout (pages, tiles, 64x64 icons),
// the deck shows it and reports taps, page changes and the encoder.
//
// Channels: the console UART (USB-C "UART", CH340, 921600 baud) and TCP in the home network after the
// mutual HMAC login with the key from the pairing over USB. Answers go to the channel of the last line.
//
// Big-Deck only (1024x600): a vertical volume slider at the right edge.
//   Deck -> PC: VOL <0..100>   while dragging (at most every 40 ms) and on release
//   PC -> Deck: VOL <0..100>   moves the slider to the volume of the PC (not while a finger is on it)
//
// Since FW 38: designs (THEME, page image IMG page 470x270 RGB565 little endian, shown 2x over the tile area),
// dimming with a big clock (OPT, TIME) and the build effect. The boot image (IMG boot) is taken and thrown
// away, the block world of Deck 1/2 is not drawn. Not: firmware updates through the app (FWINFO: no space).

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <vector>
#include "mbedtls/md.h"
#include "src/widgets/slider/lv_slider_private.h"     // knob area for the fader grooves
#include "pins.h"
#include "wlan.h"
#include "display.h"
#include "link.h"
#include "deck.h"
#include "battery.h"

LV_FONT_DECLARE(font_clock_120)

#define DECK_FW         "40"
#define DECK_W          1024
#define DECK_H          600
#define NET_PORT        5555
#define DISC_PORT       5556
#define PC_TIMEOUT_MS   5000    // no line from the app for this long: not connected (USB)
#define NET_TIMEOUT_MS 12000    // the same over WiFi
#define USB_DATA_MS     3000    // the app sends a PING every second over USB
#define LONG_MS          600    // long tap: yellow frame, LONG on release
#define DELETE_MS       1500    // red frame and the question "Delete tile?"
#define KNOB_LONG_MS    1500
#define ENC_SEND_MS       25
#define OSD_MS          1200

#define ICON_SZ         64
#define ICON_BYTES      (ICON_SZ * ICON_SZ * 2)
#define MAX_PAGES       8
#define MAX_COLS        6
#define MAX_ROWS        4
#define MAX_TILES       (MAX_COLS * MAX_ROWS)
#define NAV_H           56
#define GAP             10
#define VOL_W           84      // volume slider at the right edge
#define GRID_W          (DECK_W - VOL_W)
#define VOL_SEND_MS     40
#define FADER_TRACK     8       // volume fader like on a mixing desk: dark slot, glowing level, scale marks,
#define FADER_LEN       420     // silver saddle cap 46 x 64 (longer than wide) with ribs and a white index line
#define FADER_W         46
#define FADER_H         64
#define FADER_TOP       0xD9DCE3    // silver saddle cap: base gradient ...
#define FADER_BOT       0x7C818C
#define FADER_RIDGE     0xE6E8ED    // raised ends: lit outer edge ...
#define FADER_RIDGE2    0xA9ADB6    // ... towards the hollow
#define FADER_HOLLOW    0x80848E    // hollow: in the shadow of the upper end ...
#define FADER_SHINE     0xC6C9D0    // ... and its lit side
#define FADER_RIB       0x3A3D46
#define FADER_END       18          // height of each raised end

#define COL_BG      0x121216
#define COL_TILE    0x2C303A
#define COL_TEXT    0xEBEBF0
#define COL_DIM     0x82879A
#define COL_ACCENT  0x5096FF
#define COL_LONG    0xEB5050
#define COL_WARN    0xF0BE3C
#define COL_FULL    0x2CF086    // battery really full (the green of the CNC theme)

#define LAYOUT_FILE "/deck_layout.txt"
#define THEME_FILE  "/theme.txt"
#define PAGE_FILE   "/page.bin"
#define PAGE_W      470         // page image: half the tile area (940x540), shown 2x
#define PAGE_H      270
#define PAGE_BYTES  (PAGE_W * PAGE_H * 2)
#define KEY565      0x2987      // RGB(44,48,58): the app draws the icons on this colour, see-through on a design
#define TILE_BYTES  (ICON_BYTES + ICON_SZ * ICON_SZ)    // RGB565 + alpha plane

struct theme_t {
    bool on = false;
    String hash = "none";
    uint16_t tile_top = 0, tile_bot = 0, border_top = 0, border_bot = 0;     // RGB565
    uint8_t style = 3;          // 0 flat, 1 vertical, 2 diagonal (drawn vertical), 3 glass
    uint8_t alpha = 204;        // opacity of the tiles
};

struct tile_t {
    uint8_t page, idx;
    bool icon;
    String label;
};

struct live_t {
    bool used, icon;
    String label;
    uint8_t *buf;       // icon as received (raw or packed)
    int len, cap;
};

static void (*console_cb)(char *line) = NULL;
static String deck_id, net_key;
static Preferences prefs;
static bool mode_on = false;

// layout
static std::vector<tile_t> tiles, pending;
static int g_cols = 0, g_rows = 0, g_pages = 0, cur_page = 0;
static String g_hash = "none";
static int p_cols, p_rows, p_pages;
static String p_hash;
static String page_title[MAX_PAGES];
static live_t live[MAX_TILES];

// channel
static Print *io = &Serial;
static Stream *in = &Serial;
static bool from_net = false, via_net = false;
static uint32_t last_pc = 0, last_serial = 0;
static bool pc_seen = false;

// network
static WiFiServer net_server(NET_PORT);
static WiFiClient net_client;
static WiFiUDP disc;
static bool net_started = false, net_auth = false;
static String net_nonce;
static uint32_t net_last = 0;

// buffers
static uint16_t *icon_buf;              // one unpacked icon
static uint8_t *z_buf;                  // packed data while receiving
static uint8_t *blk;                    // 4 KB blocks (images, refused)

// user interface
static lv_obj_t *page_obj, *grid, *nav, *nav_title, *wait_box, *wait_lbl, *ask_box, *osd_box, *osd_lbl, *osd_bar;
static lv_obj_t *tile_obj[MAX_TILES], *tile_img[MAX_TILES], *tile_lbl[MAX_TILES];
static lv_image_dsc_t tile_dsc[MAX_TILES];
static int area_y, area_h;
static int down_tile = -1, ask_tile = -1;
static uint32_t down_since = 0;
static bool long_sent = false, delete_armed = false;
static bool dragging = false;           // long held stored tile is dragged to another place (MOVE, FW 40)
static int drag_target = -1;            // place under the finger, framed also when empty
static lv_point_t down_pt;
static lv_obj_t *drag_box;              // accent frame on the target place
static uint32_t osd_until = 0;
static lv_obj_t *vol_box, *vol_slider, *vol_lbl;
static int vol_sent = -1;
static uint32_t vol_at = 0;
static bool drawn_ok = false;

// designs
static theme_t theme;
static lv_obj_t *page_img, *tile_shine[MAX_TILES];
static lv_image_dsc_t page_dsc;
static File img_file;
static unsigned img_left = 0;
static bool img_discard = false;

// display options (OPT) and dimming
static bool opt_build = false, opt_clock = false;
static uint32_t dim_ms = 0;
static uint8_t dim_bri = 25;
static bool dimmed = false, wake_hold = false;
static uint32_t enc_active = 0;
static lv_obj_t *wake_obj, *clock_lbl, *date_lbl, *batt_lbl;
static int clock_min = -1;
static uint8_t cnc_dim_min = 0;     // CNC/laser screen: dim after this many minutes without a job, 0 = never

// ---------------------------------------------------------------- helpers

static String from_hex (const char *h)
{
    String out;
    for(; h[0] && h[1]; h += 2) {
        char b[3] = { h[0], h[1], 0 };
        out += (char)strtol(b, NULL, 16);
    }
    return out;
}

static String to_hex (const String &s)
{
    static const char *d = "0123456789abcdef";
    String out;
    for(size_t i = 0; i < s.length(); i++) {
        out += d[(uint8_t)s[i] >> 4];
        out += d[(uint8_t)s[i] & 15];
    }
    return out;
}

static String hmac_hex (const String &key, const String &text)
{
    uint8_t out[32];
    static const char *d = "0123456789abcdef";
    String h;

    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)key.c_str(), key.length(),
                    (const uint8_t *)text.c_str(), text.length(), out);
    for(int i = 0; i < 32; i++) {
        h += d[out[i] >> 4];
        h += d[out[i] & 15];
    }
    return h;
}

static bool same_text (const String &a, const String &b)
{
    if(a.length() != b.length())
        return false;
    uint8_t diff = 0;
    for(size_t i = 0; i < a.length(); i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

// exactly n bytes of raw data after a command line, at most ms without new data
static size_t read_exact (uint8_t *buf, size_t n, uint32_t ms = 3000)
{
    size_t got = 0;
    uint32_t t0 = millis();

    while(got < n && millis() - t0 < ms) {
        int a = in->available();
        if(a > 0) {
            got += in->readBytes(buf + got, min((size_t)a, n - got));
            t0 = millis();
        } else
            delay(1);
    }
    return got;
}

static String icon_path (int p, int i)
{
    return "/i" + String(p) + "_" + String(i) + ".bin";
}

// packed icons: control byte c; c & 0x80: (c & 0x7F) + 1 times the next pixel, else c + 1 pixels as they are
static bool unpack_icon (const uint8_t *src, size_t n, uint16_t *dst)
{
    size_t o = 0, i = 0;
    const size_t total = ICON_SZ * ICON_SZ;

    while(i < n && o < total) {
        uint8_t c = src[i++];
        size_t cnt = (c & 0x7F) + 1;
        if(o + cnt > total)
            return false;
        if(c & 0x80) {
            if(i + 2 > n)
                return false;
            uint16_t v = src[i] | (src[i + 1] << 8);
            i += 2;
            while(cnt--)
                dst[o++] = v;
        } else {
            if(i + 2 * cnt > n)
                return false;
            while(cnt--) {
                dst[o++] = src[i] | (src[i + 1] << 8);
                i += 2;
            }
        }
    }
    return o == total && i == n;
}

static bool icon_from (const uint8_t *data, size_t n, uint16_t *dst)
{
    if(n == ICON_BYTES) {
        memcpy(dst, data, ICON_BYTES);
        return true;
    }
    return n < ICON_BYTES && unpack_icon(data, n, dst);
}

static bool load_icon (int p, int i, uint16_t *dst)
{
    File f = LittleFS.open(icon_path(p, i), "r");
    if(!f)
        return false;
    size_t n = f.size();
    bool ok = n > 0 && n <= ICON_BYTES && f.read(z_buf, n) == n;
    f.close();
    return ok && icon_from(z_buf, n, dst);
}

static lv_color_t c565 (uint16_t c)
{
    return lv_color_make((c >> 11) << 3 | (c >> 13), ((c >> 5) & 0x3F) << 2 | ((c >> 9) & 3), (c & 0x1F) << 3 | ((c >> 2) & 7));
}

// alpha plane of a tile icon: the key colour is see-through (only seen on a design: plain tiles have that colour)
static void icon_alpha (uint8_t *data)
{
    const uint16_t *px = (const uint16_t *)data;
    uint8_t *a = data + ICON_BYTES;
    for(int k = 0; k < ICON_SZ * ICON_SZ; k++)
        a[k] = px[k] == KEY565 ? 0 : 255;
}

static const tile_t *find_tile (int p, int i)
{
    for(auto &t : tiles)
        if(t.page == p && t.idx == i)
            return &t;
    return NULL;
}

static void clear_live (void)
{
    for(auto &l : live)
        l.used = false;
}

static void clear_store (void)
{
    File root = LittleFS.open("/");
    std::vector<String> names;
    for(File f = root.openNextFile(); f; f = root.openNextFile()) {
        String n = f.name();
        if(n.startsWith("i") && n.endsWith(".bin"))
            names.push_back("/" + n);
        f.close();
    }
    root.close();
    for(auto &n : names)
        LittleFS.remove(n);
}

static void save_layout (void)
{
    File f = LittleFS.open(LAYOUT_FILE, "w");
    if(!f)
        return;
    f.printf("%d %d %d %s\n", g_cols, g_rows, g_pages, g_hash.c_str());
    for(auto &t : tiles)
        f.printf("%u %u %d %s\n", t.page, t.idx, t.icon ? 1 : 0, t.label.c_str());
    f.close();
}

static void load_layout (void)
{
    File f = LittleFS.open(LAYOUT_FILE, "r");
    if(!f)
        return;
    char hash[64] = "";
    String l = f.readStringUntil('\n');
    if(sscanf(l.c_str(), "%d %d %d %63s", &g_cols, &g_rows, &g_pages, hash) == 4) {
        g_hash = hash;
        while(f.available()) {
            l = f.readStringUntil('\n');
            int p, i, ic, off = 0;
            if(sscanf(l.c_str(), "%d %d %d %n", &p, &i, &ic, &off) >= 3) {
                tile_t t { (uint8_t)p, (uint8_t)i, ic != 0, off > 0 ? l.substring(off) : String("") };
                t.label.trim();
                tiles.push_back(t);
            }
        }
    } else
        g_cols = g_rows = g_pages = 0;
    f.close();
}

// ---------------------------------------------------------------- drawing

static bool pc_ok (void)
{
    return pc_seen && millis() - last_pc < (via_net ? NET_TIMEOUT_MS : PC_TIMEOUT_MS);
}

static int grid_h (void)
{
    return g_pages > 1 ? area_h - NAV_H : area_h;
}

static void tile_frame (int i, bool pressed, bool is_long)
{
    lv_obj_t *o = tile_obj[i];
    if(pressed) {
        lv_obj_set_style_border_width(o, 4, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(delete_armed ? COL_LONG : is_long ? COL_WARN : COL_ACCENT), 0);
    } else {
        lv_obj_set_style_border_width(o, theme.on ? 2 : 0, 0);
        lv_obj_set_style_border_color(o, c565(theme.border_top), 0);
    }
}

// tiles, page bars and the page image after a THEME or IMGEND
static void apply_theme (void)
{
    if(!grid)
        return;
    bool img = theme.on && page_dsc.data;
    lv_obj_update_flag(page_img, LV_OBJ_FLAG_HIDDEN, !img);
    if(img) {
        lv_image_cache_drop(&page_dsc);
        lv_image_set_src(page_img, &page_dsc);
    }
    lv_obj_set_style_bg_opa(grid, img ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(nav, img ? LV_OPA_TRANSP : LV_OPA_COVER, 0);

    for(int i = 0; i < MAX_TILES; i++) {
        lv_obj_t *t = tile_obj[i];
        if(theme.on) {
            lv_obj_set_style_bg_color(t, c565(theme.tile_top), 0);
            lv_obj_set_style_bg_grad_color(t, c565(theme.style ? theme.tile_bot : theme.tile_top), 0);
            lv_obj_set_style_bg_grad_dir(t, theme.style ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE, 0);
            lv_obj_set_style_bg_opa(t, img ? theme.alpha : LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(t, lv_color_hex(COL_TILE), 0);
            lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_NONE, 0);
            lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        }
        lv_obj_update_flag(tile_shine[i], LV_OBJ_FLAG_HIDDEN, !(theme.on && theme.style == 3));
        tile_frame(i, false, false);
    }
}

static bool parse_theme (const char *line)
{
    char hash[40] = { 0 };
    unsigned t1, t2, b1, b2, st, al;
    if(sscanf(line, "%39s %x %x %x %x %u %u", hash, &t1, &t2, &b1, &b2, &st, &al) != 7)
        return false;
    theme.hash = hash;
    theme.tile_top = t1;
    theme.tile_bot = t2;
    theme.border_top = b1;
    theme.border_bot = b2;
    theme.style = min(st, 3u);
    theme.alpha = min(al, 255u);
    theme.on = true;
    return true;
}

static void load_page_image (void)
{
    File f = LittleFS.open(PAGE_FILE, "r");
    bool ok = f && f.size() == PAGE_BYTES;
    if(ok) {
        if(!page_dsc.data)
            page_dsc.data = (const uint8_t *)ps_malloc(PAGE_BYTES);
        ok = page_dsc.data && f.read((uint8_t *)page_dsc.data, PAGE_BYTES) == PAGE_BYTES;
    }
    if(f)
        f.close();
    if(!ok && page_dsc.data) {
        free((void *)page_dsc.data);
        page_dsc.data = NULL;
    }
}

static void load_theme (void)
{
    theme = theme_t();
    File f = LittleFS.open(THEME_FILE, "r");
    if(f) {
        String line = f.readStringUntil('\n');
        f.close();
        parse_theme(line.c_str());
    }
    if(theme.on)
        load_page_image();
}

static void draw_tile (int i)
{
    if(i < 0 || i >= MAX_TILES || !tile_obj[i])
        return;

    const tile_t *t = find_tile(cur_page, i);
    live_t &l = live[i];
    bool used = l.used || t;
    bool icon = false;
    const char *text = "";

    if(l.used) {
        text = l.label.c_str();
        icon = l.icon && l.buf && icon_from(l.buf, l.len, (uint16_t *)tile_dsc[i].data);
        if(icon)
            icon_alpha((uint8_t *)tile_dsc[i].data);
    } else if(t) {
        text = t->label.c_str();
        icon = t->icon && load_icon(cur_page, i, (uint16_t *)tile_dsc[i].data);
        if(icon)
            icon_alpha((uint8_t *)tile_dsc[i].data);
    }

    if(!used) {
        lv_obj_add_flag(tile_obj[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(tile_obj[i], LV_OBJ_FLAG_HIDDEN);

    if(icon) {
        lv_image_cache_drop(&tile_dsc[i]);
        lv_image_set_src(tile_img[i], &tile_dsc[i]);
        lv_obj_remove_flag(tile_img[i], LV_OBJ_FLAG_HIDDEN);
    } else
        lv_obj_add_flag(tile_img[i], LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(tile_lbl[i], text);
    lv_obj_align(tile_lbl[i], icon ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_CENTER, 0, icon ? -6 : 0);
    lv_obj_set_style_text_font(tile_lbl[i], icon ? &lv_font_montserrat_18 : &lv_font_montserrat_24, 0);
    tile_frame(i, i == down_tile, long_sent);
    lv_obj_invalidate(tile_obj[i]);
}

static void place_tiles (void)
{
    int cols = max(g_cols, 1), rows = max(g_rows, 1);
    int gh = grid_h();
    int tw = (GRID_W - GAP * (cols + 1)) / cols, th = (gh - GAP * (rows + 1)) / rows;
    int label_h = 30;
    int isz = min(tw - 16, th - label_h - 12);
    int scale = isz >= ICON_SZ * 2 ? 512 : isz >= ICON_SZ * 3 / 2 ? 384 : 256;     // whole steps stay sharp

    lv_obj_set_size(grid, GRID_W, gh);
    for(int i = 0; i < MAX_TILES; i++) {
        int c = i % cols, r = i / cols;
        if(i >= cols * rows) {
            lv_obj_add_flag(tile_obj[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_pos(tile_obj[i], GAP + c * (tw + GAP), GAP + r * (th + GAP));
        lv_obj_set_size(tile_obj[i], tw, th);
        lv_image_set_scale(tile_img[i], scale);
        lv_obj_align(tile_img[i], LV_ALIGN_CENTER, 0, -label_h / 2);
        lv_obj_set_width(tile_lbl[i], tw - 12);
        lv_obj_set_size(tile_shine[i], tw, th / 2);
    }
}

static void draw_nav (void)
{
    if(g_pages <= 1) {
        lv_obj_add_flag(nav, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(nav, LV_OBJ_FLAG_HIDDEN);
    char buf[48];
    if(page_title[cur_page].length())
        snprintf(buf, sizeof(buf), "%s", page_title[cur_page].c_str());
    else
        snprintf(buf, sizeof(buf), "%d / %d", cur_page + 1, g_pages);
    lv_label_set_text(nav_title, buf);
}

static void draw_page (void)
{
    if(!page_obj)
        return;
    place_tiles();
    for(int i = 0; i < MAX_TILES; i++)
        if(i < g_cols * g_rows)
            draw_tile(i);
    draw_nav();
}

// a new page: with the build effect the tiles fade in one after the other
static void show_page (void)
{
    draw_page();
    if(!opt_build)
        return;
    int n = 0;
    for(int i = 0; i < g_cols * g_rows && i < MAX_TILES; i++)
        if(!lv_obj_has_flag(tile_obj[i], LV_OBJ_FLAG_HIDDEN))
            lv_obj_fade_in(tile_obj[i], 160, 35 * n++);
}

static void draw_wait (void)
{
    bool ok = pc_ok() && g_pages > 0;
    if(ok == drawn_ok && ok)
        return;
    drawn_ok = ok;

    lv_obj_update_flag(wait_box, LV_OBJ_FLAG_HIDDEN, ok);
    lv_obj_update_flag(grid, LV_OBJ_FLAG_HIDDEN, !ok);
    lv_obj_update_flag(vol_box, LV_OBJ_FLAG_HIDDEN, !ok);
    if(ok) {
        lv_obj_update_flag(nav, LV_OBJ_FLAG_HIDDEN, g_pages <= 1);
        return;
    }
    lv_obj_add_flag(nav, LV_OBJ_FLAG_HIDDEN);

    char buf[160];
    wifi_state_t st = wifi_home_state();
    if(pc_ok())
        strcpy(buf, "Big-Deck\nconnected, waiting for the layout ...");
    else if(!net_key.length())
        strcpy(buf, "Big-Deck\nnot paired yet: connect the USB-C \"UART\" to the PC\nand pair the deck in the PC-Deck app");
    else if(st == Wifi_Connected)
        snprintf(buf, sizeof(buf), "Big-Deck\nwaiting for the PC-Deck app\n%s  %s", wifi_cfg.ssid, wifi_ip());
    else if(*wifi_cfg.ssid)
        snprintf(buf, sizeof(buf), "Big-Deck\nconnecting to '%s' ...", wifi_cfg.ssid);
    else
        strcpy(buf, "Big-Deck\nno home network set: enter it on the WiFi screen");
    lv_label_set_text(wait_lbl, buf);
}

static void osd_show (int pct, const char *text)
{
    lv_label_set_text(osd_lbl, text);
    lv_obj_update_flag(osd_bar, LV_OBJ_FLAG_HIDDEN, pct < 0);
    if(pct >= 0)
        lv_bar_set_value(osd_bar, constrain(pct, 0, 100), LV_ANIM_OFF);
    lv_obj_remove_flag(osd_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(osd_box);
    osd_until = millis() + OSD_MS;
}

// ---------------------------------------------------------------- dimming

static uint8_t bri_normal (void)
{
    return prefs.getUChar("bri", 255);
}

static void wake_up (void)
{
    if(!dimmed)
        return;
    dimmed = false;
    lv_obj_add_flag(clock_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(date_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(batt_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(wake_obj, LV_OPA_TRANSP, 0);
    if(!wake_hold)
        lv_obj_add_flag(wake_obj, LV_OBJ_FLAG_HIDDEN);
    display_backlight(mode_on ? bri_normal() * 100 / 255 : 100);
}

static void draw_clock (void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if(t.tm_min == clock_min)
        return;
    clock_min = t.tm_min;
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    lv_label_set_text_fmt(clock_lbl, "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text_fmt(date_lbl, "%s  %02d.%02d.%04d", days[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);

    // battery at the place of the status bar one, grey; green from BATT_GREEN_V on (really full)
    if(!battery_present()) {
        lv_label_set_text(batt_lbl, "");
        return;
    }
    int p = battery_percent();
    const char *sym = p >= 90 ? LV_SYMBOL_BATTERY_FULL : p >= 60 ? LV_SYMBOL_BATTERY_3 : p >= 35 ? LV_SYMBOL_BATTERY_2
                    : p >= 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    int m = battery_minutes();
    int mv = (int)(battery_volts() * 1000.0f + 0.5f);  // the voltage too, only here on the clock screen
    if(m >= 0)
        lv_label_set_text_fmt(batt_lbl, "%s %d%%  %d,%03dV  %d:%02d", sym, p, mv / 1000, mv % 1000, m / 60, m % 60);
    else
        lv_label_set_text_fmt(batt_lbl, "%s %d%%  %d,%03dV", sym, p, mv / 1000, mv % 1000);
    bool full = battery_volts() >= BATT_GREEN_V;
    lv_obj_set_style_text_color(batt_lbl, lv_color_hex(full ? COL_FULL : COL_DIM), 0);
}

static void dim_down (void)
{
    dimmed = true;
    display_backlight(max(1, dim_bri * 100 / 255));
    bool clock = CLOCK && (opt_clock || !mode_on) && wifi_time_valid();     // the CNC screen always shows the clock
    lv_obj_set_style_bg_opa(wake_obj, clock ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_update_flag(clock_lbl, LV_OBJ_FLAG_HIDDEN, !clock);
    lv_obj_update_flag(date_lbl, LV_OBJ_FLAG_HIDDEN, !clock);
    lv_obj_update_flag(batt_lbl, LV_OBJ_FLAG_HIDDEN, !clock);
    clock_min = -1;
    if(clock)
        draw_clock();
    lv_obj_remove_flag(wake_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wake_obj);
}

// the first touch only wakes: the wake layer takes the whole press and goes away on release
static void wake_event (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED) {
        wake_hold = true;
        wake_up();
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        wake_hold = false;
        if(!dimmed)
            lv_obj_add_flag(wake_obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// PC-Deck: after the time from the app. CNC/laser screen: after the time set on the display, only while
// no job runs; a change of the machine state or of the connection wakes it and counts as use.
static void dim_poll (void)
{
    static uint32_t last_state = 0xFFFFFFFF;
    static bool last_online = false;
    uint32_t ms = mode_on ? dim_ms : (uint32_t)cnc_dim_min * 60000UL;

    if(!mode_on) {
        if(mc.state != last_state || mc.online != last_online) {
            last_state = mc.state;
            last_online = mc.online;
            lv_display_trigger_activity(NULL);
            wake_up();
        }
        bool quiet = !mc.online || mc.state == STATE_IDLE || (mc.state & (STATE_ALARM | STATE_ESTOP));
        if(!quiet)
            ms = 0;
    }
    if(battery_logging())       // discharge log: the same load all the time
        ms = 0;
    if(!ms) {
        wake_up();
        return;
    }
    uint32_t idle = min(lv_display_get_inactive_time(NULL), millis() - enc_active);
    if(!dimmed && !wake_hold && idle >= ms)
        dim_down();
    else if(dimmed && !lv_obj_has_flag(clock_lbl, LV_OBJ_FLAG_HIDDEN))
        draw_clock();
}

// OPT <build 0/1> <dim s|0> <dim brightness> <clock 0/1> [<block world 0/1> <figures 0/1>], kept in the NVS
static void apply_opt (const char *line, bool save)
{
    int b = 0, sec = 0, bd = 25, c = 0, wo = 1, wa = 1;
    sscanf(line, "%d %d %d %d %d %d", &b, &sec, &bd, &c, &wo, &wa);
    opt_build = b;
    dim_ms = sec > 0 ? (uint32_t)sec * 1000 : 0;
    dim_bri = constrain(bd, 1, 255);
    opt_clock = c;
    if(dimmed && mode_on) {
        if(!dim_ms)
            wake_up();
        else
            display_backlight(max(1, dim_bri * 100 / 255));
    }
    if(save) {
        char norm[48];
        snprintf(norm, sizeof(norm), "%d %d %d %d %d %d", b, sec, bd, c, wo, wa);
        if(prefs.getString("opt", "") != norm)
            prefs.putString("opt", norm);
    }
}

// ---------------------------------------------------------------- touch

static void ask_delete (int i)
{
    ask_tile = i;
    lv_obj_remove_flag(ask_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ask_box);
}

// accent frame on place t of the grid, -1 = none
static void drag_show (int t)
{
    if(t == drag_target)
        return;
    drag_target = t;
    if(t < 0) {
        lv_obj_add_flag(drag_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int cols = max(g_cols, 1), rows = max(g_rows, 1);
    int tw = (GRID_W - GAP * (cols + 1)) / cols, th = (grid_h() - GAP * (rows + 1)) / rows;
    int c = t % cols, r = t / cols;
    lv_obj_set_pos(drag_box, GAP + c * (tw + GAP), GAP + r * (th + GAP));
    lv_obj_set_size(drag_box, tw, th);
    lv_obj_remove_flag(drag_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(drag_box);
}

// place of the grid under the finger, -1 outside the grid (the page bar is no target)
static int slot_under (const lv_point_t &p)
{
    lv_area_t g;
    lv_obj_get_coords(grid, &g);
    int x = p.x - g.x1, y = p.y - g.y1, w = lv_area_get_width(&g), h = lv_area_get_height(&g);
    int cols = max(g_cols, 1), rows = max(g_rows, 1);
    if(x < 0 || y < 0 || x >= w || y >= h)
        return -1;
    int t = (y * rows / h) * cols + x * cols / w;
    return t < MAX_TILES ? t : -1;
}

static void tile_event (lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if(!pc_ok() || ask_tile >= 0)
        return;

    lv_point_t p = { 0, 0 };
    if(lv_indev_active())
        lv_indev_get_point(lv_indev_active(), &p);

    if(code == LV_EVENT_PRESSED) {
        down_tile = i;
        down_since = millis();
        down_pt = p;
        long_sent = delete_armed = dragging = false;
        drag_show(-1);
        tile_frame(i, true, false);
    } else if(code == LV_EVENT_PRESSING && down_tile == i) {
        uint32_t held = millis() - down_since;
        if(!long_sent && held >= LONG_MS) {
            long_sent = true;
            tile_frame(i, true, true);
        }
        // from 0.6 s on and half a tile away: drag (stored tiles only, same page)
        if(long_sent && find_tile(cur_page, i)) {
            int cols = max(g_cols, 1), rows = max(g_rows, 1);
            if(!dragging && (abs(p.x - down_pt.x) > GRID_W / cols / 2 || abs(p.y - down_pt.y) > grid_h() / rows / 2))
                dragging = true;
            if(dragging) {
                int t = slot_under(p);
                drag_show(t == i ? -1 : t);
            }
        }
        if(!dragging && !delete_armed && held >= DELETE_MS && find_tile(cur_page, i)) {
            delete_armed = true;
            tile_frame(i, true, true);
            down_tile = -1;
            ask_delete(i);
        }
    } else if(code == LV_EVENT_RELEASED && down_tile == i) {
        if(dragging) {      // moved: the app swaps the tiles and sends both places again (TSET/TDEL)
            int t = drag_target;
            drag_show(-1);
            if(t >= 0)
                io->printf("MOVE %d %d %d\n", cur_page, i, t);
        } else
            io->printf(long_sent ? "LONG %d %d\n" : "PRESS %d %d\n", cur_page, i);
        down_tile = -1;
        long_sent = dragging = false;
        tile_frame(i, false, false);
    } else if(code == LV_EVENT_PRESS_LOST && down_tile == i) {
        down_tile = -1;
        long_sent = dragging = false;
        drag_show(-1);
        tile_frame(i, false, false);
    }
}

// volume slider: VOL <0..100> to the app while dragging (at most every VOL_SEND_MS) and on release
static void vol_event (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int v = lv_slider_get_value(vol_slider);

    // saddle cap seen from above: two raised ends with ribs, a round hollow between them lit from the top,
    // the white index line across the lowest point of the hollow
    if(code == LV_EVENT_DRAW_MAIN_END) {
        const lv_area_t *k = &((lv_slider_t *)vol_slider)->right_knob_area;
        lv_layer_t *layer = lv_event_get_layer(e);
        int32_t cy = (k->y1 + k->y2) / 2;
        lv_draw_rect_dsc_t d;

        auto grad = [&](int32_t y1, int32_t y2, uint32_t c1, uint32_t c2, int r) {
            lv_draw_rect_dsc_init(&d);
            d.radius = r;
            d.bg_grad.dir = LV_GRAD_DIR_VER;
            d.bg_grad.stops_count = 2;
            d.bg_grad.stops[0].color = lv_color_hex(c1);
            d.bg_grad.stops[1].color = lv_color_hex(c2);
            d.bg_grad.stops[0].opa = d.bg_grad.stops[1].opa = LV_OPA_COVER;
            d.bg_grad.stops[0].frac = 0;
            d.bg_grad.stops[1].frac = 255;
            lv_area_t a = { k->x1 + 1, y1, k->x2 - 1, y2 };
            lv_draw_rect(layer, &d, &a);
        };
        auto line = [&](int32_t y, uint32_t c, lv_opa_t opa, int inset) {
            lv_draw_rect_dsc_init(&d);
            d.bg_color = lv_color_hex(c);
            d.bg_opa = opa;
            lv_area_t a = { k->x1 + inset, y, k->x2 - inset, y };
            lv_draw_rect(layer, &d, &a);
        };

        // raised ends: lit at the outer edge, falling off towards the hollow
        grad(k->y1 + 1, k->y1 + FADER_END, FADER_RIDGE, FADER_RIDGE2, 5);
        grad(k->y2 - FADER_END, k->y2 - 1, FADER_RIDGE2, FADER_RIDGE, 5);
        // hollow: the upper side in the shadow of the upper end, the lower side catches the light
        grad(k->y1 + FADER_END + 1, cy, FADER_HOLLOW, FADER_SHINE, 0);
        grad(cy + 1, k->y2 - FADER_END - 1, FADER_SHINE, FADER_HOLLOW, 0);
        // fine ribs across the ends, each with a light edge
        for(int r = 0; r < 3; r++) {
            int32_t y = k->y1 + 4 + r * 4;
            line(y, FADER_RIB, LV_OPA_COVER, 4);
            line(y + 1, 0xFFFFFF, 110, 4);
            y = k->y2 - 5 - r * 4;
            line(y, FADER_RIB, LV_OPA_COVER, 4);
            line(y + 1, 0xFFFFFF, 110, 4);
        }
        // highlight on the top edge of the cap, index line in the hollow
        line(k->y1 + 1, 0xFFFFFF, 120, 6);
        line(cy - 2, FADER_RIB, 160, 1);        // white index line with a dark edge, visible on silver
        line(cy - 1, 0xFFFFFF, LV_OPA_COVER, 1);
        line(cy, 0xFFFFFF, LV_OPA_COVER, 1);
        line(cy + 1, FADER_RIB, 160, 1);
        return;
    }

    if(code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED)
        return;
    lv_label_set_text_fmt(vol_lbl, "%d", v);
    if(!pc_ok() || v == vol_sent || (code == LV_EVENT_VALUE_CHANGED && millis() - vol_at < VOL_SEND_MS))
        return;
    vol_sent = v;
    vol_at = millis();
    io->printf("VOL %d\n", v);
}

static void nav_event (lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);

    if(!pc_ok() || g_pages <= 1 || ask_tile >= 0)
        return;
    cur_page = (cur_page + dir + g_pages) % g_pages;
    clear_live();
    show_page();
    io->printf("SHOW %d\n", cur_page);      // the app sends the live tiles of this page
}

static void ask_event (lv_event_t *e)
{
    bool yes = (bool)(intptr_t)lv_event_get_user_data(e);

    if(ask_tile >= 0 && yes && pc_ok())
        io->printf("DEL %d %d\n", cur_page, ask_tile);
    if(ask_tile >= 0) {
        delete_armed = false;
        tile_frame(ask_tile, false, false);
    }
    ask_tile = -1;
    lv_obj_add_flag(ask_box, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- encoder

static volatile int32_t enc_count = 0;
static volatile uint8_t enc_state = 0;

static void IRAM_ATTR enc_isr (void)
{
    // Gray code: old and new state of A/B -> -1, 0 or +1 (invalid jumps do not count)
    static const int8_t step[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
    uint8_t ab = (gpio_get_level((gpio_num_t)PIN_ENC_A) << 1) | gpio_get_level((gpio_num_t)PIN_ENC_B);
    enc_state = ((enc_state << 2) | ab) & 0x0F;
    enc_count += step[enc_state];
}

static void encoder_begin (void)
{
    if(!ENCODER)
        return;
    pinMode(PIN_ENC_A, INPUT_PULLUP);
    pinMode(PIN_ENC_B, INPUT_PULLUP);
    pinMode(PIN_ENC_SW, INPUT_PULLUP);
    enc_state = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), enc_isr, CHANGE);
}

static void encoder_poll (void)
{
    static bool sw_raw = false, sw_down = false, sw_turned = false;
    static uint32_t sw_change = 0, sw_since = 0, last_send = 0;
    if(!ENCODER)
        return;
    uint32_t now = millis();
    bool report = mode_on && pc_ok();

    // push button, debounced 20 ms
    bool raw = digitalRead(PIN_ENC_SW) == LOW;
    if(raw != sw_raw) {
        sw_raw = raw;
        sw_change = now;
    }
    if(now - sw_change >= 20) {
        if(raw && !sw_down) {
            sw_down = true;
            sw_turned = dimmed;     // waking press: its release reports nothing
            sw_since = now;
            enc_active = now;
            wake_up();
        } else if(!raw && sw_down) {
            sw_down = false;
            if(report && !sw_turned)
                io->println(now - sw_since >= KNOB_LONG_MS ? "KNOBLONG" : "KNOB");
            else if(!mode_on && !sw_turned && now - sw_since < KNOB_LONG_MS)
                link_rt(CMD_OVR_FEED_RESET);        // CNC: short press, feed override back to 100 %
        }
    }

    // turns, sent together every ENC_SEND_MS; a started detent stays
    if(now - last_send < ENC_SEND_MS)
        return;
    noInterrupts();
    int32_t detents = enc_count / ENC_STEPS;
    enc_count -= detents * ENC_STEPS;
    interrupts();
    if(detents == 0)
        return;
    last_send = now;
    enc_active = now;
    if(dimmed) {                // the first turn only wakes
        wake_up();
        return;
    }
    if(ENC_REVERSE)
        detents = -detents;
    if(sw_down)
        sw_turned = true;       // turned while pressed: the release is not a press
    if(report)
        io->printf(sw_down ? "ROTP %+d\n" : "ROT %+d\n", (int)detents);
    else if(!mode_on) {
        // CNC: feed override, 1 % per detent, 10 % while pressed (grblHAL limits it to 10..200 %)
        uint8_t cmd = sw_down ? (detents > 0 ? CMD_OVR_FEED_PLUS : CMD_OVR_FEED_MINUS)
                              : (detents > 0 ? CMD_OVR_FEED_FINE_PLUS : CMD_OVR_FEED_FINE_MINUS);
        for(int i = abs((int)detents); i > 0; i--)
            link_rt(cmd);
    }
}

// ---------------------------------------------------------------- protocol

static void handle_wifi (char *arg)
{
    if(!strcmp(arg, "STATUS")) {
        bool conn = wifi_home_state() == Wifi_Connected;
        io->printf("WIFI %d %d %s %s %d %d\n", net_key.length() ? 1 : 0, conn ? 1 : 0, conn ? wifi_ip() : "-",
                   *wifi_cfg.ssid ? to_hex(wifi_cfg.ssid).c_str() : "-", wifi_home_state() != Wifi_Off ? 1 : 0,
                   millis() - last_serial < USB_DATA_MS ? 1 : 0);
        io->println("ACK");
    } else if(!strcmp(arg, "SCAN")) {
        wifi_scan_start();
        int n = -1;
        for(uint32_t t0 = millis(); (n = wifi_scan_result()) < 0 && n != WIFI_SCAN_ERROR && millis() - t0 < 10000; )
            delay(50);
        for(int k = 0; k < n; k++)
            io->printf("NET -60 0 %s\n", to_hex(wifi_scan_ssid(k)).c_str());     // signal and security not kept
        io->println("ACK");
    } else if(from_net)
        io->println("ERR usb");     // set up only over USB
    else if(!strncmp(arg, "SET ", 4)) {
        char ssid[80] = { 0 }, pass[140] = { 0 }, key[80] = { 0 };
        if(sscanf(arg + 4, "%79s %139s %79s", ssid, pass, key) != 3) {
            io->println("ERR wifi");
            return;
        }
        // the home network of the display: the same one the app chose; "-" (empty field in the app) for the
        // network already set keeps its stored password
        bool keep = !strcmp(pass, "-") && from_hex(ssid) == wifi_cfg.ssid;
        strlcpy(wifi_cfg.ssid, from_hex(ssid).c_str(), sizeof(wifi_cfg.ssid));
        if(!keep)
            strlcpy(wifi_cfg.pass, strcmp(pass, "-") ? from_hex(pass).c_str() : "", sizeof(wifi_cfg.pass));
        wifi_cfg.enabled = true;
        wifi_apply();
        net_key = key;
        prefs.putString("key", net_key);
        io->println("ACK");
    } else if(!strcmp(arg, "RESET")) {     // another PC: forget the pairing (the home network stays)
        net_key = "";
        prefs.putString("key", net_key);
        io->println("ACK");
    } else if(!strncmp(arg, "EN ", 3)) {
        wifi_cfg.enabled = atoi(arg + 3) != 0;
        wifi_apply();
        io->println("ACK");
    } else
        io->println("ERR wifi");
}

static void handle_line (char *line)
{
    last_pc = millis();
    pc_seen = true;

    if(!strncmp(line, "HELLO", 5)) {
        // fields as Deck 1/2: fw, size, id, layout hash, page drawn at once, design hash, IP
        io->printf("DECK %s %d %d %s %s 1 %s %s\n", DECK_FW, DECK_W, DECK_H, deck_id.c_str(), g_hash.c_str(),
                   theme.on ? theme.hash.c_str() : "none",
                   wifi_home_state() == Wifi_Connected ? wifi_ip() : "-");
    } else if(!strncmp(line, "BEGIN ", 6)) {
        char hash[64] = { 0 };
        if(sscanf(line + 6, "%d %d %d %63s", &p_cols, &p_rows, &p_pages, hash) != 4) {
            io->println("ERR begin");
            return;
        }
        p_cols = constrain(p_cols, 1, MAX_COLS);
        p_rows = constrain(p_rows, 1, MAX_ROWS);
        p_pages = constrain(p_pages, 0, MAX_PAGES);
        p_hash = hash;
        pending.clear();
        for(auto &t : page_title)
            t = "";
        clear_store();
        clear_live();
        io->println("ACK");
    } else if(!strncmp(line, "TILE ", 5)) {
        int p, i, ic, off = 0;
        if(sscanf(line + 5, "%d %d %d %n", &p, &i, &ic, &off) < 3) {
            io->println("ERR tile");
            return;
        }
        tile_t t { (uint8_t)p, (uint8_t)i, ic != 0, off > 0 ? String(line + 5 + off) : String("") };
        t.label.trim();
        pending.push_back(t);
        io->println("ACK");
    } else if(!strncmp(line, "ICON ", 5) || !strncmp(line, "ICONZ ", 6)) {
        bool z = line[4] == 'Z';
        int p, i, n;
        if(sscanf(line + (z ? 6 : 5), "%d %d %d", &p, &i, &n) != 3 || n <= 0 || n > ICON_BYTES || (z && n == ICON_BYTES) ||
            (!z && n != ICON_BYTES)) {
            io->println(z ? "ERR iconz" : "ERR icon");
            return;
        }
        if(read_exact(z_buf, n) != (size_t)n) {
            io->println("ERR short");
            return;
        }
        if(z && !unpack_icon(z_buf, n, icon_buf)) {
            io->println("ERR iconz");
            return;
        }
        File f = LittleFS.open(icon_path(p, i), "w");
        if(!f) {
            io->println("ERR fs");
            return;
        }
        size_t w = f.write(z_buf, n);
        f.close();
        io->println(w == (size_t)n ? "ACK" : "ERR full");
    } else if(!strcmp(line, "END")) {
        tiles = pending;
        pending.clear();
        g_cols = p_cols;
        g_rows = p_rows;
        g_pages = p_pages;
        g_hash = p_hash;
        save_layout();
        if(cur_page >= g_pages)
            cur_page = 0;
        drawn_ok = false;
        show_page();
        io->println("ACK");
    } else if(!strncmp(line, "TSET ", 5) || !strncmp(line, "TDEL ", 5)) {
        bool del = line[1] == 'D';
        int p, i, ic = 0, off = 0;
        int n = del ? sscanf(line + 5, "%d %d", &p, &i) : sscanf(line + 5, "%d %d %d %n", &p, &i, &ic, &off);
        if(n < (del ? 2 : 3) || p < 0 || p >= g_pages || i < 0 || i >= g_cols * g_rows) {
            io->println("ERR tset");
            return;
        }
        for(size_t k = 0; k < tiles.size(); k++)
            if(tiles[k].page == p && tiles[k].idx == i) {
                tiles.erase(tiles.begin() + k);
                break;
            }
        if(del || !ic)
            LittleFS.remove(icon_path(p, i));
        if(!del) {
            tile_t t { (uint8_t)p, (uint8_t)i, ic != 0, off > 0 ? String(line + 5 + off) : String("") };
            t.label.trim();
            tiles.push_back(t);
        }
        save_layout();
        if(p == cur_page) {
            live[i].used = false;
            draw_tile(i);
        }
        io->println("ACK");
    } else if(!strncmp(line, "LHASH ", 6)) {
        char hash[64] = { 0 };
        sscanf(line + 6, "%63s", hash);
        g_hash = hash;
        save_layout();
        io->println("ACK");
    } else if(!strncmp(line, "UPD ", 4)) {
        // live tile, RAM only, visible page only: UPD <page> <place> <icon 0/1/2> [<bytes>] <text>
        int p, i, ic, off = 0, n = ICON_BYTES;
        if(sscanf(line + 4, "%d %d %d %n", &p, &i, &ic, &off) < 3 || i < 0 || i >= MAX_TILES) {
            io->println("ERR upd");
            return;
        }
        if(ic == 2) {
            int off2 = 0;
            if(sscanf(line + 4 + off, "%d %n", &n, &off2) < 1 || n <= 0 || n >= ICON_BYTES) {
                io->println("ERR upd");
                return;
            }
            off += off2;
        }
        live_t &l = live[i];
        if(ic) {
            if(l.cap < n) {
                free(l.buf);
                l.buf = (uint8_t *)ps_malloc(n);
                l.cap = l.buf ? n : 0;
            }
            uint8_t *dst = l.buf ? l.buf : z_buf;
            if(read_exact(dst, n) != (size_t)n) {
                io->println("ERR short");
                return;
            }
            if(!l.buf) {
                io->println("ERR mem");
                return;
            }
            l.len = n;
        }
        if(p == cur_page && g_pages > 0) {
            l.used = true;
            l.icon = ic != 0;
            l.label = off > 0 ? String(line + 4 + off) : String("");
            l.label.trim();
            draw_tile(i);
        }
        io->println("ACK");
    } else if(!strncmp(line, "CLR ", 4)) {
        int p, i;
        if(sscanf(line + 4, "%d %d", &p, &i) != 2 || i < 0 || i >= MAX_TILES) {
            io->println("ERR clr");
            return;
        }
        if(p == cur_page && live[i].used) {
            live[i].used = false;
            draw_tile(i);
        }
        io->println("ACK");
    } else if(!strcmp(line, "CURPAGE")) {
        io->printf("CURPAGE %d\n", cur_page);
        io->println("ACK");
    } else if(!strncmp(line, "TITLE ", 6)) {
        int p = -1, off = 0;
        if(sscanf(line + 6, "%d %n", &p, &off) < 1 || p < 0 || p >= MAX_PAGES || off == 0) {
            io->println("ERR title");
            return;
        }
        String t = strcmp(line + 6 + off, "-") ? from_hex(line + 6 + off) : String("");
        t.trim();
        if(t.length() > 28)
            t = t.substring(0, 28);
        if(t != page_title[p]) {
            page_title[p] = t;
            if(p == cur_page)
                draw_nav();
        }
        io->println("ACK");
    } else if(!strncmp(line, "PAGE ", 5)) {
        int p = atoi(line + 5);
        if(p >= 0 && p < g_pages && p != cur_page) {
            cur_page = p;
            clear_live();
            show_page();
        }
        io->println("ACK");
    } else if(!strncmp(line, "BRI ", 4)) {
        int b = constrain(atoi(line + 4), 5, 255);
        prefs.putUChar("bri", (uint8_t)b);
        if(mode_on)
            display_backlight(b * 100 / 255);
        io->println("ACK");
    } else if(!strncmp(line, "VOL ", 4)) {
        // volume from the PC (Big-Deck): moves the slider unless a finger is on it
        int v = constrain(atoi(line + 4), 0, 100);
        if(vol_slider && !lv_obj_has_state(vol_slider, LV_STATE_PRESSED)) {
            lv_slider_set_value(vol_slider, v, LV_ANIM_OFF);
            lv_label_set_text_fmt(vol_lbl, "%d", v);
            vol_sent = v;
        }
        io->println("ACK");
    } else if(!strncmp(line, "OSD ", 4)) {
        char *p = line + 4;
        int pct = *p == '-' ? -1 : atoi(p);
        char *sp = strchr(p, ' ');
        if(mode_on)
            osd_show(pct, sp ? sp + 1 : "");
        io->println("ACK");
    } else if(!strncmp(line, "IMG ", 4)) {
        // IMG <boot|page> <bytes>, then DATA blocks and IMGEND; the boot image is read and thrown away
        char name[8] = { 0 };
        unsigned n = 0;
        sscanf(line + 4, "%7s %u", name, &n);
        if(img_file)
            img_file.close();
        img_left = n;
        img_discard = !strcmp(name, "boot");
        if(!img_discard && (strcmp(name, "page") || n != PAGE_BYTES)) {
            img_left = 0;
            io->println("ERR img");
            return;
        }
        if(!img_discard)
            img_file = LittleFS.open(PAGE_FILE ".tmp", "w");
        io->println(img_discard || img_file ? "ACK" : "ERR fs");
    } else if(!strncmp(line, "DATA ", 5)) {
        // at most 4096 bytes; ACK after writing = flow control
        unsigned n = atoi(line + 5);
        if(!(img_file || img_discard) || n == 0 || n > 4096 || n > img_left) {
            io->println("ERR data");
            return;
        }
        if(read_exact(blk, n) != n) {
            io->println("ERR short");
            return;
        }
        if(img_file)
            img_file.write(blk, n);
        img_left -= n;
        io->println("ACK");
    } else if(!strcmp(line, "IMGEND")) {
        if(img_discard) {
            img_discard = false;
            io->println(img_left ? "ERR imgend" : "ACK");
            return;
        }
        if(!img_file || img_left) {
            if(img_file)
                img_file.close();
            io->println("ERR imgend");
            return;
        }
        img_file.close();
        LittleFS.remove(PAGE_FILE);
        LittleFS.rename(PAGE_FILE ".tmp", PAGE_FILE);
        if(theme.on) {
            load_page_image();
            apply_theme();
        }
        io->println("ACK");
    } else if(!strncmp(line, "THEME ", 6)) {
        // THEME off | THEME <hash> <tile top> <tile bottom> <border top> <border bottom> <style> <opacity>
        if(!strcmp(line + 6, "off")) {
            LittleFS.remove(THEME_FILE);
            theme = theme_t();
        } else {
            File f = LittleFS.open(THEME_FILE, "w");
            if(f) {
                f.println(line + 6);
                f.close();
            }
            load_theme();
        }
        clear_live();
        apply_theme();
        draw_page();
        io->println("ACK");
    } else if(!strncmp(line, "OPT ", 4)) {
        apply_opt(line + 4, true);
        io->println("ACK");
    } else if(!strncmp(line, "TIME ", 5)) {
        // UTC from the app; the display keeps its own time zone. Only a missing or wrong clock is set.
        unsigned long u = 0;
        sscanf(line + 5, "%lu", &u);
        time_t now = time(NULL);
        if(u > 1700000000UL && (!wifi_time_valid() || labs((long)(now - (time_t)u)) > 2)) {
            struct timeval tv = { (time_t)u, 0 };
            settimeofday(&tv, NULL);
            clock_min = -1;
        }
        io->println("ACK");
    } else if(!strncmp(line, "WIFI ", 5))
        handle_wifi(line + 5);
    else if(!strcmp(line, "LOGO"))
        io->printf("LOGO %s %s 0\nACK\n", to_hex("grblHAL").c_str(), to_hex("Big-Deck").c_str());
    else if(!strcmp(line, "FWINFO"))
        io->printf("FWINFO " DECK_FW " bigdeck 0\nACK\n");     // updates of the display not through the app
    else if(!strncmp(line, "FW", 2))
        io->println("ERR fw");
    else if(!strcmp(line, "WSTAT")) {
        io->printf("WSTAT %d 0 0 0 %lu %d\n", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                   (unsigned long)(millis() / 1000), (int)esp_reset_reason());
        io->println("ACK");
    } else
        // PING, NAME, CAL, TDIAG, LS: accepted, nothing to do
        io->println("ACK");
}

// one line from a channel; console lines (lower case) go to the console
static void dispatch (char *line, bool net)
{
    if(!net && !(*line >= 'A' && *line <= 'Z')) {
        if(console_cb)
            console_cb(line);
        return;
    }
    if(!BIGDECK)            // CNC only: the deck protocol is not answered
        return;
    if(net) {
        io = &net_client;
        in = &net_client;
    } else {
        io = &Serial;
        in = &Serial;
        last_serial = millis();
    }
    from_net = net;
    via_net = net;
    handle_line(line);
}

static void serial_poll (void)
{
    static char line[256];
    static size_t len = 0;

    while(Serial.available()) {
        char c = Serial.read();
        if(c == '\n' || c == '\r') {
            if(len) {
                line[len] = '\0';
                len = 0;
                dispatch(line, false);
            }
        } else if(len < sizeof(line) - 1)
            line[len++] = c;
    }
}

static void net_poll (void)
{
    static char buf[256];
    static size_t len = 0;

    if(!BIGDECK)            // CNC only: no deck server in the home network
        return;
    if(wifi_home_state() != Wifi_Connected) {
        if(net_started) {
            if(net_client)
                net_client.stop();
            net_server.end();
            disc.stop();
            net_started = false;
            net_auth = false;
        }
        return;
    }
    if(!net_started) {
        net_server.begin();
        net_server.setNoDelay(true);
        disc.begin(DISC_PORT);
        net_started = true;
    }

    bool busy = net_client.connected() && net_auth && millis() - net_last < PC_TIMEOUT_MS;
    if(disc.parsePacket() > 0) {
        char b[16] = { 0 };
        disc.read(b, sizeof(b) - 1);
        if(!strncmp(b, "PCDECK?", 7)) {
            disc.beginPacket(disc.remoteIP(), disc.remotePort());
            disc.printf("PCDECK %s %d %s %d\n", deck_id.c_str(), NET_PORT, DECK_FW, busy ? 1 : 0);
            disc.endPacket();
        }
    }

    if(net_server.hasClient()) {
        WiFiClient c = net_server.accept();
        if(busy) {
            c.println("ERR busy");
            c.stop();
        } else {
            if(net_client)
                net_client.stop();
            net_client = c;
            net_client.setNoDelay(true);
            net_auth = false;
            net_nonce = "";
            len = 0;
            net_last = millis();
        }
    }
    if(!net_client.connected())
        return;
    if(!net_auth && millis() - net_last > 5000) {   // no login: not kept open
        net_client.stop();
        return;
    }

    while(net_client.available()) {
        int c = net_client.read();
        if(c < 0)
            break;
        if(c == '\n') {
            buf[len] = '\0';
            len = 0;
            net_last = millis();
            if(!net_auth) {     // AUTH1 <nonce_pc>, then AUTH2 <proof>
                if(net_key.length() && !net_nonce.length() && !strncmp(buf, "AUTH1 ", 6) && strlen(buf) <= 70) {
                    char n[33];
                    for(int i = 0; i < 4; i++)
                        snprintf(n + i * 8, 9, "%08lx", (unsigned long)esp_random());
                    net_nonce = n;
                    net_client.printf("CHAL %s %s\n", net_nonce.c_str(), hmac_hex(net_key, "D" + String(buf + 6)).c_str());
                    continue;
                }
                net_auth = net_nonce.length() && !strncmp(buf, "AUTH2 ", 6) &&
                           same_text(String(buf + 6), hmac_hex(net_key, "A" + net_nonce));
                net_nonce = "";
                net_client.println(net_auth ? "ACK" : "ERR auth");
                if(!net_auth) {
                    net_client.stop();
                    return;
                }
                continue;
            }
            dispatch(buf, true);
            if(!net_client.connected())
                return;
        } else if(c != '\r' && len < sizeof(buf) - 1)
            buf[len++] = (char)c;
    }
}

// ---------------------------------------------------------------- public

void deck_begin (void (*console)(char *line))
{
    console_cb = console;

    uint64_t mac = ESP.getEfuseMac();
    char id[13];
    snprintf(id, sizeof(id), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
    deck_id = id;

    prefs.begin("pcdeck", false);
    net_key = prefs.getString("key", "");

    icon_buf = (uint16_t *)ps_malloc(ICON_BYTES);
    z_buf = (uint8_t *)ps_malloc(ICON_BYTES);
    blk = (uint8_t *)ps_malloc(4096);

    cnc_dim_min = prefs.getUChar("cncdim", 0);
    String opt = prefs.getString("opt", "");
    if(opt.length())
        apply_opt(opt.c_str(), false);

    if(LittleFS.begin(true)) {
        load_layout();
        load_theme();
    } else
        Serial.println("Big-Deck: no file system");

    encoder_begin();
    if(!BIGDECK)
        return;
    Serial.printf("Big-Deck %s, id %s, %s, layout %dx%d, %d pages\n", DECK_FW, deck_id.c_str(),
                  net_key.length() ? "paired" : "not paired", g_cols, g_rows, g_pages);
}

void deck_poll (void)
{
    serial_poll();
    net_poll();
    encoder_poll();

    if(!page_obj)
        return;
    dim_poll();
    if(osd_until && (int32_t)(millis() - osd_until) >= 0) {
        osd_until = 0;
        lv_obj_add_flag(osd_box, LV_OBJ_FLAG_HIDDEN);
    }
    static uint32_t last = 0;
    if(millis() - last >= 250) {
        last = millis();
        draw_wait();
    }
}

static lv_obj_t *plain (lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int r)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, r, 0);
    return o;
}

static lv_obj_t *text (lv_obj_t *parent, const lv_font_t *font, uint32_t col, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, s);
    return l;
}

static lv_obj_t *button (lv_obj_t *parent, int x, int y, int w, int h, const char *s, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = plain(parent, x, y, w, h, COL_TILE, 12);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_obj_center(text(b, &lv_font_montserrat_24, COL_TEXT, s));
    return b;
}

void deck_ui (lv_obj_t *root, int y)
{
    area_y = y;
    area_h = DECK_H - y;

    page_obj = plain(root, 0, y, DECK_W, area_h, COL_BG, 0);

    // page image of a design: 470x270, stretched over the tile area
    page_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    page_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    page_dsc.header.w = PAGE_W;
    page_dsc.header.h = PAGE_H;
    page_dsc.header.stride = PAGE_W * 2;
    page_dsc.data_size = PAGE_BYTES;
    page_img = lv_image_create(page_obj);
    lv_obj_set_pos(page_img, 0, 0);
    lv_obj_set_size(page_img, GRID_W, area_h);
    lv_image_set_inner_align(page_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(page_img, LV_OBJ_FLAG_HIDDEN);

    grid = plain(page_obj, 0, 0, DECK_W, area_h, COL_BG, 0);

    for(int i = 0; i < MAX_TILES; i++) {
        lv_obj_t *t = plain(grid, 0, 0, 100, 100, COL_TILE, 14);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLL_CHAIN);    // dragging a tile never scrolls a parent
        lv_obj_set_style_border_opa(t, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(t, tile_event, LV_EVENT_ALL, (void *)(intptr_t)i);
        tile_obj[i] = t;

        tile_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        tile_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565A8;     // alpha plane: key colour see-through
        tile_dsc[i].header.w = ICON_SZ;
        tile_dsc[i].header.h = ICON_SZ;
        tile_dsc[i].header.stride = ICON_SZ * 2;
        tile_dsc[i].data_size = TILE_BYTES;
        tile_dsc[i].data = (const uint8_t *)ps_calloc(1, TILE_BYTES);

        // glass style: white shine on the upper half, fading out downwards
        tile_shine[i] = lv_obj_create(t);
        lv_obj_remove_style_all(tile_shine[i]);
        lv_obj_remove_flag(tile_shine[i], (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_style_radius(tile_shine[i], 14, 0);
        lv_obj_set_style_bg_color(tile_shine[i], lv_color_white(), 0);
        lv_obj_set_style_bg_grad_color(tile_shine[i], lv_color_white(), 0);
        lv_obj_set_style_bg_grad_dir(tile_shine[i], LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(tile_shine[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(tile_shine[i], 60, 0);
        lv_obj_set_style_bg_grad_opa(tile_shine[i], 0, 0);
        lv_obj_add_flag(tile_shine[i], LV_OBJ_FLAG_HIDDEN);

        tile_img[i] = lv_image_create(t);
        lv_image_set_antialias(tile_img[i], true);
        lv_obj_remove_flag(tile_img[i], LV_OBJ_FLAG_CLICKABLE);
        tile_lbl[i] = text(t, &lv_font_montserrat_18, COL_TEXT, "");
        lv_label_set_long_mode(tile_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    }

    // target frame while a tile is dragged, also on an empty place
    drag_box = lv_obj_create(grid);
    lv_obj_remove_style_all(drag_box);
    lv_obj_remove_flag(drag_box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_radius(drag_box, 14, 0);
    lv_obj_set_style_border_width(drag_box, 4, 0);
    lv_obj_set_style_border_color(drag_box, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_opa(drag_box, LV_OPA_COVER, 0);
    lv_obj_add_flag(drag_box, LV_OBJ_FLAG_HIDDEN);

    nav = plain(page_obj, 0, area_h - NAV_H, GRID_W, NAV_H, COL_BG, 0);
    button(nav, GAP, 4, 160, NAV_H - 10, LV_SYMBOL_LEFT, nav_event, (void *)(intptr_t)-1);
    button(nav, GRID_W - GAP - 160, 4, 160, NAV_H - 10, LV_SYMBOL_RIGHT, nav_event, (void *)(intptr_t)1);

    // volume: vertical slider at the right edge, speaker symbol on top, value below
    vol_box = plain(page_obj, GRID_W, 0, VOL_W, area_h, COL_BG, 0);
    lv_obj_t *spk = text(vol_box, &lv_font_montserrat_18, COL_DIM, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(spk, LV_ALIGN_TOP_MID, 0, 6);
    int track_y = (area_h - FADER_LEN) / 2;
    for(int m = 0; m <= 10; m++) {
        int len = m % 5 ? 7 : 12, y = track_y + m * FADER_LEN / 10;
        for(int side = 0; side < 2; side++) {
            lv_obj_t *t = plain(vol_box, side ? VOL_W / 2 + 10 : VOL_W / 2 - 10 - len, y - 1, len, 2, m % 5 ? 0x4A4E58 : 0x7A7F8C, 1);
            lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    vol_slider = lv_slider_create(vol_box);
    // like a mixing desk fader: long narrow track, accent colour from below, cap with a finger hollow (vol_event)
    lv_obj_set_size(vol_slider, FADER_TRACK, FADER_LEN);    // higher than wide: vertical
    lv_obj_align(vol_slider, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(vol_slider, 0, 100);
    lv_obj_set_style_radius(vol_slider, FADER_TRACK / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(vol_slider, FADER_TRACK / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0x07080B), LV_PART_MAIN);     // the slot
    lv_obj_set_style_bg_opa(vol_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(vol_slider, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(vol_slider, lv_color_hex(0x3A3E48), LV_PART_MAIN);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);  // glowing level
    lv_obj_set_style_shadow_color(vol_slider, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(vol_slider, 12, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(vol_slider, 160, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(FADER_TOP), LV_PART_KNOB);        // silver saddle cap
    lv_obj_set_style_bg_grad_color(vol_slider, lv_color_hex(FADER_BOT), LV_PART_KNOB);
    lv_obj_set_style_bg_grad_dir(vol_slider, LV_GRAD_DIR_VER, LV_PART_KNOB);
    lv_obj_set_style_border_width(vol_slider, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(vol_slider, lv_color_hex(0x4A4E58), LV_PART_KNOB);
    lv_obj_set_style_radius(vol_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_pad_hor(vol_slider, (FADER_W - FADER_TRACK) / 2, LV_PART_KNOB);    // 46 px wide
    lv_obj_set_style_pad_ver(vol_slider, (FADER_H - FADER_TRACK) / 2, LV_PART_KNOB);    // 64 px high
    lv_obj_set_style_shadow_width(vol_slider, 14, LV_PART_KNOB);
    lv_obj_set_style_shadow_offset_y(vol_slider, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(vol_slider, 180, LV_PART_KNOB);
    lv_obj_set_ext_click_area(vol_slider, 24);
    lv_obj_add_event_cb(vol_slider, vol_event, LV_EVENT_ALL, NULL);
    vol_lbl = text(vol_box, &lv_font_montserrat_16, COL_DIM, "");
    lv_obj_align(vol_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
    nav_title = text(nav, &lv_font_montserrat_24, COL_DIM, "");
    lv_obj_center(nav_title);

    wait_box = plain(page_obj, 0, 0, DECK_W, area_h, COL_BG, 0);
    lv_obj_add_flag(vol_box, LV_OBJ_FLAG_HIDDEN);
    wait_lbl = text(wait_box, &lv_font_montserrat_32, COL_DIM, "Big-Deck");
    lv_obj_center(wait_lbl);

    ask_box = plain(page_obj, (DECK_W - 520) / 2, (area_h - 220) / 2, 520, 220, 0x1E2028, 18);
    lv_obj_set_style_border_width(ask_box, 3, 0);
    lv_obj_set_style_border_color(ask_box, lv_color_hex(COL_LONG), 0);
    lv_obj_t *q = text(ask_box, &lv_font_montserrat_32, COL_TEXT, "Delete tile?");
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 34);
    button(ask_box, 30, 130, 210, 64, "Cancel", ask_event, (void *)(intptr_t)0);
    lv_obj_t *del = button(ask_box, 280, 130, 210, 64, "Delete", ask_event, (void *)(intptr_t)1);
    lv_obj_set_style_bg_color(del, lv_color_hex(COL_LONG), 0);
    lv_obj_add_flag(ask_box, LV_OBJ_FLAG_HIDDEN);

    osd_box = plain(page_obj, (DECK_W - 560) / 2, (area_h - 140) / 2, 560, 140, 0x1E2028, 18);
    lv_obj_set_style_bg_opa(osd_box, 235, 0);
    osd_lbl = text(osd_box, &lv_font_montserrat_32, COL_TEXT, "");
    lv_obj_align(osd_lbl, LV_ALIGN_TOP_MID, 0, 24);
    osd_bar = lv_bar_create(osd_box);
    lv_obj_set_size(osd_bar, 460, 20);
    lv_obj_align(osd_bar, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_bar_set_range(osd_bar, 0, 100);
    lv_obj_add_flag(osd_box, LV_OBJ_FLAG_HIDDEN);

    // dimmed: on the top layer over everything, black with the clock or see-through; takes the waking touch
    wake_obj = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(wake_obj);
    lv_obj_set_size(wake_obj, DECK_W, DECK_H);
    lv_obj_remove_flag(wake_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wake_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(wake_obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wake_obj, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(wake_obj, wake_event, LV_EVENT_ALL, NULL);
    // 120 px font of its own (digits and ':' only, tools/clockfont.py): a zoomed 48 px font was computed in
    // software on every redraw and loaded the PSRAM, which caused video underruns
    clock_lbl = text(wake_obj, &font_clock_120, COL_TEXT, "");
    lv_obj_align(clock_lbl, LV_ALIGN_CENTER, 0, -50);
    date_lbl = text(wake_obj, &lv_font_montserrat_32, COL_DIM, "");
    lv_obj_align(date_lbl, LV_ALIGN_CENTER, 0, 110);
    batt_lbl = text(wake_obj, &lv_font_montserrat_18, COL_DIM, "");
    lv_obj_set_style_text_align(batt_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(batt_lbl, BATT_X, BATT_Y);                  // like the battery in the CNC status bar
    lv_obj_add_flag(batt_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wake_obj, LV_OBJ_FLAG_HIDDEN);

    apply_theme();
    drawn_ok = true;    // draw_wait() sets the start state
    draw_page();
    lv_obj_add_flag(page_obj, LV_OBJ_FLAG_HIDDEN);
}

void deck_set_mode (bool on)
{
    if(!BIGDECK)
        on = false;
    if(on == mode_on)
        return;
    mode_on = on;
    wifi_deck_mode(on);
    if(!on)
        wake_up();
    if(page_obj) {
        lv_obj_update_flag(page_obj, LV_OBJ_FLAG_HIDDEN, !on);
        if(on) {
            drawn_ok = false;       // draw_wait() again
            draw_wait();
        }
    }
    display_backlight(on ? bri_normal() * 100 / 255 : 100);
}

uint8_t deck_cnc_dim (void)
{
    return cnc_dim_min;
}

void deck_set_cnc_dim (uint8_t minutes)
{
    cnc_dim_min = minutes;
    prefs.putUChar("cncdim", minutes);
    lv_display_trigger_activity(NULL);
}

bool deck_mode (void)
{
    return mode_on;
}

bool deck_app_connected (void)
{
    return pc_ok();
}

// User interface (LVGL), style B3 "Electric"
//
// Screens: main (map + position, six buttons that change between idle and job), zoom (map
// fullscreen with STOP), menu, move, probe, laser and SD card.
// Buttons fire on tap, on press (HOLD, reacts at once) or after being held (machine moves,
// offsets, abort), a bar inside the button shows the hold time.

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include "ui.h"
#include "link.h"
#include "map.h"
#include "theme.h"
#include "beep.h"
#include "wlan.h"

#define LONG_PRESS_MS       600     // buttons that move the machine or change offsets
#define FORCE_UNLOCK_MS    3000     // unlock without homing ($22 override locks)
#define MAP_REDRAW_MS       300
#define PROBE_MAX_DIST     50.0f    // max. probing distance (mm), limited further by the work envelope
#define PROBE_RETRACT       5.0f
#define JOG_FEED          3000      // mm/min, grblHAL limits it to the axis max. rate

#define SCR_W   480
#define SCR_H   320
#define CARD_H  192     // map and position cards
#define ROW_Y   242     // main button row
#define ROW_H    70

enum action_t {
    A_None = -1,
    A_Menu, A_Back, A_Home, A_Unlock, A_Reset, A_ForceUnlock, A_ZeroXY, A_ZeroZ,
    A_Hold, A_Resume, A_Load, A_Start, A_FeedDn, A_FeedUp, A_FeedReset, A_PwrDn, A_PwrUp, A_PwrReset,
    A_Zoom, A_ZoomBack, A_ZoomStop, A_View,
    A_ScrMove, A_ScrProbe, A_ScrLaser, A_ScrSd,
    A_JogXm, A_JogXp, A_JogYm, A_JogYp, A_JogZm, A_JogZp,
    A_Step0, A_Step1, A_Step2, A_Step3, A_Step4,
    A_PlateDn, A_PlateUp, A_PFeedDn, A_PFeedUp, A_Probe,
    A_LPwrDn, A_LPwrUp, A_LTimeDn, A_LTimeUp, A_Fire,
    A_SdRefresh, A_SdRun, A_SdPick,
    A_ScrSettings, A_Key, A_KeyDel, A_KeyClr, A_KeyNow, A_Save, A_JobReset,
    A_ScrWifi, A_WifiToggle, A_WifiSave, A_WifiScan
};

enum screen_t { S_Main, S_Zoom, S_Menu, S_Move, S_Probe, S_Laser, S_Sd, S_SetGroups, S_SetList, S_SetEdit, S_Wifi, S_Count };

struct ui_btn_t {
    lv_obj_t *obj, *icon, *text, *bar;
    int tap, hold;          // actions
    uint16_t hold_ms;
    bool tap_on_press;
    bool fired;
    uint32_t t0;
    uint32_t c1, c2;        // gradient colors, both 0 = glass
    int arg;
};

static const float step_val[] = { 0.01f, 0.05f, 0.1f, 1.0f, 10.0f };
static const uint8_t laser_pwr_val[] = { 1, 2, 3, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100 };
static const uint16_t laser_time_val[] = { 10, 20, 50, 100, 200, 500, 1000, 2000 };
static const uint16_t probe_feed_val[] = { 25, 50, 100, 150, 200, 300 };
#define N_VAL(a) (int)(sizeof(a) / sizeof(a[0]))

static Preferences prefs;
static struct {
    float plate;
    uint8_t probe_feed, laser_pwr, laser_time, step;
} cfg = { 0.0f, 2, 4, 3, 3 };

static ui_btn_t btns[64];
static int n_btns = 0;
static screen_t screen = S_Main;
static lv_obj_t *scr_obj[S_Count];

// top bar
static ui_btn_t *b_menu;
static lv_obj_t *pill, *pill_lbl, *lbl_feed, *lbl_pwr, *lbl_wcs, *lbl_link;

// main
static lv_obj_t *map_card, *map_canvas, *map_name, *dro_card, *dro_val[3], *dro_mach[3];
static lv_obj_t *prog_track, *prog_fill, *lbl_pct, *lbl_sub, *tool_h, *tool_v, *tool_c;
static ui_btn_t *row[6], *b_zoom, *b_view;
static bool view_preview = false;       // map card shows the G-code preview instead of the machine area
static uint16_t preview_seen = 0;
static uint16_t *map_buf = NULL;
static int map_w = 0, map_h = 0;
static map_view_t map_v;

// zoom
static lv_obj_t *zoom_canvas, *zoom_chip, *zoom_dot, *zoom_lbl, *ztool_h, *ztool_v, *ztool_c;
static ui_btn_t *b_zback, *b_stop;
static uint16_t *zoom_buf = NULL;
static map_view_t zoom_v;
static bool stop_req = false, spindle_stop_sent = false;

// move, probe, laser, sd
static lv_obj_t *move_pos, *move_step_lbl;
static ui_btn_t *b_step[N_VAL(step_val)];
static lv_obj_t *probe_plate, *probe_feed, *probe_msg, *laser_pwr, *laser_time, *laser_msg;
static lv_obj_t *sd_list, *sd_msg;

// message banner below the top bar, errors and refused commands
static lv_obj_t *banner, *banner_lbl;
static uint32_t banner_until = 0;       // 0 = until tapped
static bool job_error = false;          // an SD job ended with an error, grblHAL needs a reset
static uint16_t err_seen = 0, refused_seen = 0;

// settings
static lv_obj_t *grp_list, *grp_msg, *set_list, *set_title;
static lv_obj_t *ed_title, *ed_name, *ed_now, *ed_hint, *ed_entry;
static ui_btn_t *b_save, *b_dot, *b_minus;
static char set_group[24] = "";
static int set_cur = -1;                // index into setlist.item
static char edit_buf[24] = "";
static uint16_t setlist_seen = 0;
static bool set_verify = false;         // a changed value is waiting for its confirmation
static uint32_t set_verify_at = 0;
static bool move_from_main = false;     // Move was opened from the position card, back returns to main
static int pending_screen = -1;         // screen change requested from a list row event

// WiFi
static lv_obj_t *lbl_wifi, *wifi_status, *wifi_dd, *wifi_ta[4], *wifi_kb, *ota_box, *ota_bar, *ota_lbl;
static lv_obj_t *wifi_form;             // everything but the keyboard, moved up while typing
static bool wifi_scanning = false;
static ui_btn_t *b_wifi_toggle;
static ui_btn_t *b_sd_run;
static char sd_sel[64] = "";
static uint16_t files_seq = 0, msg_seq = 0;
static bool sd_dirty = false;

// state
static bool job_mode = false, unlock_pending = false;
static uint32_t job_t0 = 0, job_secs = 0, last_map = 0, last_state = STATE_IDLE;
static char msg[64] = "";

static enum { P_Off, P_Start, P_Busy, P_Done } probe_seq = P_Off;
static uint32_t probe_t0;

// ------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------

static inline lv_color_t C (uint32_t c)
{
    return lv_color_hex(c);
}

static lv_obj_t *box (lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);

    return o;
}

static lv_obj_t *glass (lv_obj_t *parent, int x, int y, int w, int h, int r)
{
    lv_obj_t *o = box(parent, x, y, w, h);

    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, C(COL_CARD), 0);
    lv_obj_set_style_bg_opa(o, OPA_CARD, 0);
    lv_obj_set_style_border_color(o, C(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(o, 40, 0);
    lv_obj_set_style_border_width(o, 1, 0);

    return o;
}

static void gradient (lv_obj_t *o, uint32_t c1, uint32_t c2, lv_grad_dir_t dir)
{
    lv_obj_set_style_bg_color(o, C(c1), 0);
    lv_obj_set_style_bg_grad_color(o, C(c2), 0);
    lv_obj_set_style_bg_grad_dir(o, dir, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static lv_obj_t *label (lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
    lv_label_set_text(l, text);

    return l;
}

static void set_text (lv_obj_t *l, const char *text)
{
    if(strcmp(lv_label_get_text(l), text))
        lv_label_set_text(l, text);
}

static void show (lv_obj_t *o, bool on)
{
    if(on)
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------------------------------------------------
// Buttons
// ------------------------------------------------------------------------

static void action_run (int action, int arg);

static void btn_event (lv_event_t *e)
{
    ui_btn_t *b = (ui_btn_t *)lv_event_get_user_data(e);

    switch(lv_event_get_code(e)) {

        case LV_EVENT_PRESSED:
            beep_click();
            b->t0 = millis();
            b->fired = false;
            if(b->tap_on_press && b->tap != A_None) {
                b->fired = true;
                action_run(b->tap, b->arg);
            }
            break;

        case LV_EVENT_PRESSING:
            if(b->hold != A_None && !b->fired) {
                uint32_t t = millis() - b->t0;
                if(t >= b->hold_ms) {
                    b->fired = true;
                    show(b->bar, false);
                    action_run(b->hold, b->arg);
                } else if(t > 120) {
                    show(b->bar, true);
                    lv_obj_set_width(b->bar, (lv_obj_get_style_width(b->obj, 0) - 20) * t / b->hold_ms);
                }
            }
            break;

        case LV_EVENT_RELEASED:
            show(b->bar, false);
            if(!b->fired && !b->tap_on_press && b->tap != A_None)
                action_run(b->tap, b->arg);
            b->fired = true;
            break;

        case LV_EVENT_PRESS_LOST:
            show(b->bar, false);
            b->fired = true;
            break;

        default:
            break;
    }
}

static void btn_colors (ui_btn_t *b, uint32_t c1, uint32_t c2)
{
    b->c1 = c1;
    b->c2 = c2;

    if(c1 || c2) {
        gradient(b->obj, c1, c2, LV_GRAD_DIR_VER);
        lv_obj_set_style_border_width(b->obj, 0, 0);
        lv_obj_set_style_text_color(b->obj, C(COL_TEXT), 0);
    } else {
        lv_obj_set_style_bg_color(b->obj, C(COL_CARD), 0);
        lv_obj_set_style_bg_grad_dir(b->obj, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_bg_opa(b->obj, OPA_CARD, 0);
        lv_obj_set_style_border_width(b->obj, 1, 0);
        lv_obj_set_style_text_color(b->obj, C(COL_TEXT), 0);
    }
}

// Icon above the text when the button has both, centered otherwise. Called again when
// btn_set() changes the content (the main row buttons start empty).
static void btn_layout (ui_btn_t *b)
{
    bool two = *lv_label_get_text(b->icon) && *lv_label_get_text(b->text);
    int h = lv_obj_get_style_height(b->obj, 0);

    lv_obj_align(b->icon, LV_ALIGN_CENTER, 0, two ? -h / 6 : 0);
    lv_obj_align(b->text, LV_ALIGN_CENTER, 0, two ? h / 4 : 0);
}

// icon and/or text, the fonts follow the button height
static ui_btn_t *btn (lv_obj_t *parent, int x, int y, int w, int h, int r, const char *icon, const char *text,
                      uint32_t c1, uint32_t c2, int tap, int hold = A_None, uint16_t hold_ms = LONG_PRESS_MS)
{
    ui_btn_t *b = &btns[n_btns++];

    b->obj = glass(parent, x, y, w, h, r);
    lv_obj_add_flag(b->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b->obj, 2);

    // disabled: plain glass, dimmed text
    lv_obj_set_style_bg_color(b->obj, C(COL_CARD), LV_STATE_DISABLED);
    lv_obj_set_style_bg_grad_dir(b->obj, LV_GRAD_DIR_NONE, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(b->obj, 90, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(b->obj, 1, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b->obj, C(COL_MUTED), LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(b->obj, 150, LV_STATE_DISABLED);
    // pressed: brighter edge
    lv_obj_set_style_border_width(b->obj, 2, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(b->obj, 200, LV_STATE_PRESSED);

    const lv_font_t *ifont = h >= 90 ? &lv_font_montserrat_28 : (h >= 56 ? &lv_font_montserrat_24 : &lv_font_montserrat_20);

    b->icon = lv_label_create(b->obj);
    lv_obj_set_style_text_font(b->icon, ifont, 0);
    lv_label_set_text(b->icon, icon ? icon : "");

    b->text = lv_label_create(b->obj);
    lv_obj_set_style_text_font(b->text, h >= 90 ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
    lv_label_set_text(b->text, text ? text : "");
    btn_layout(b);

    b->bar = box(b->obj, 10, h - 9, 0, 4);
    lv_obj_set_style_radius(b->bar, 2, 0);
    lv_obj_set_style_bg_color(b->bar, C(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(b->bar, LV_OPA_COVER, 0);
    show(b->bar, false);

    b->tap = tap;
    b->hold = hold;
    b->hold_ms = hold_ms;
    b->tap_on_press = false;
    b->fired = true;
    b->arg = 0;
    btn_colors(b, c1, c2);

    lv_obj_add_event_cb(b->obj, btn_event, LV_EVENT_ALL, b);

    return b;
}

static void btn_set (ui_btn_t *b, const char *icon, const char *text, uint32_t c1, uint32_t c2,
                     int tap, int hold = A_None, uint16_t hold_ms = LONG_PRESS_MS, bool on_press = false)
{
    bool two = *lv_label_get_text(b->icon) && *lv_label_get_text(b->text);

    set_text(b->icon, icon ? icon : "");
    set_text(b->text, text ? text : "");
    if(two != (*lv_label_get_text(b->icon) && *lv_label_get_text(b->text)))
        btn_layout(b);
    if(c1 != b->c1 || c2 != b->c2)
        btn_colors(b, c1, c2);
    b->tap = tap;
    b->hold = hold;
    b->hold_ms = hold_ms;
    b->tap_on_press = on_press;
}

static void btn_enable (ui_btn_t *b, bool on)
{
    if(on == lv_obj_has_state(b->obj, LV_STATE_DISABLED)) {
        if(on)
            lv_obj_remove_state(b->obj, LV_STATE_DISABLED);
        else
            lv_obj_add_state(b->obj, LV_STATE_DISABLED);
    }
}

// ------------------------------------------------------------------------
// Background
// ------------------------------------------------------------------------

static lv_image_dsc_t bg_dsc;

static void background (void)
{
    uint16_t *px = (uint16_t *)ps_malloc(SCR_W * SCR_H * 2);

    if(px == NULL) {
        lv_obj_set_style_bg_color(lv_screen_active(), C(COL_BG1), 0);
        return;
    }

    // vertical gradient with two soft color blobs, as in the mockups
    struct { float x, y, r; uint32_t c; } blob[2] = { { 70, 285, 240, COL_BLOB1 }, { 430, 0, 240, COL_BLOB2 } };

    for(int y = 0; y < SCR_H; y++) {
        float t = (float)y / SCR_H, r = 0, g = 0, b = 0;
        r = ((COL_BG1 >> 16) & 0xFF) * (1 - t) + ((COL_BG2 >> 16) & 0xFF) * t;
        g = ((COL_BG1 >> 8) & 0xFF) * (1 - t) + ((COL_BG2 >> 8) & 0xFF) * t;
        b = (COL_BG1 & 0xFF) * (1 - t) + (COL_BG2 & 0xFF) * t;
        for(int x = 0; x < SCR_W; x++) {
            float pr = r, pg = g, pb = b;
            for(auto &bl : blob) {
                float dx = x - bl.x, dy = y - bl.y, d2 = (dx * dx + dy * dy) / (bl.r * bl.r);
                float a = 0.90f * expf(-d2 * 1.6f);
                pr += (((bl.c >> 16) & 0xFF) - pr) * a;
                pg += (((bl.c >> 8) & 0xFF) - pg) * a;
                pb += ((bl.c & 0xFF) - pb) * a;
            }
            px[y * SCR_W + x] = (((int)pr & 0xF8) << 8) | (((int)pg & 0xFC) << 3) | ((int)pb >> 3);
        }
    }

    memset(&bg_dsc, 0, sizeof(bg_dsc));
    bg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    bg_dsc.header.w = SCR_W;
    bg_dsc.header.h = SCR_H;
    bg_dsc.header.stride = SCR_W * 2;
    bg_dsc.data_size = SCR_W * SCR_H * 2;
    bg_dsc.data = (const uint8_t *)px;

    lv_obj_t *img = lv_image_create(lv_screen_active());
    lv_image_set_src(img, &bg_dsc);
    lv_obj_set_pos(img, 0, 0);
}

// ------------------------------------------------------------------------
// Message banner
// ------------------------------------------------------------------------

static void banner_show (const char *text, uint32_t color, uint32_t ms)
{
    lv_label_set_text(banner_lbl, text);
    lv_obj_set_style_bg_color(banner, C(color), 0);
    show(banner, true);
    banner_until = ms ? millis() + ms : 0;
}

static void banner_hide (void)
{
    show(banner, false);
}

static const char *error_text (uint16_t code)
{
    switch(code) {
        case 1:  return "Expected command letter";
        case 2:  return "Bad number format";
        case 3:  return "Invalid $ statement";
        case 9:  return "Locked (alarm or job)";
        case 10: return "Soft limit";
        case 15: return "Jog beyond travel";
        case 16: return "Invalid jog command";
        case 20: return "Unsupported G-code";
        case 22: return "Feed rate missing";
        case 33: return "Invalid target";
        case 45: return "Limit switch engaged";
        case 46: return "Homing required";
        case 52: return "Setting value out of range";
        case 60: case 64: case 65: return "SD card not mounted";
        case 61: return "File read error";
        case 71: case 72: case 73: case 74: case 75: case 76: return "Expression error";
        default: return "";
    }
}

static void build_banner (lv_obj_t *root)
{
    banner = box(root, 8, 42, 464, 40);
    lv_obj_set_style_radius(banner, 12, 0);
    lv_obj_set_style_bg_opa(banner, 235, 0);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_CLICKABLE);
    banner_lbl = label(banner, &lv_font_montserrat_14, COL_TEXT, "");
    lv_label_set_long_mode(banner_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(banner_lbl, 440);
    lv_obj_center(banner_lbl);
    lv_obj_add_event_cb(banner, [](lv_event_t *e) { banner_hide(); }, LV_EVENT_CLICKED, NULL);
    show(banner, false);
}

// ------------------------------------------------------------------------
// Screens
// ------------------------------------------------------------------------

static void screen_show (screen_t s)
{
    screen = s;
    for(int i = 0; i < S_Count; i++)
        show(scr_obj[i], i == s);

    // top bar everywhere but in the zoom view, menu button goes back on sub-screens
    bool top = s != S_Zoom;
    show(b_menu->obj, top);
    show(pill, top);
    show(lbl_feed, top);
    show(lbl_pwr, top);
    show(lbl_wcs, top);
    show(lbl_link, top);
    show(lbl_wifi, top);
    if(s == S_Main)
        btn_set(b_menu, LV_SYMBOL_LIST, NULL, 0, 0, A_Menu);
    else
        btn_set(b_menu, LV_SYMBOL_LEFT, NULL, 0, 0, A_Back);

    if(s == S_SetGroups && !setlist.loading) {
        link_settings();
        set_text(grp_msg, "Loading...");
        setlist_seen = setlist.seq;
    }
    if(s == S_SetGroups || s == S_SetList)
        setlist_seen = setlist.seq - 1;     // rebuild the list
    if(s == S_SetEdit && set_cur >= 0)
        strlcpy(edit_buf, setlist.item[set_cur].value, sizeof(edit_buf));

    if(s == S_Wifi) {
        action_run(A_WifiScan, 0);
        lv_textarea_set_text(wifi_ta[1], wifi_cfg.pass);
        lv_textarea_set_text(wifi_ta[2], wifi_cfg.ota);
        lv_textarea_set_text(wifi_ta[3], wifi_host());
        show(wifi_kb, false);
        lv_obj_set_y(wifi_form, 0);
    }

    if(s == S_Sd) {
        files.count = 0;
        files.done = false;
        link_send("FL");
        set_text(sd_msg, "Loading...");
    }
    *msg = '\0';
    last_map = 0;
}

static lv_obj_t *crosshair_part (lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = box(parent, 0, 0, w, h);

    lv_obj_set_style_bg_color(o, C(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);

    return o;
}

static void crosshair (lv_obj_t *parent, int size, lv_obj_t **h, lv_obj_t **v, lv_obj_t **c)
{
    *h = crosshair_part(parent, size * 2 + 1, 2);
    *v = crosshair_part(parent, 2, size * 2 + 1);
    *c = box(parent, 0, 0, size / 2 * 2 + 1, size / 2 * 2 + 1);
    lv_obj_set_style_radius(*c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(*c, C(0xFFFFFF), 0);
    lv_obj_set_style_border_width(*c, 2, 0);
    lv_obj_set_style_shadow_color(*c, C(COL_RING1), 0);
    lv_obj_set_style_shadow_width(*c, size, 0);
    lv_obj_set_style_shadow_spread(*c, size / 3, 0);
    lv_obj_set_style_shadow_opa(*c, 200, 0);
}

static void crosshair_move (const map_view_t *v, lv_obj_t *h, lv_obj_t *vl, lv_obj_t *c, int ox, int oy)
{
    int px, py;
    bool on = mc.online && map_to_px(v, mc.mpos[0], mc.mpos[1], &px, &py);

    show(h, on);
    show(vl, on);
    show(c, on);
    if(on) {
        int hw = lv_obj_get_style_width(h, 0), vh = lv_obj_get_style_height(vl, 0), cw = lv_obj_get_style_width(c, 0);
        lv_obj_set_pos(h, ox + px - hw / 2, oy + py - 1);
        lv_obj_set_pos(vl, ox + px - 1, oy + py - vh / 2);
        lv_obj_set_pos(c, ox + px - cw / 2, oy + py - cw / 2);
    }
}

// Expand icon (two arrows) drawn with lines
static void expand_icon (lv_obj_t *parent)
{
    static lv_point_precise_t a[] = { {4, 20}, {20, 4} }, a1[] = { {12, 4}, {20, 4}, {20, 12} }, a2[] = { {4, 12}, {4, 20}, {12, 20} };
    lv_point_precise_t *pts[] = { a, a1, a2 };
    int n[] = { 2, 3, 3 };

    for(int i = 0; i < 3; i++) {
        lv_obj_t *l = lv_line_create(parent);
        lv_line_set_points(l, pts[i], n[i]);
        lv_obj_set_style_line_width(l, 2, 0);
        lv_obj_set_style_line_color(l, C(0xFFFFFF), 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    }
}

// The map card is exactly as wide as the machine area, the position card takes the rest.
static void layout_main (void)
{
    float x0, y0, x1, y1;
    map_extent(&x0, &y0, &x1, &y1);

    int h = CARD_H - 12, w = (int)lroundf(h * (x1 - x0) / (y1 - y0));
    w = constrain(w, 120, 260);
    if(w == map_w && h == map_h)
        return;

    map_w = w;
    map_h = h;
    free(map_buf);
    map_buf = (uint16_t *)ps_malloc(w * h * 2);
    lv_canvas_set_buffer(map_canvas, map_buf, w, h, LV_COLOR_FORMAT_RGB565);

    lv_obj_set_width(map_card, w + 12);
    lv_obj_set_pos(b_zoom->obj, w + 12 - 40, CARD_H - 40);
    lv_obj_set_pos(b_view->obj, w + 12 - 78, CARD_H - 40);
    lv_obj_set_pos(dro_card, 8 + w + 12 + 8, 42);
    int dw = SCR_W - 8 - (8 + w + 12 + 8);
    lv_obj_set_width(dro_card, dw);
    for(int i = 0; i < 3; i++)
        lv_obj_set_width(dro_val[i], dw - 50);
    lv_obj_set_width(prog_track, dw - 20);
    lv_obj_set_width(lbl_sub, dw - 80);
    last_map = 0;
}

static void build_topbar (lv_obj_t *s)
{
    b_menu = btn(s, 8, 6, 40, 30, 10, LV_SYMBOL_LIST, NULL, 0, 0, A_Menu);

    pill = box(s, 56, 6, 116, 30);
    lv_obj_set_style_radius(pill, 15, 0);
    lv_obj_set_style_shadow_width(pill, 14, 0);
    lv_obj_set_style_shadow_opa(pill, 120, 0);
    gradient(pill, COL_STATE1, COL_STATE2, LV_GRAD_DIR_HOR);
    pill_lbl = label(pill, &lv_font_montserrat_16, COL_TEXT, "");
    lv_obj_center(pill_lbl);

    lbl_feed = label(s, &lv_font_montserrat_16, 0xEBEEFA, "F 0");
    lv_obj_set_pos(lbl_feed, 186, 12);
    lbl_pwr = label(s, &lv_font_montserrat_16, COL_PWR, "");
    lv_obj_set_pos(lbl_pwr, 268, 12);
    lbl_wcs = label(s, &lv_font_montserrat_16, COL_WCS, "G54");
    lv_obj_set_pos(lbl_wcs, 370, 12);
    lbl_wifi = label(s, &lv_font_montserrat_16, 0x50506E, LV_SYMBOL_WIFI);
    lv_obj_set_pos(lbl_wifi, 420, 12);
    lbl_link = label(s, &lv_font_montserrat_16, COL_RED1, LV_SYMBOL_CLOSE);
    lv_obj_set_pos(lbl_link, 450, 12);
}

static void build_main (lv_obj_t *s)
{
    static const uint32_t ax_col[3] = { COL_X, COL_Y, COL_Z };

    map_card = glass(s, 8, 42, 190, CARD_H, 16);
    map_canvas = lv_canvas_create(map_card);
    lv_obj_set_pos(map_canvas, 6, 6);
    lv_obj_set_style_radius(map_canvas, 10, 0);
    lv_obj_set_style_clip_corner(map_canvas, true, 0);
    crosshair(map_card, 9, &tool_h, &tool_v, &tool_c);
    map_name = label(map_card, &lv_font_montserrat_12, 0xD2D7F0, "");
    lv_obj_set_pos(map_name, 12, 9);
    b_zoom = btn(map_card, 150, CARD_H - 40, 32, 32, 16, NULL, NULL, 0, 0, A_Zoom);
    lv_obj_set_style_bg_color(b_zoom->obj, C(0x141632), 0);
    lv_obj_set_style_bg_opa(b_zoom->obj, 220, 0);
    expand_icon(b_zoom->obj);
    b_view = btn(map_card, 112, CARD_H - 40, 32, 32, 16, LV_SYMBOL_IMAGE, NULL, 0, 0, A_View);
    lv_obj_set_style_bg_color(b_view->obj, C(0x141632), 0);
    lv_obj_set_style_bg_opa(b_view->obj, 220, 0);
    lv_obj_set_style_text_font(b_view->icon, &lv_font_montserrat_14, 0);

    dro_card = glass(s, 206, 42, 266, CARD_H, 16);
    // tapping the position card opens the Move screen
    lv_obj_add_flag(dro_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(dro_card, 2, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(dro_card, 200, LV_STATE_PRESSED);
    lv_obj_add_event_cb(dro_card, [](lv_event_t *e) {
        beep_click();
        action_run(A_ScrMove, 0);
        move_from_main = true;
    }, LV_EVENT_CLICKED, NULL);
    for(int i = 0; i < 3; i++) {
        int y = 8 + i * 46;
        lv_obj_t *badge = box(dro_card, 10, y + 6, 30, 30);
        lv_obj_set_style_radius(badge, 7, 0);
        gradient(badge, ax_col[i], lv_color_to_u32(lv_color_darken(C(ax_col[i]), 90)) & 0xFFFFFF, LV_GRAD_DIR_VER);
        lv_obj_t *l = label(badge, &lv_font_montserrat_14, COL_TEXT, i == 0 ? "X" : (i == 1 ? "Y" : "Z"));
        lv_obj_center(l);

        dro_val[i] = label(dro_card, &lv_font_montserrat_36, COL_TEXT, "0.00");
        lv_obj_set_style_text_align(dro_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(dro_val[i], 216);
        lv_obj_set_pos(dro_val[i], 40, y);
        dro_mach[i] = label(dro_card, &lv_font_montserrat_12, COL_MUTED, "");
        lv_obj_set_pos(dro_mach[i], 46, y + 32);
        show(dro_mach[i], false);
    }

    prog_track = box(dro_card, 10, CARD_H - 36, 246, 8);
    lv_obj_set_style_radius(prog_track, 4, 0);
    lv_obj_set_style_bg_color(prog_track, C(COL_TRACK), 0);
    lv_obj_set_style_bg_opa(prog_track, LV_OPA_COVER, 0);
    prog_fill = box(prog_track, 0, 0, 0, 8);
    lv_obj_set_style_radius(prog_fill, 4, 0);
    gradient(prog_fill, COL_RING1, COL_RING2, LV_GRAD_DIR_HOR);

    lbl_pct = label(dro_card, &lv_font_montserrat_16, COL_TEXT, "");
    lv_obj_set_pos(lbl_pct, 10, CARD_H - 24);
    lbl_sub = label(dro_card, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_set_style_text_align(lbl_sub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(lbl_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_sub, 186);
    lv_obj_align(lbl_sub, LV_ALIGN_TOP_RIGHT, -10, CARD_H - 21);

    for(int k = 0; k < 6; k++)
        row[k] = btn(s, 8 + k * 78, ROW_Y, 72, ROW_H, 14, NULL, NULL, 0, 0, A_None);
}

static void build_zoom (lv_obj_t *s)
{
    zoom_buf = (uint16_t *)ps_malloc(SCR_W * SCR_H * 2);
    zoom_canvas = lv_canvas_create(s);
    lv_canvas_set_buffer(zoom_canvas, zoom_buf, SCR_W, SCR_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(zoom_canvas, 0, 0);
    crosshair(s, 16, &ztool_h, &ztool_v, &ztool_c);

    zoom_chip = glass(s, 140, 8, 200, 28, 14);
    lv_obj_set_style_bg_color(zoom_chip, C(0x141632), 0);
    lv_obj_set_style_bg_opa(zoom_chip, 210, 0);
    zoom_dot = box(zoom_chip, 12, 9, 10, 10);
    lv_obj_set_style_radius(zoom_dot, 5, 0);
    lv_obj_set_style_bg_opa(zoom_dot, LV_OPA_COVER, 0);
    zoom_lbl = label(zoom_chip, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_pos(zoom_lbl, 30, 5);

    b_zback = btn(s, 8, 8, 46, 46, 23, LV_SYMBOL_LEFT, NULL, 0, 0, A_ZoomBack);
    lv_obj_set_style_bg_color(b_zback->obj, C(0x141632), 0);
    lv_obj_set_style_bg_opa(b_zback->obj, 210, 0);

    b_stop = btn(s, 382, 222, 90, 90, 45, LV_SYMBOL_STOP, "STOP", COL_RED1, COL_RED2, A_ZoomStop);
    b_stop->tap_on_press = true;
    lv_obj_set_style_border_width(b_stop->obj, 2, 0);
    lv_obj_set_style_border_color(b_stop->obj, C(0xFFC8C8), 0);
    lv_obj_set_style_border_opa(b_stop->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(b_stop->obj, C(0xFF283C), 0);
    lv_obj_set_style_shadow_width(b_stop->obj, 24, 0);
    lv_obj_set_style_shadow_opa(b_stop->obj, 160, 0);
}

static void build_menu (lv_obj_t *s)
{
    struct { const char *icon, *text; uint32_t c1, c2; int tap, hold; } t[] = {
        { LV_SYMBOL_GPS,      "Move",    COL_FEED1, COL_FEED2, A_ScrMove,  A_None },
        { LV_SYMBOL_DOWNLOAD, "Probe Z", COL_FEED1, COL_FEED2, A_ScrProbe, A_None },
        { LV_SYMBOL_CHARGE,   "Laser",   COL_PWR1,  COL_PWR2,  A_ScrLaser, A_None },
        { LV_SYMBOL_SD_CARD,  "SD card", COL_PWR1,  COL_PWR2,  A_ScrSd,    A_None },
        { LV_SYMBOL_SETTINGS, "Settings", COL_GREEN1, COL_GREEN2, A_ScrSettings, A_None },
        { LV_SYMBOL_WIFI,     "WiFi",    0, 0,                 A_ScrWifi,  A_None },
    };

    for(int i = 0; i < 6; i++)
        btn(s, 8 + (i % 3) * 158, 56 + (i / 3) * 128, 148, 110, 16, t[i].icon, t[i].text, t[i].c1, t[i].c2, t[i].tap, t[i].hold);
}

static void build_move (lv_obj_t *s)
{
    move_pos = label(s, &lv_font_montserrat_20, COL_TEXT, "");
    lv_obj_set_pos(move_pos, 16, 48);

    btn(s, 88, 80, 72, 72, 16, LV_SYMBOL_UP, "Y+", COL_FEED1, COL_FEED2, A_JogYp);
    btn(s, 8, 158, 72, 72, 16, LV_SYMBOL_LEFT, "X-", COL_FEED1, COL_FEED2, A_JogXm);
    btn(s, 168, 158, 72, 72, 16, LV_SYMBOL_RIGHT, "X+", COL_FEED1, COL_FEED2, A_JogXp);
    btn(s, 88, 236, 72, 72, 16, LV_SYMBOL_DOWN, "Y-", COL_FEED1, COL_FEED2, A_JogYm);
    btn(s, 256, 80, 72, 112, 16, LV_SYMBOL_UP, "Z+", COL_PWR1, COL_PWR2, A_JogZp);
    btn(s, 256, 198, 72, 110, 16, LV_SYMBOL_DOWN, "Z-", COL_PWR1, COL_PWR2, A_JogZm);

    move_step_lbl = label(s, &lv_font_montserrat_16, COL_MUTED, "");
    lv_obj_set_style_text_align(move_step_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(move_step_lbl, 72);
    lv_obj_set_pos(move_step_lbl, 88, 184);

    static const char *names[] = { "0.01 mm", "0.05 mm", "0.1 mm", "1 mm", "10 mm" };
    for(int i = 0; i < N_VAL(step_val); i++) {
        b_step[i] = btn(s, 344, 80 + i * 46, 128, 40, 12, NULL, names[i], 0, 0, A_Step0 + i);
    }
}

static lv_obj_t *value_row (lv_obj_t *s, int y, const char *name, int dn, int up)
{
    lv_obj_t *l = label(s, &lv_font_montserrat_16, COL_MUTED, name);
    lv_obj_set_pos(l, 16, y + 18);
    btn(s, 220, y, 64, 54, 14, LV_SYMBOL_MINUS, NULL, 0, 0, dn);
    lv_obj_t *v = label(s, &lv_font_montserrat_24, COL_TEXT, "");
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(v, 110);
    lv_obj_set_pos(v, 290, y + 13);
    btn(s, 406, y, 64, 54, 14, LV_SYMBOL_PLUS, NULL, 0, 0, up);

    return v;
}

static lv_obj_t *msg_label (lv_obj_t *s, int x, int y, int w)
{
    lv_obj_t *l = label(s, &lv_font_montserrat_16, COL_TEXT, "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);

    return l;
}

static void build_probe (lv_obj_t *s)
{
    probe_plate = value_row(s, 48, "Plate (mm)", A_PlateDn, A_PlateUp);
    probe_feed = value_row(s, 110, "Feed (mm/min)", A_PFeedDn, A_PFeedUp);
    btn(s, 8, 216, 200, 96, 16, LV_SYMBOL_DOWNLOAD, "Probe Z (hold)", COL_ORANGE1, COL_ORANGE2, A_None, A_Probe);
    probe_msg = msg_label(s, 224, 214, 248);
}

static void build_laser (lv_obj_t *s)
{
    laser_pwr = value_row(s, 48, "Power (%)", A_LPwrDn, A_LPwrUp);
    laser_time = value_row(s, 110, "Pulse (ms)", A_LTimeDn, A_LTimeUp);
    btn(s, 8, 216, 200, 96, 16, LV_SYMBOL_CHARGE, "Fire (hold)", COL_RED1, COL_RED2, A_None, A_Fire);
    laser_msg = msg_label(s, 224, 214, 248);
}

static lv_obj_t *scroll_list (lv_obj_t *s, int x, int y, int w, int h)
{
    lv_obj_t *l = glass(s, x, y, w, h, 16);

    lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(l, 6, 0);
    lv_obj_set_style_pad_row(l, 6, 0);

    return l;
}

static lv_obj_t *list_row (lv_obj_t *list, const char *left, const char *right, bool sel)
{
    lv_obj_t *r = lv_button_create(list);

    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 40);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_bg_color(r, C(sel ? COL_FEED2 : COL_CARD), 0);
    lv_obj_set_style_bg_opa(r, sel ? LV_OPA_COVER : 120, 0);
    lv_obj_set_style_bg_opa(r, 220, LV_STATE_PRESSED);

    lv_obj_t *l = label(r, &lv_font_montserrat_16, COL_TEXT, left);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, right ? 300 : 420);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);

    if(right) {
        lv_obj_t *v = label(r, &lv_font_montserrat_16, COL_PWR, right);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(v, 120);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
    }

    return r;
}

static void build_settings (lv_obj_t *groups, lv_obj_t *list, lv_obj_t *edit)
{
    grp_list = scroll_list(groups, 8, 44, 464, 268);
    grp_msg = label(groups, &lv_font_montserrat_16, COL_MUTED, "");
    lv_obj_align(grp_msg, LV_ALIGN_TOP_RIGHT, -16, 14);

    set_list = scroll_list(list, 8, 44, 464, 268);
    set_title = label(list, &lv_font_montserrat_16, COL_MUTED, "");
    lv_obj_align(set_title, LV_ALIGN_TOP_RIGHT, -16, 14);

    // edit: description on the left, keypad on the right
    ed_title = label(edit, &lv_font_montserrat_24, COL_TEXT, "");
    lv_obj_set_pos(ed_title, 12, 44);
    ed_name = msg_label(edit, 12, 74, 224);
    lv_obj_set_style_text_font(ed_name, &lv_font_montserrat_14, 0);
    ed_now = label(edit, &lv_font_montserrat_14, COL_PWR, "");
    lv_label_set_long_mode(ed_now, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ed_now, 160);
    lv_obj_set_pos(ed_now, 12, 116);
    btn(edit, 176, 108, 60, 30, 10, NULL, "Now", 0, 0, A_KeyNow);
    ed_hint = msg_label(edit, 12, 144, 224);
    lv_obj_set_style_text_font(ed_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ed_hint, C(COL_MUTED), 0);
    lv_obj_set_height(ed_hint, 44);

    lv_obj_t *entry = glass(edit, 8, 192, 228, 44, 12);
    ed_entry = label(entry, &lv_font_montserrat_24, COL_TEXT, "");
    lv_obj_set_style_text_align(ed_entry, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(ed_entry, 210);
    lv_obj_align(ed_entry, LV_ALIGN_RIGHT_MID, -8, 0);

    btn(edit, 8, 244, 72, 68, 14, LV_SYMBOL_BACKSPACE, NULL, 0, 0, A_KeyDel);
    btn(edit, 86, 244, 72, 68, 14, NULL, "C", 0, 0, A_KeyClr);
    b_save = btn(edit, 164, 244, 72, 68, 14, LV_SYMBOL_SAVE, "Save", COL_GREEN1, COL_GREEN2, A_Save);

    static const char keys[] = "789456123-0.";
    for(int i = 0; i < 12; i++) {
        char t[2] = { keys[i], 0 };
        ui_btn_t *k = btn(edit, 244 + (i % 3) * 78, 44 + (i / 3) * 68, 72, 62, 14, NULL, t, 0, 0, A_Key);
        lv_obj_set_style_text_font(k->text, &lv_font_montserrat_24, 0);
        k->arg = keys[i];
        if(keys[i] == '.')
            b_dot = k;
        if(keys[i] == '-')
            b_minus = k;
    }
}

static void groups_fill (void)
{
    lv_obj_clean(grp_list);

    for(int i = 0; i < setlist.count; i++) {
        int j = 0;
        while(j < i && strcmp(setlist.item[j].group, setlist.item[i].group))
            j++;
        if(j < i)
            continue;   // group already listed

        lv_obj_t *r = list_row(grp_list, setlist.item[i].group, NULL, false);
        lv_obj_add_event_cb(r, [](lv_event_t *e) {
            strlcpy(set_group, setlist.item[(int)(intptr_t)lv_event_get_user_data(e)].group, sizeof(set_group));
            pending_screen = S_SetList;
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    set_text(grp_msg, setlist.loading ? "Loading..." : "");
}

static void settings_fill (void)
{
    char left[64];

    lv_obj_clean(set_list);
    set_text(set_title, set_group);

    for(int i = 0; i < setlist.count; i++) {
        const setting_t *st = &setlist.item[i];
        if(strcmp(st->group, set_group))
            continue;
        snprintf(left, sizeof(left), "$%u  %s", st->id, st->name);
        lv_obj_t *r = list_row(set_list, left, st->value, false);
        lv_obj_add_event_cb(r, [](lv_event_t *e) {
            set_cur = (int)(intptr_t)lv_event_get_user_data(e);
            pending_screen = S_SetEdit;
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void edit_key (char c)
{
    size_t n = strlen(edit_buf);

    if(c == '<') {
        if(n)
            edit_buf[n - 1] = '\0';
    } else if(c == 'C')
        *edit_buf = '\0';
    else if(c == '-') {
        if(n == 0)
            strcpy(edit_buf, "-");
    } else if(c == '.') {
        if(!strchr(edit_buf, '.') && n < sizeof(edit_buf) - 1)
            strcat(edit_buf, n == 0 ? "0." : ".");
    } else if(n < sizeof(edit_buf) - 1) {
        edit_buf[n] = c;
        edit_buf[n + 1] = '\0';
    }
}

static void update_edit (void)
{
    char buf[96];
    const setting_t *st = set_cur >= 0 ? &setlist.item[set_cur] : NULL;
    bool may_save = mc.online && (mc.state == STATE_IDLE || (mc.state & STATE_ALARM));

    if(st == NULL)
        return;

    snprintf(buf, sizeof(buf), "$%u", st->id);
    set_text(ed_title, buf);
    set_text(ed_name, st->name);
    snprintf(buf, sizeof(buf), "Now: %s %s", st->value, st->unit);
    set_text(ed_now, buf);

    switch(st->type) {
        case Fmt_Bool:         strcpy(buf, "0 = off, 1 = on"); break;
        case Fmt_Bitfield:
        case Fmt_XBitfield:    snprintf(buf, sizeof(buf), "Bits 1, 2, 4..: %s", st->format); break;
        case Fmt_RadioButtons: snprintf(buf, sizeof(buf), "0, 1, 2..: %s", st->format); break;
        case Fmt_AxisMask:     strcpy(buf, "Bits: 1 = X, 2 = Y, 4 = Z"); break;
        default:
            if(*st->min || *st->max)
                snprintf(buf, sizeof(buf), "Range: %s .. %s", st->min, st->max);
            else
                *buf = '\0';
            break;
    }
    set_text(ed_hint, buf);
    set_text(ed_entry, edit_buf);

    btn_enable(b_save, may_save && st->editable && *edit_buf);
    btn_enable(b_dot, st->type == Fmt_Decimal);
    btn_enable(b_minus, st->type == Fmt_Decimal || st->type == Fmt_Integer);

    // after saving: ask for the value again and report whether it was taken
    if(set_verify_at && millis() >= set_verify_at) {
        set_verify_at = 0;
        link_setting(st->id);
    }
}

static void build_wifi (lv_obj_t *scr)
{
    static const char *names[4] = { "Network", "Password", "OTA password", "Name" };
    lv_obj_t *s = wifi_form = box(scr, 0, 0, SCR_W, SCR_H);

    wifi_status = label(s, &lv_font_montserrat_16, COL_TEXT, "");
    lv_label_set_long_mode(wifi_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(wifi_status, 330);
    lv_obj_set_pos(wifi_status, 16, 54);
    b_wifi_toggle = btn(s, 360, 44, 112, 40, 12, LV_SYMBOL_POWER, NULL, 0, 0, A_WifiToggle);

    wifi_kb = lv_keyboard_create(scr);
    lv_obj_set_size(wifi_kb, SCR_W, 170);
    lv_obj_align(wifi_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    show(wifi_kb, false);
    lv_obj_add_event_cb(wifi_kb, [](lv_event_t *e) {
        lv_event_code_t code = lv_event_get_code(e);
        if(code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
            show(wifi_kb, false);
            lv_obj_set_y(wifi_form, 0);
        }
    }, LV_EVENT_ALL, NULL);

    // the network is picked from a scan, only the passwords are typed
    wifi_dd = lv_dropdown_create(s);
    lv_obj_set_style_text_font(wifi_dd, &lv_font_montserrat_16, 0);
    lv_obj_set_size(wifi_dd, 206, 42);
    lv_obj_set_pos(wifi_dd, 146, 92);
    lv_dropdown_set_options(wifi_dd, "");
    btn(s, 360, 92, 112, 42, 12, LV_SYMBOL_REFRESH, "Scan", 0, 0, A_WifiScan);

    for(int i = 0; i < 4; i++) {
        lv_obj_t *l = label(s, &lv_font_montserrat_16, COL_MUTED, names[i]);
        lv_obj_set_pos(l, 16, 104 + i * 48);
        if(i == 0)
            continue;   // network: dropdown above
        lv_obj_t *ta = wifi_ta[i] = lv_textarea_create(s);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_password_mode(ta, false);   // visible while typing
        lv_textarea_set_max_length(ta, i == 1 ? 64 : 32);
        if(i == 3)
            lv_textarea_set_accepted_chars(ta, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-");
        lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
        lv_obj_set_size(ta, 206, 42);
        lv_obj_set_pos(ta, 146, 92 + i * 48);
        lv_obj_add_event_cb(ta, [](lv_event_t *e) {
            lv_obj_t *t = (lv_obj_t *)lv_event_get_target(e);
            lv_keyboard_set_textarea(wifi_kb, t);
            show(wifi_kb, true);
            lv_obj_move_foreground(wifi_kb);
            lv_obj_set_y(wifi_form, 96 - lv_obj_get_y(t));     // the field just above the keyboard
        }, LV_EVENT_FOCUSED, NULL);
    }

    btn(s, 360, 236, 112, 76, 16, LV_SYMBOL_SAVE, "Save", COL_GREEN1, COL_GREEN2, A_WifiSave);
}

// Full screen progress of a firmware update, drawn at once (the update blocks the main loop).
void ui_ota_progress (unsigned pct)
{
    char buf[40];

    if(ota_box == NULL) {
        ota_box = box(lv_screen_active(), 0, 0, SCR_W, SCR_H);
        lv_obj_set_style_bg_color(ota_box, C(COL_BG2), 0);
        lv_obj_set_style_bg_opa(ota_box, 235, 0);
        ota_lbl = label(ota_box, &lv_font_montserrat_24, COL_TEXT, "");
        lv_obj_align(ota_lbl, LV_ALIGN_CENTER, 0, -30);
        lv_obj_t *track = box(ota_box, 60, 170, 360, 14);
        lv_obj_set_style_radius(track, 7, 0);
        lv_obj_set_style_bg_color(track, C(COL_TRACK), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        ota_bar = box(track, 0, 0, 0, 14);
        lv_obj_set_style_radius(ota_bar, 7, 0);
        gradient(ota_bar, COL_RING1, COL_RING2, LV_GRAD_DIR_HOR);
    }

    lv_obj_move_foreground(ota_box);
    snprintf(buf, sizeof(buf), "Firmware update %u %%", pct);
    set_text(ota_lbl, buf);
    lv_obj_set_width(ota_bar, 360 * pct / 100);
}

static void build_sd (lv_obj_t *s)
{
    sd_list = glass(s, 8, 44, 340, 268, 16);
    lv_obj_add_flag(sd_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sd_list, LV_DIR_VER);
    lv_obj_set_flex_flow(sd_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sd_list, 6, 0);
    lv_obj_set_style_pad_row(sd_list, 6, 0);

    btn(s, 360, 44, 112, 72, 16, LV_SYMBOL_REFRESH, "Refresh", 0, 0, A_SdRefresh);
    sd_msg = msg_label(s, 360, 128, 112);
    lv_obj_set_style_text_font(sd_msg, &lv_font_montserrat_14, 0);
    b_sd_run = btn(s, 360, 212, 112, 100, 16, LV_SYMBOL_OK, "Load", COL_GREEN1, COL_GREEN2, A_SdRun);
}

static void sd_fill (void)
{
    lv_obj_clean(sd_list);

    for(int i = 0; i < files.count; i++) {
        lv_obj_t *r = lv_button_create(sd_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), 40);
        lv_obj_set_style_radius(r, 10, 0);
        bool sel = !strcmp(files.name[i], sd_sel);
        lv_obj_set_style_bg_color(r, C(sel ? COL_FEED2 : COL_CARD), 0);
        lv_obj_set_style_bg_opa(r, sel ? LV_OPA_COVER : 120, 0);
        lv_obj_t *l = label(r, &lv_font_montserrat_16, COL_TEXT, files.name[i]);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, 300);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        // the list is rebuilt in update_screen(), not while the row's own event runs
        lv_obj_add_event_cb(r, [](lv_event_t *e) {
            strlcpy(sd_sel, files.name[(int)(intptr_t)lv_event_get_user_data(e)], sizeof(sd_sel));
            link_preview(sd_sel);
            sd_dirty = true;
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    if(files.count < 0)
        set_text(sd_msg, "No SD card");
    else if(files.count == 0)
        set_text(sd_msg, "No files");
    else
        set_text(sd_msg, *sd_sel ? sd_sel : "Tap a file");
}

// ------------------------------------------------------------------------
// Actions
// ------------------------------------------------------------------------

static void save_cfg (void)
{
    prefs.putBytes("cfg", &cfg, sizeof(cfg));
}

static void jog (int axis, float dir)
{
    char cmd[48];

    snprintf(cmd, sizeof(cmd), "$J=G91G21%c%.3fF%d", "XYZ"[axis], dir * step_val[cfg.step], JOG_FEED);
    link_cmd(cmd);
}

static void probe_start (void)
{
    if(mc.flags & DL_FLAG_PROBE_ON) {
        strcpy(msg, "Probe already triggered!");
        return;
    }

    float dist = PROBE_MAX_DIST;
    if(mc.env_valid) {
        float room = mc.mpos[2] - mc.env_min[2] - 0.5f;
        if(room < dist)
            dist = room;
    }
    if(dist < 1.0f) {
        strcpy(msg, "Too close to Z min");
        return;
    }

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "G91G38.2Z-%.3fF%u", dist, probe_feed_val[cfg.probe_feed]);
    link_cmd(cmd);
    probe_seq = P_Start;
    probe_t0 = millis();
    strcpy(msg, "Probing...");
}

// G38.2 has been sent: wait until it has run, then set Z0 (plate thickness) and retract
static void probe_poll (void)
{
    if(probe_seq == P_Off)
        return;

    if(mc.state & (STATE_ALARM | STATE_ESTOP)) {
        probe_seq = P_Off;
        strcpy(msg, "Probe failed (alarm)");
        return;
    }

    switch(probe_seq) {
        case P_Start:
            if(mc.state != STATE_IDLE)
                probe_seq = P_Busy;
            else if(link_idle() && millis() - probe_t0 > 1500) {
                probe_seq = P_Off;
                link_cmd("G90");
                strcpy(msg, "Probe command rejected");
            }
            break;

        case P_Busy:
            if(mc.state == STATE_IDLE) {
                if(mc.flags & DL_FLAG_PROBE_OK) {
                    char cmd[48];
                    link_cmd("G90");
                    snprintf(cmd, sizeof(cmd), "G10L20P0Z%.3f", cfg.plate);
                    link_cmd(cmd);
                    snprintf(cmd, sizeof(cmd), "G91G0Z%.1f", PROBE_RETRACT);
                    link_cmd(cmd);
                    link_cmd("G90");
                    probe_seq = P_Done;
                } else {
                    probe_seq = P_Off;
                    link_cmd("G90");
                    strcpy(msg, "No contact");
                }
            }
            break;

        case P_Done:
            if(link_idle() && mc.state == STATE_IDLE) {
                probe_seq = P_Off;
                strcpy(msg, "Z0 set");
            }
            break;

        default:
            break;
    }
}

static void action_run (int action, int arg)
{
    char cmd[80];

    switch(action) {
        case A_Menu:        screen_show(S_Menu); break;
        case A_Back:
            screen_show(screen == S_Menu || (screen == S_Move && move_from_main) ? S_Main : screen == S_SetEdit ? S_SetList :
                        screen == S_SetList ? S_SetGroups : S_Menu);
            break;
        case A_ScrSettings: screen_show(S_SetGroups); break;
        case A_ScrWifi:     screen_show(S_Wifi); break;
        case A_WifiToggle:
            wifi_cfg.enabled = !wifi_cfg.enabled;
            wifi_apply();
            break;
        case A_WifiScan:
            wifi_scan_start();
            wifi_scanning = true;
            lv_dropdown_set_options(wifi_dd, "Searching...");
            break;
        case A_WifiSave:
            if(!wifi_scanning && lv_dropdown_get_option_count(wifi_dd) && wifi_scan_result() > 0)
                lv_dropdown_get_selected_str(wifi_dd, wifi_cfg.ssid, sizeof(wifi_cfg.ssid));
            strlcpy(wifi_cfg.pass, lv_textarea_get_text(wifi_ta[1]), sizeof(wifi_cfg.pass));
            strlcpy(wifi_cfg.ota, lv_textarea_get_text(wifi_ta[2]), sizeof(wifi_cfg.ota));
            wifi_set_host(lv_textarea_get_text(wifi_ta[3]));
            lv_textarea_set_text(wifi_ta[3], wifi_host());
            show(wifi_kb, false);
            lv_obj_set_y(wifi_form, 0);
            wifi_apply();
            banner_show(*wifi_cfg.ota ? "WiFi settings saved" : "Saved - updates are off without OTA password",
                         COL_GREEN2, 2500);
            break;
        case A_Key:         edit_key((char)arg); break;
        case A_KeyDel:      edit_key('<'); break;
        case A_KeyClr:      edit_key('C'); break;
        case A_KeyNow:
            if(set_cur >= 0)
                strlcpy(edit_buf, setlist.item[set_cur].value, sizeof(edit_buf));
            break;
        case A_Save:
            if(set_cur >= 0 && *edit_buf) {
                snprintf(cmd, sizeof(cmd), "$%u=%s", setlist.item[set_cur].id, edit_buf);
                link_cmd(cmd);
                set_verify = true;
                set_verify_at = millis() + 400;
            }
            break;
        case A_JobReset:
            link_rt(CMD_RESET);
            job_error = false;
            banner_hide();
            break;
        case A_Home:        link_cmd("$H"); screen_show(S_Main); break;
        case A_Unlock:      link_cmd("$X"); break;
        case A_Reset:       link_rt(CMD_RESET); break;
        case A_ForceUnlock: link_rt(CMD_RESET); unlock_pending = true; break;
        case A_ZeroXY:      link_cmd("G10L20P0X0Y0"); break;
        case A_ZeroZ:       link_cmd("G10L20P0Z0"); break;
        case A_Hold:        link_rt(CMD_FEED_HOLD); break;
        case A_Resume:      link_rt(CMD_CYCLE_START); break;
        case A_Load:        screen_show(S_Sd); break;
        case A_SdRun:       // the file is loaded, it starts with START (hold) on the main screen
            if(*sd_sel)
                screen_show(S_Main);
            break;
        case A_Start:
            if(*sd_sel) {
                snprintf(cmd, sizeof(cmd), "$F=/%s", sd_sel);
                link_cmd(cmd);
                screen_show(S_Main);
            }
            break;
        case A_FeedDn:      link_rt(CMD_OVR_FEED_MINUS); break;
        case A_FeedUp:      link_rt(CMD_OVR_FEED_PLUS); break;
        case A_FeedReset:   link_rt(CMD_OVR_FEED_RESET); break;
        case A_PwrDn:       link_rt(CMD_OVR_SPIN_MINUS); break;
        case A_PwrUp:       link_rt(CMD_OVR_SPIN_PLUS); break;
        case A_PwrReset:    link_rt(CMD_OVR_SPIN_RESET); break;
        case A_Zoom:        screen_show(S_Zoom); break;
        case A_ZoomBack:    screen_show(S_Main); break;
        case A_View:        view_preview = !view_preview; last_map = 0; break;
        case A_ZoomStop:
            // first tap: feed hold and laser off, then ABORT resets
            if((mc.state & STATE_HOLD) && (mc.flags & DL_FLAG_HOLD_DONE))
                link_rt(CMD_RESET);
            else {
                link_rt(CMD_FEED_HOLD);
                stop_req = true;
                spindle_stop_sent = false;
            }
            break;
        case A_ScrMove:     move_from_main = false; screen_show(S_Move); break;
        case A_ScrProbe:    screen_show(S_Probe); break;
        case A_ScrLaser:    screen_show(S_Laser); break;
        case A_ScrSd:       screen_show(S_Sd); break;
        case A_JogXm:       jog(0, -1); break;
        case A_JogXp:       jog(0, 1); break;
        case A_JogYm:       jog(1, -1); break;
        case A_JogYp:       jog(1, 1); break;
        case A_JogZm:       jog(2, -1); break;
        case A_JogZp:       jog(2, 1); break;
        case A_Step0: case A_Step1: case A_Step2: case A_Step3: case A_Step4:
            cfg.step = action - A_Step0;
            save_cfg();
            break;
        case A_PlateDn:     cfg.plate = max(0.0f, cfg.plate - 0.1f); save_cfg(); break;
        case A_PlateUp:     cfg.plate = min(50.0f, cfg.plate + 0.1f); save_cfg(); break;
        case A_PFeedDn:     if(cfg.probe_feed) cfg.probe_feed--; save_cfg(); break;
        case A_PFeedUp:     if(cfg.probe_feed < N_VAL(probe_feed_val) - 1) cfg.probe_feed++; save_cfg(); break;
        case A_Probe:       probe_start(); break;
        case A_LPwrDn:      if(cfg.laser_pwr) cfg.laser_pwr--; save_cfg(); break;
        case A_LPwrUp:      if(cfg.laser_pwr < N_VAL(laser_pwr_val) - 1) cfg.laser_pwr++; save_cfg(); break;
        case A_LTimeDn:     if(cfg.laser_time) cfg.laser_time--; save_cfg(); break;
        case A_LTimeUp:     if(cfg.laser_time < N_VAL(laser_time_val) - 1) cfg.laser_time++; save_cfg(); break;
        case A_Fire:
            snprintf(cmd, sizeof(cmd), "LF,%u,%u", laser_pwr_val[cfg.laser_pwr], laser_time_val[cfg.laser_time]);
            link_send(cmd);
            break;
        case A_SdRefresh:   screen_show(S_Sd); break;
        default:            break;
    }
}

// ------------------------------------------------------------------------
// Update
// ------------------------------------------------------------------------

static void state_info (char *text, uint32_t *c1, uint32_t *c2)
{
    *c1 = COL_STATE1;
    *c2 = COL_STATE2;

    if(!mc.online) {
        strcpy(text, "NO LINK");
        *c1 = *c2 = 0x50506E;
    } else if(mc.state & STATE_ESTOP) {
        strcpy(text, "E-STOP");
        *c1 = COL_RED1; *c2 = COL_RED2;
    } else if(mc.state & STATE_ALARM) {
        switch(mc.alarm) {
            case ALARM_HARD_LIMIT:      strcpy(text, "HARD LIM"); break;
            case ALARM_SOFT_LIMIT:      strcpy(text, "SOFT LIM"); break;
            case ALARM_ABORT_CYCLE:     strcpy(text, "ABORTED"); break;
            case ALARM_PROBE_INITIAL:
            case ALARM_PROBE_CONTACT:   strcpy(text, "PROBE ERR"); break;
            case ALARM_HOMING_RESET:
            case ALARM_HOMING_DOOR:
            case ALARM_HOMING_PULLOFF:
            case ALARM_HOMING_APPROACH: strcpy(text, "HOME FAIL"); break;
            case ALARM_ESTOP:           strcpy(text, "E-STOP"); break;
            case ALARM_HOMING_REQUIRED: strcpy(text, "NO HOME"); break;
            case ALARM_LIMITS_ENGAGED:  strcpy(text, "LIMIT ON"); break;
            default:                    sprintf(text, "ALARM %u", mc.alarm); break;
        }
        *c1 = COL_RED1; *c2 = COL_RED2;
    } else if(mc.state & STATE_SAFETY_DOOR) {
        strcpy(text, "DOOR");
        *c1 = COL_ORANGE1; *c2 = COL_ORANGE2;
    } else if(mc.state & STATE_HOMING) {
        strcpy(text, "Homing");
        *c1 = COL_PWR1; *c2 = COL_PWR2;
    } else if(mc.state & STATE_HOLD) {
        strcpy(text, "Hold");
        *c1 = COL_ORANGE1; *c2 = COL_ORANGE2;
    } else if(mc.state & STATE_TOOL_CHANGE) {
        strcpy(text, "Tool");
        *c1 = COL_ORANGE1; *c2 = COL_ORANGE2;
    } else if(mc.state & STATE_JOG)
        strcpy(text, "Jog");
    else if(mc.state & STATE_CYCLE)
        strcpy(text, "Running");
    else if(mc.state & STATE_CHECK_MODE) {
        strcpy(text, "Check");
        *c1 = COL_ORANGE1; *c2 = COL_ORANGE2;
    } else if(mc.state & STATE_SLEEP) {
        strcpy(text, "Sleep");
        *c1 = *c2 = 0x50506E;
    } else {
        strcpy(text, "Idle");
        *c1 = COL_GREEN1; *c2 = COL_GREEN2;
    }
}

static void update_topbar (void)
{
    static uint32_t pc1 = 1, pc2 = 1;
    char buf[48];
    uint32_t c1, c2;

    state_info(buf, &c1, &c2);
    set_text(pill_lbl, buf);
    if(c1 != pc1 || c2 != pc2) {
        pc1 = c1;
        pc2 = c2;
        gradient(pill, c1, c2, LV_GRAD_DIR_HOR);
        lv_obj_set_style_shadow_color(pill, C(c1), 0);
    }

    snprintf(buf, sizeof(buf), "F %.0f", mc.feed);
    set_text(lbl_feed, buf);
    if(mc.mode == 1 && mc.s_max > 0.0f)
        snprintf(buf, sizeof(buf), "P %.0f %%", mc.s * 100.0f / mc.s_max);
    else
        snprintf(buf, sizeof(buf), "S %.0f", mc.s);
    set_text(lbl_pwr, buf);
    snprintf(buf, sizeof(buf), "G%u", 54 + mc.wcs);
    set_text(lbl_wcs, buf);
    set_text(lbl_link, mc.online ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    // WiFi symbol: grey off, orange connecting, green connected, cyan with a sender connected over WiFi
    lv_obj_set_style_text_color(lbl_wifi, C(wifi_sender() ? COL_RING1 : wifi_state() == Wifi_Connected ? COL_GREEN1 :
                                             wifi_state() == Wifi_Connecting ? COL_ORANGE1 : 0x50506E), 0);
    lv_obj_set_style_text_color(lbl_link, C(mc.online ? COL_GREEN1 : COL_RED1), 0);
}

static void update_main_buttons (void)
{
    bool idle = mc.online && mc.state == STATE_IDLE;
    bool alarm = mc.online && (mc.state & (STATE_ALARM | STATE_ESTOP));
    bool held = !!(mc.state & (STATE_HOLD | STATE_TOOL_CHANGE));
    bool hold_done = (mc.state & STATE_HOLD) && (mc.flags & DL_FLAG_HOLD_DONE);

    if(job_mode) {
        btn_set(row[0], LV_SYMBOL_MINUS, "Feed", COL_FEED1, COL_FEED2, A_FeedDn, A_FeedReset);
        btn_set(row[1], LV_SYMBOL_PLUS, "Feed", COL_FEED1, COL_FEED2, A_FeedUp, A_FeedReset);
        btn_set(row[2], LV_SYMBOL_MINUS, "Power", COL_PWR1, COL_PWR2, A_PwrDn, A_PwrReset);
        btn_set(row[3], LV_SYMBOL_PLUS, "Power", COL_PWR1, COL_PWR2, A_PwrUp, A_PwrReset);
        for(int i = 0; i < 4; i++)
            btn_enable(row[i], mc.online);
    } else {
        btn_set(row[0], LV_SYMBOL_HOME, "Home", COL_FEED1, COL_FEED2, A_None, A_Home);
        btn_enable(row[0], idle || alarm);

        // RESET while a critical alarm waits for a reset, HOLD 3S while homing is required, UNLOCK otherwise.
        // After an SD job error grblHAL refuses jogs until a reset, RESET is offered then too.
        if(job_error && !alarm) {
            btn_set(row[1], LV_SYMBOL_WARNING, "Reset", COL_ORANGE1, COL_ORANGE2, A_JobReset);
            btn_enable(row[1], mc.online);
        } else if(mc.flags & DL_FLAG_BLOCKING) {
            btn_set(row[1], LV_SYMBOL_WARNING, "Reset", COL_RED1, COL_RED2, A_Reset);
            btn_enable(row[1], true);
        } else if(alarm && (mc.flags & DL_FLAG_HOMING_LOCK)) {
            btn_set(row[1], LV_SYMBOL_REFRESH, "Hold 3s", COL_ORANGE1, COL_ORANGE2, A_None, A_ForceUnlock, FORCE_UNLOCK_MS);
            btn_enable(row[1], !!(mc.flags & DL_FLAG_FORCE_UNLOCK));
        } else {
            btn_set(row[1], LV_SYMBOL_REFRESH, "Unlock", alarm ? COL_ORANGE1 : 0, alarm ? COL_ORANGE2 : 0, A_Unlock);
            btn_enable(row[1], alarm && !(mc.flags & DL_FLAG_LIMIT_ON));
        }

        btn_set(row[2], LV_SYMBOL_GPS, "X0 Y0", COL_FEED1, COL_FEED2, A_None, A_ZeroXY);
        btn_enable(row[2], idle);
        btn_set(row[3], LV_SYMBOL_DOWN, "Z0", COL_FEED1, COL_FEED2, A_None, A_ZeroZ);
        btn_enable(row[3], idle);
    }

    // HOLD while running, STOP (hold to abort) once the hold is complete
    if(hold_done)
        btn_set(row[4], LV_SYMBOL_STOP, "Stop", COL_RED1, COL_RED2, A_None, A_Reset);
    else
        btn_set(row[4], LV_SYMBOL_PAUSE, "Hold", COL_ORANGE1, COL_ORANGE2, A_Hold, A_None, LONG_PRESS_MS, true);
    btn_enable(row[4], hold_done || (mc.online && (mc.state & (STATE_CYCLE | STATE_JOG))));

    // RESUME after a hold, START (hold) with a selected file, LOAD (opens the file list) otherwise
    if(held) {
        btn_set(row[5], LV_SYMBOL_PLAY, "Resume", COL_GREEN1, COL_GREEN2, A_Resume);
        btn_enable(row[5], true);
    } else if(*sd_sel) {
        btn_set(row[5], LV_SYMBOL_PLAY, "Start", idle ? COL_GREEN1 : 0, idle ? COL_GREEN2 : 0, A_Load, A_Start);
        btn_enable(row[5], idle);
    } else {
        btn_set(row[5], LV_SYMBOL_SD_CARD, "Load", 0, 0, A_Load);
        btn_enable(row[5], idle);
    }
}

static void update_main (void)
{
    char buf[64];

    for(int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "%.2f", mc.wpos[i]);
        set_text(dro_val[i], buf);
    }

    if(job_mode) {
        bool sd = mc.pct >= 0;
        show(prog_track, sd);
        if(sd) {
            lv_obj_set_width(prog_fill, lv_obj_get_style_width(prog_track, 0) * mc.pct / 1000);
            snprintf(buf, sizeof(buf), "%d %%", mc.pct / 10);
        } else
            strcpy(buf, "");
        set_text(lbl_pct, buf);
        snprintf(buf, sizeof(buf), "%s  %lu:%02lu", sd ? "" : "Job", job_secs / 60, job_secs % 60);
        set_text(lbl_sub, buf);
        set_text(map_name, sd ? mc.job : "");
    } else {
        show(prog_track, false);
        set_text(lbl_pct, "");
        snprintf(buf, sizeof(buf), "work offset G%u", 54 + mc.wcs);
        set_text(lbl_sub, *sd_sel ? sd_sel : buf);
    }

    update_main_buttons();

    // a new preview switches to the preview view, the button toggles between preview and machine area
    if(preview.seq != preview_seen) {
        preview_seen = preview.seq;
        view_preview = preview.valid;
        last_map = 0;
    }
    bool pv = view_preview && preview.valid;
    btn_enable(b_view, preview.valid);
    set_text(b_view->icon, pv ? LV_SYMBOL_GPS : LV_SYMBOL_IMAGE);
    if(!job_mode)
        set_text(map_name, pv ? preview.name : (preview.loading ? "Loading preview..." : ""));

    layout_main();
    if(map_buf && (map_changed() || !last_map) && millis() - last_map >= MAP_REDRAW_MS) {
        if(pv)
            map_view_preview(&map_v, map_w, map_h);
        else
            map_view_full(&map_v, map_w, map_h);
        map_render(map_buf, &map_v, pv);     // 10 mm grid in the preview, 50 mm for the whole machine
        lv_obj_invalidate(map_canvas);
        last_map = millis();
    }
    crosshair_move(&map_v, tool_h, tool_v, tool_c, 6, 6);
}

static void update_zoom (void)
{
    char buf[64];
    bool hold_done = (mc.state & STATE_HOLD) && (mc.flags & DL_FLAG_HOLD_DONE);

    // after the STOP tap the hold comes to an end, then the laser/spindle is switched off
    if(stop_req && hold_done) {
        if((mc.flags & DL_FLAG_SPINDLE_ON) && !spindle_stop_sent) {
            link_rt(CMD_OVR_SPIN_STOP);
            spindle_stop_sent = true;
        }
        stop_req = false;
    }
    if(!(mc.state & STATE_HOLD))
        stop_req = false;

    if(hold_done) {
        btn_set(b_stop, LV_SYMBOL_CLOSE, "ABORT", COL_RED1, COL_RED2, A_ZoomStop);
        lv_obj_set_style_border_color(b_stop->obj, C(0xFFFFFF), 0);
        strcpy(buf, (mc.flags & DL_FLAG_SPINDLE_ON) ? "Stopped" : "Stopped  -  laser off");
        lv_obj_set_style_bg_color(zoom_dot, C(COL_ORANGE1), 0);
    } else {
        btn_set(b_stop, LV_SYMBOL_STOP, "STOP", COL_RED1, COL_RED2, A_ZoomStop);
        lv_obj_set_style_border_color(b_stop->obj, C(0xFFC8C8), 0);
        if(mc.pct >= 0)
            snprintf(buf, sizeof(buf), "%d %%  -  %lu:%02lu", mc.pct / 10, job_secs / 60, job_secs % 60);
        else
            snprintf(buf, sizeof(buf), "X %.1f  Y %.1f", mc.wpos[0], mc.wpos[1]);
        lv_obj_set_style_bg_color(zoom_dot, C(mc.state & STATE_HOLD ? COL_ORANGE1 : COL_RING1), 0);
    }
    b_stop->tap_on_press = true;
    set_text(zoom_lbl, buf);

    if(zoom_buf && (map_changed() || !last_map) && millis() - last_map >= MAP_REDRAW_MS) {
        map_view_zoom(&zoom_v, SCR_W, SCR_H);
        map_render(zoom_buf, &zoom_v, true);
        lv_obj_invalidate(zoom_canvas);
        last_map = millis();
    }
    crosshair_move(&zoom_v, ztool_h, ztool_v, ztool_c, 0, 0);
}

static void update_screen (void)
{
    char buf[64];
    bool idle = mc.online && mc.state == STATE_IDLE;

    switch(screen) {
        case S_Main:
            update_main();
            break;

        case S_Zoom:
            update_zoom();
            break;

        case S_Move:
            snprintf(buf, sizeof(buf), "X %.2f    Y %.2f    Z %.2f", mc.wpos[0], mc.wpos[1], mc.wpos[2]);
            set_text(move_pos, buf);
            snprintf(buf, sizeof(buf), "%g mm", step_val[cfg.step]);
            set_text(move_step_lbl, buf);
            for(int i = 0; i < N_VAL(step_val); i++)
                btn_set(b_step[i], NULL, lv_label_get_text(b_step[i]->text), i == cfg.step ? COL_GREEN1 : 0,
                         i == cfg.step ? COL_GREEN2 : 0, A_Step0 + i);
            for(int i = 0; i < n_btns; i++)
                if(btns[i].tap >= A_JogXm && btns[i].tap <= A_JogZp)
                    btn_enable(&btns[i], mc.online && (idle || (mc.state & STATE_JOG)));
            break;

        case S_Probe:
            snprintf(buf, sizeof(buf), "%.1f", cfg.plate);
            set_text(probe_plate, buf);
            snprintf(buf, sizeof(buf), "%u", probe_feed_val[cfg.probe_feed]);
            set_text(probe_feed, buf);
            set_text(probe_msg, msg);
            break;

        case S_Laser:
            snprintf(buf, sizeof(buf), "%u", laser_pwr_val[cfg.laser_pwr]);
            set_text(laser_pwr, buf);
            snprintf(buf, sizeof(buf), "%u", laser_time_val[cfg.laser_time]);
            set_text(laser_time, buf);
            if(mc.msg_seq != msg_seq) {
                msg_seq = mc.msg_seq;
                strlcpy(msg, mc.msg, sizeof(msg));
            }
            set_text(laser_msg, msg);
            break;

        case S_Wifi:
            if(wifi_scanning && wifi_scan_result() >= 0) {
                int n = wifi_scan_result();
                wifi_scanning = false;
                if(n == 0)
                    lv_dropdown_set_options(wifi_dd, *wifi_cfg.ssid ? wifi_cfg.ssid : "No network found");
                else {
                    lv_dropdown_clear_options(wifi_dd);
                    int sel = 0;
                    for(int i = 0; i < n; i++) {
                        lv_dropdown_add_option(wifi_dd, wifi_scan_ssid(i), LV_DROPDOWN_POS_LAST);
                        if(!strcmp(wifi_scan_ssid(i), wifi_cfg.ssid))
                            sel = i;
                    }
                    lv_dropdown_set_selected(wifi_dd, sel);
                }
            }
            switch(wifi_state()) {
                case Wifi_Off:        set_text(wifi_status, *wifi_cfg.ssid ? "WiFi off" : "WiFi off - enter the network"); break;
                case Wifi_Connecting: set_text(wifi_status, "Connecting..."); break;
                case Wifi_Connected:
                    snprintf(buf, sizeof(buf), "%s  %s.local%s", wifi_ip(), wifi_host(), wifi_sender() ? "  - sender" : "");
                    set_text(wifi_status, buf);
                    break;
            }
            set_text(b_wifi_toggle->text, wifi_cfg.enabled ? "On" : "Off");
            btn_layout(b_wifi_toggle);
            btn_colors(b_wifi_toggle, wifi_cfg.enabled ? COL_GREEN1 : 0, wifi_cfg.enabled ? COL_GREEN2 : 0);
            break;

        case S_SetGroups:
            if(setlist.seq != setlist_seen && !setlist.loading) {
                setlist_seen = setlist.seq;
                groups_fill();
            }
            break;

        case S_SetList:
            if(setlist.seq != setlist_seen && !setlist.loading) {
                setlist_seen = setlist.seq;
                settings_fill();
            }
            break;

        case S_SetEdit:
            if(set_verify && setlist.seq != setlist_seen && set_cur >= 0) {
                const setting_t *st = &setlist.item[set_cur];
                set_verify = false;
                if(fabsf(strtof(st->value, NULL) - strtof(edit_buf, NULL)) < 0.0005f)
                    banner_show("Saved", COL_GREEN2, 2000);
                else
                    banner_show("Not accepted", COL_RED2, 3000);
            }
            setlist_seen = setlist.seq;
            update_edit();
            break;

        case S_Sd:
            if((files.done && files.seq != files_seq) || sd_dirty) {
                files_seq = files.seq;
                sd_dirty = false;
                sd_fill();
            }
            btn_enable(b_sd_run, idle && *sd_sel);
            break;

        default:
            break;
    }
}

void ui_update (void)
{
    // job: from leaving idle into a cycle until idle again (holds included)
    bool job = !!(mc.state & (STATE_CYCLE | STATE_HOLD | STATE_TOOL_CHANGE));
    bool alarm = mc.online && (mc.state & (STATE_ALARM | STATE_ESTOP));
    static bool was_alarm = false;

    // beeps: three on an alarm, two when a job of at least 10 s has finished
    if(alarm && !was_alarm)
        beep(3);
    else if(!job && job_mode && !alarm && job_secs >= 10)
        beep(2, 150, 120);
    was_alarm = alarm;

    if(job && !job_mode)
        job_t0 = millis();
    if(job)
        job_secs = (millis() - job_t0) / 1000;
    job_mode = job;

    // forced unlock: the reset has cleared the homing lock, now unlock
    if(unlock_pending && !(mc.state & STATE_ALARM))
        unlock_pending = false;
    else if(unlock_pending && !(mc.flags & DL_FLAG_BLOCKING) && mc.alarm != ALARM_HOMING_REQUIRED && link_cmd("$X"))
        unlock_pending = false;

    // the zoom view follows a job that starts, the menus go back to the main screen
    if(job && last_state == STATE_IDLE && screen != S_Main && screen != S_Zoom)
        screen_show(S_Main);
    last_state = mc.state;

    // errors from the controller and command lines it did not accept
    if(mc.err_seq != err_seen) {
        char buf[96];
        err_seen = mc.err_seq;
        if(mc.err_job) {
            job_error = true;
            snprintf(buf, sizeof(buf), "Job stopped: error %u %s - press Reset", mc.err_code, error_text(mc.err_code));
            banner_show(buf, COL_RED2, 0);
        } else {
            snprintf(buf, sizeof(buf), "Error %u %s", mc.err_code, error_text(mc.err_code));
            banner_show(buf, COL_RED2, 4000);
        }
        beep(3);
    }
    if(mc.refused_seq != refused_seen) {
        char buf[96];
        refused_seen = mc.refused_seq;
        snprintf(buf, sizeof(buf), "Not accepted: %s", mc.refused);
        banner_show(buf, COL_ORANGE2, 4000);
        beep(3);
    }
    if(banner_until && millis() >= banner_until) {
        banner_until = 0;
        banner_hide();
    }
    if(job && job_error) {      // a new job runs, the reset has happened
        job_error = false;
        banner_hide();
    }

    if(pending_screen >= 0) {
        screen_show((screen_t)pending_screen);
        pending_screen = -1;
    }

    map_record();
    probe_poll();
    update_topbar();
    update_screen();
}

void ui_init (void)
{
    prefs.begin("ui", false);
    if(prefs.getBytesLength("cfg") == sizeof(cfg))
        prefs.getBytes("cfg", &cfg, sizeof(cfg));
    cfg.step = min<uint8_t>(cfg.step, N_VAL(step_val) - 1);
    cfg.probe_feed = min<uint8_t>(cfg.probe_feed, N_VAL(probe_feed_val) - 1);
    cfg.laser_pwr = min<uint8_t>(cfg.laser_pwr, N_VAL(laser_pwr_val) - 1);
    cfg.laser_time = min<uint8_t>(cfg.laser_time, N_VAL(laser_time_val) - 1);

    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, C(COL_BG2), 0);
    background();

    for(int i = 0; i < S_Count; i++)
        scr_obj[i] = box(root, 0, 0, SCR_W, SCR_H);

    build_main(scr_obj[S_Main]);
    build_zoom(scr_obj[S_Zoom]);
    build_menu(scr_obj[S_Menu]);
    build_move(scr_obj[S_Move]);
    build_probe(scr_obj[S_Probe]);
    build_laser(scr_obj[S_Laser]);
    build_sd(scr_obj[S_Sd]);
    build_settings(scr_obj[S_SetGroups], scr_obj[S_SetList], scr_obj[S_SetEdit]);
    build_wifi(scr_obj[S_Wifi]);
    build_topbar(root);
    build_banner(root);

    map_init();
    layout_main();
    screen_show(S_Main);
    ui_update();
}

/*
  display_link.c - serial link to an ESP32-S3 touch display for grblHAL

  Part of grblHAL-Plugin-Display, https://github.com/Steppge/grblHAL-Plugin-Display

  Copyright (c) 2026 Steppge

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  v0.1 - machine status to the display, realtime commands and command lines from it,
         SD card file list, laser test pulse, G-code preview, settings, error reports,
         sender (LightBurn) over WiFi on a second UART.

  An ESP32-S3 with an MKS TS35 (see esp32s3/) is connected to a serial port of the controller
  board (DISPLAY_LINK_STREAM; BTT Octopus Pro: USART2 = PD5 TX2 / PD6 RX2 header next to USB-C, stream instance 1).
  Lines in both directions end with an XOR checksum over the text between the start
  character and '*'.

  Controller to display, every DL_PERIOD_MS:

    $DS,<seq>,<state>,<alarm>,<flags>,<wx>,<wy>,<wz>,<mx>,<my>,<mz>,<feed>,<s>,<ovf>,<ovr>,<ovs>,<pct>,<mode>,<wcs>*<hh>

      state   grblHAL system state bits (hex), alarm: alarm code
      flags   (hex) DL_FLAG_xxx below
      w, m    work and machine position (mm), feed: current feed rate, s: programmed spindle speed/power
      ov      feed, rapid and spindle override (%), pct: SD job progress in 0.1 %, -1 = no SD job
      mode    $32 (0 spindle, 1 laser, 2 lathe), wcs: 0 = G54 ... 5 = G59

  and once a second the machine size, the work envelope (valid once homed, else all 0) and the
  name of the running SD job:

    $DI,<x travel>,<y travel>,<z travel>,<x min>,<y min>,<x max>,<y max>,<z min>,<s max>*<hh>
    $DN,<file name>*<hh>

  Answers and messages:

    $FL,<file name>*<hh>    one per G-code file on the SD card, after >FL
    $FE,<count>*<hh>        end of the file list (-1: no SD card)
    $MS,<text>*<hh>         message for the user, e.g. the result of a laser test pulse

  Display to controller:

    >RT,<hex>*<hh>          realtime command, e.g. 18 = reset, 21 = feed hold, 7E = cycle start
    >GC,<id>,<line>*<hh>    command line ($X, $H, $J=..., G-code), answered with
    $AK,<id>,<1|0>*<hh>     1 = queued, 0 = busy or refused, the display sends it again later
    >FL*<hh>                list the G-code files on the SD card
    >LF,<power %>,<ms>*<hh> laser test pulse, only when idle with the spindle/laser off
    >PV,<file name>*<hh>    preview of a G-code file on the SD card (only when idle), answered with
    $PB,<x0>,<y0>,<x1>,<y1>,<w>,<h>*<hh>   bounds of the burned/cut moves (work coordinates, mm)
                                            and bitmap size, w a multiple of 8
    $PR,<row>,<hex>*<hh>    bitmap row (row 0 = top = y1), MSB first
    $PE,<rows>*<hh>         end, 0 = nothing burned/cut, -1 = not possible (busy, no file)
    >SL*<hh>                all settings (as $$), answered one per line with
    $SV|<id>|<group>|<editable>|<datatype>|<value>|<unit>|<min>|<max>|<name>|<format>*<hh>
    $SE,<count>*<hh>        end of the settings list
    >SR,<id>*<hh>           one setting again (after it was changed with a $<id>=<value> command line)
    >HB*<hh>                heartbeat of the display, every 500 ms
    >TC,<1|0>*<hh>          a sender (LightBurn) has connected to / disconnected from the display over WiFi:
                            the second UART (DISPLAY_LINK_SENDER_STREAM) becomes / stops being the active
                            stream. A job running when the connection is lost is held, the laser switched off.

  Errors (status codes) of commands and SD jobs:

    $ER,<code>,<sd job>*<hh>   sd job = 1: an SD job was running and has been ended by the error

  Enabled with DISPLAY_LINK_ENABLE in my_machine.h, started from my_plugin_init() in my_plugin.c.
  Optional in my_machine.h: DISPLAY_LINK_STREAM (serial stream instance, default 1), DISPLAY_LINK_BAUD (115200),
  DISPLAY_LINK_SENDER_STREAM (UART for a sender over WiFi, default 2, 255 = none), DISPLAY_LINK_SENDER_BAUD (921600).
  Only grblHAL core APIs are used, so the plugin is not tied to the STM32F4 driver.
*/

#include "driver.h"

#if DISPLAY_LINK_ENABLE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grbl/hal.h"
#include "grbl/report.h"
#include "grbl/protocol.h"
#include "grbl/state_machine.h"
#include "grbl/system.h"
#include "grbl/gcode.h"
#include "grbl/stepper.h"
#include "grbl/spindle_control.h"
#include "grbl/stream.h"
#include "grbl/settings.h"

#if SDCARD_ENABLE
#include <strings.h>
#include "grbl/vfs.h"
#include "sdcard/sdcard.h"
#include "sdcard/fs_stream.h"
#endif

#define DL_VERSION "0.1"

#ifndef DISPLAY_LINK_STREAM
#define DISPLAY_LINK_STREAM     1
#endif
#ifndef DISPLAY_LINK_BAUD
#define DISPLAY_LINK_BAUD  115200
#endif
#ifndef DISPLAY_LINK_SENDER_STREAM
#define DISPLAY_LINK_SENDER_STREAM  2   // second UART for a sender (LightBurn) over WiFi, 255 = none
#endif
#ifndef DISPLAY_LINK_SENDER_BAUD
#define DISPLAY_LINK_SENDER_BAUD 921600
#endif
#define DL_PERIOD_MS          100   // status line interval
#define DL_INFO_MS           1000   // machine size and job name interval
#define DL_TIMEOUT_MS        1000   // no valid line for this long = display not connected

#define DL_FLAG_BLOCKING     0x01   // critical alarm (hard limit, e-stop) waits for a reset
#define DL_FLAG_HOLD_DONE    0x02   // feed hold has come to a stop
#define DL_FLAG_SPINDLE_ON   0x04
#define DL_FLAG_LIMIT_ON     0x08   // a limit switch is engaged and strict mode refuses $X
#define DL_FLAG_HOMING_LOCK  0x10   // alarm: homing required
#define DL_FLAG_FORCE_UNLOCK 0x20   // homing lock may be overridden ($22 override locks)
#define DL_FLAG_SD_JOB       0x40   // job runs from the SD card
#define DL_FLAG_PROBE_ON     0x80   // probe input triggered
#define DL_FLAG_PROBE_OK    0x100   // the last probe move made contact
#define DL_FLAG_LASER_TEST  0x200   // a laser test pulse is running

#define DL_FILES_MAX          100
#define DL_PULSE_MAX_MS      2000

static const io_stream_t *dl_stream = NULL;
static on_execute_realtime_ptr on_execute_realtime;
static on_report_options_ptr on_report_options;
static uint32_t last_rx = 0, last_status = 0, last_info = 0;
static uint16_t seq = 0;
static bool pulse_on = false;
static uint32_t pulse_start = 0, pulse_ms = 0;

// All received characters go into the input buffer, none is a realtime command.
static bool dl_rx_char (const uint8_t c)
{
    return false;
}

static void send_line (const char *body)
{
    char line[200];
    uint8_t sum = 0;

    for(const char *p = body; *p; p++)
        sum ^= (uint8_t)*p;

    snprintf(line, sizeof(line), "$%s*%02X\n", body, sum);
    dl_stream->write(line);
}

// ------------------------------------------------------------------------
// SD card file list and laser test pulse
// ------------------------------------------------------------------------

#if SDCARD_ENABLE

static bool file_name_ok (const char *name)
{
    static const char *const types[] = { "nc", "ngc", "ncc", "gcode", "gc", "tap", "txt", "cnc", "" };
    const char *ext = strrchr(name, '.');

    if(ext == NULL || strlen(name) > 60 || strpbrk(name, "?~!*,"))
        return false;

    ext++;
    for(uint_fast8_t i = 0; *types[i]; i++) {
        if(!strcasecmp(ext, types[i]))
            return true;
    }

    return false;
}

static void send_file_list (void)
{
    char body[80];
    int count = -1;
    vfs_dir_t *dir;
    vfs_dirent_t *dirent;

    // the card may be mounted on demand only, sdcard_getfs() mounts it
    if(sdcard_getfs() && (dir = vfs_opendir("/"))) {
        count = 0;
        while((dirent = vfs_readdir(dir)) && count < DL_FILES_MAX) {
            if(!dirent->st_mode.directory && file_name_ok(dirent->name)) {
                snprintf(body, sizeof(body), "FL,%s", dirent->name);
                send_line(body);
                count++;
            }
        }
        vfs_closedir(dir);
    }

    snprintf(body, sizeof(body), "FE,%d", count);
    send_line(body);
}

#else

static void send_file_list (void)
{
    send_line("FE,-1");
}

#endif

static void message (const char *text)
{
    char body[64];

    snprintf(body, sizeof(body), "MS,%s", text);
    send_line(body);
}

static void pulse_stop (void)
{
    spindle_ptrs_t *sp = spindle_get(0);

    if(sp)
        sp->set_state(sp, (spindle_state_t){0}, 0.0f);

    pulse_on = false;
}

static void pulse_start_cmd (char *args)
{
    char text[48];
    unsigned pct = 0, ms = 0;
    spindle_ptrs_t *sp = spindle_get(0);

    if(sscanf(args, "%u,%u", &pct, &ms) != 2 || pct > 100 || ms == 0 || ms > DL_PULSE_MAX_MS) {
        message("Invalid pulse");
        return;
    }

    if(state_get() != STATE_IDLE || sp == NULL || spindle_is_on() || pulse_on) {
        message("Laser busy");
        return;
    }

    spindle_state_t on = {0};
    on.on = On;

    sp->set_state(sp, on, sp->rpm_max * (float)pct / 100.0f);
    pulse_start = hal.get_elapsed_ticks();
    pulse_ms = ms;
    pulse_on = true;

    snprintf(text, sizeof(text), "Fired %u ms at %u %%", ms, pct);
    message(text);
}

static void pulse_poll (sys_state_t state)
{
    if(pulse_on && (hal.get_elapsed_ticks() - pulse_start >= pulse_ms || state != STATE_IDLE))
        pulse_stop();
}

// ------------------------------------------------------------------------
// G-code preview: the file is read twice while idle, a few hundred bytes per call of the
// realtime handler. Pass 1 finds the bounds of the moves with the laser/spindle on, pass 2
// draws them into a 1 bit bitmap, which is then sent row by row.
// ------------------------------------------------------------------------

#if SDCARD_ENABLE

#define PV_W_MAX        240         // bitmap size, the job is scaled to fit
#define PV_H_MAX        160
#define PV_BYTES_CALL   384         // file bytes read per realtime call
#define PV_ROWS_CALL      2         // bitmap rows sent per realtime call
#define PV_ARC_DEG     10.0f        // arcs are drawn as chords of this angle

typedef enum { Pv_Off, Pv_Bounds, Pv_Draw, Pv_Send } pv_phase_t;

static struct {
    pv_phase_t phase;
    vfs_file_t *file;
    char line[128];
    uint8_t len;
    bool comment;
    // modal state of the parser
    float x, y, scale;
    bool absolute, laser, inch;
    uint8_t motion;
    float s;
    // result
    bool any;
    float x0, y0, x1, y1;
    uint16_t w, h, row;
    uint8_t bits[PV_W_MAX / 8 * PV_H_MAX];
} pv;

static void pv_reset_parser (void)
{
    pv.x = pv.y = 0.0f;
    pv.absolute = true;
    pv.laser = pv.inch = false;
    pv.motion = 0;
    pv.s = 0.0f;
    pv.len = 0;
    pv.comment = false;
}

static void pv_stop (int result)
{
    char body[16];

    if(pv.file)
        vfs_close(pv.file);
    pv.file = NULL;
    pv.phase = Pv_Off;

    snprintf(body, sizeof(body), "PE,%d", result);
    send_line(body);
}

static void pv_point (float x, float y)
{
    if(pv.phase == Pv_Bounds) {
        if(!pv.any) {
            pv.x0 = pv.x1 = x;
            pv.y0 = pv.y1 = y;
            pv.any = true;
        } else {
            if(x < pv.x0) pv.x0 = x;
            if(x > pv.x1) pv.x1 = x;
            if(y < pv.y0) pv.y0 = y;
            if(y > pv.y1) pv.y1 = y;
        }
    } else {
        int px = (int)((x - pv.x0) * pv.scale), py = (int)((pv.y1 - y) * pv.scale);    // row 0 = top
        if(px >= 0 && py >= 0 && px < pv.w && py < pv.h)
            pv.bits[py * (pv.w / 8) + px / 8] |= 0x80 >> (px & 7);
    }
}

static void pv_segment (float xa, float ya, float xb, float yb)
{
    if(pv.phase == Pv_Bounds) {
        pv_point(xa, ya);
        pv_point(xb, yb);
    } else {
        float dx = xb - xa, dy = yb - ya;
        int n = (int)(fmaxf(fabsf(dx), fabsf(dy)) * pv.scale) + 1;
        for(int i = 0; i <= n; i++)
            pv_point(xa + dx * i / n, ya + dy * i / n);
    }
}

// Reads a decimal number. strtof() is not used: it would read "G0X10" as the hex number 0X10.
static bool pv_number (char **p, float *v)
{
    char *s = *p;
    bool neg = false, digits = false;
    float val = 0.0f, frac = 0.1f;

    if(*s == '-' || *s == '+')
        neg = *s++ == '-';

    while(*s >= '0' && *s <= '9') {
        val = val * 10.0f + (*s++ - '0');
        digits = true;
    }

    if(*s == '.') {
        s++;
        while(*s >= '0' && *s <= '9') {
            val += (*s++ - '0') * frac;
            frac *= 0.1f;
            digits = true;
        }
    }

    if(digits) {
        *v = neg ? -val : val;
        *p = s;
    }

    return digits;
}

static void pv_execute (void)
{
    char *p = pv.line;
    float x = pv.x, y = pv.y, i = 0.0f, j = 0.0f, f = pv.inch ? 25.4f : 1.0f;
    bool has_xy = false;

    if(*p == '[' || *p == '#' || *p == 'O')     // expressions, parameters and subroutines are not followed
        return;

    while(*p) {
        char letter = *p++;
        float v;

        if(!pv_number(&p, &v))
            continue;

        switch(letter) {
            case 'G':
                switch((int)(v * 10.0f + 0.5f)) {
                    case 0: case 10: case 20: case 30: pv.motion = (uint8_t)v; break;
                    case 200: pv.inch = true; f = 25.4f; break;
                    case 210: pv.inch = false; f = 1.0f; break;
                    case 900: pv.absolute = true; break;
                    case 910: pv.absolute = false; break;
                }
                break;
            case 'M':
                if(v == 3.0f || v == 4.0f)
                    pv.laser = true;
                else if(v == 5.0f || v == 2.0f || v == 30.0f)
                    pv.laser = false;
                break;
            case 'S': pv.s = v; break;
            case 'X': x = pv.absolute ? v * f : pv.x + v * f; has_xy = true; break;
            case 'Y': y = pv.absolute ? v * f : pv.y + v * f; has_xy = true; break;
            case 'I': i = v * f; break;
            case 'J': j = v * f; break;
        }
    }

    if(!has_xy)
        return;

    if(pv.motion && pv.laser && pv.s > 0.0f) {
        if(pv.motion == 1)
            pv_segment(pv.x, pv.y, x, y);
        else {
            // arc around the center (I, J relative to the start), drawn as chords
            float cx = pv.x + i, cy = pv.y + j, r = hypotf(i, j);
            float a0 = atan2f(pv.y - cy, pv.x - cx), a1 = atan2f(y - cy, x - cx);
            if(pv.motion == 2 && a1 >= a0) a1 -= 2.0f * M_PI;   // clockwise
            if(pv.motion == 3 && a1 <= a0) a1 += 2.0f * M_PI;   // counterclockwise
            int n = (int)(fabsf(a1 - a0) * 180.0f / M_PI / PV_ARC_DEG) + 1;
            float xa = pv.x, ya = pv.y;
            for(int k = 1; k <= n; k++) {
                float a = a0 + (a1 - a0) * k / n;
                float xb = k == n ? x : cx + r * cosf(a), yb = k == n ? y : cy + r * sinf(a);
                pv_segment(xa, ya, xb, yb);
                xa = xb;
                ya = yb;
            }
        }
    }

    pv.x = x;
    pv.y = y;
}

// Collects characters into upper case lines without comments and blanks.
static void pv_char (char c)
{
    if(c == '\n' || c == '\r') {
        pv.line[pv.len] = '\0';
        if(pv.len)
            pv_execute();
        pv.len = 0;
        pv.comment = false;
    } else if(c == ';')
        pv.comment = true;              // rest of the line
    else if(c == '(')
        pv.comment = true;
    else if(c == ')')
        pv.comment = false;
    else if(!pv.comment && c > ' ' && pv.len < sizeof(pv.line) - 1)
        pv.line[pv.len++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static void pv_start (const char *name)
{
    char path[72];

    if(pv.file)
        vfs_close(pv.file);
    pv.file = NULL;
    pv.phase = Pv_Off;

    snprintf(path, sizeof(path), "/%s", name);
    if(state_get() != STATE_IDLE || !sdcard_getfs() || (pv.file = vfs_open(path, "r")) == NULL) {
        pv_stop(-1);
        return;
    }

    pv.any = false;
    pv_reset_parser();
    pv.phase = Pv_Bounds;
}

static void pv_poll (sys_state_t state)
{
    if(pv.phase == Pv_Off)
        return;

    if(state != STATE_IDLE) {           // a job or jog has started: give up, the display asks again
        pv_stop(-1);
        return;
    }

    if(pv.phase == Pv_Send) {
        char body[16 + PV_W_MAX / 4];
        for(uint_fast8_t n = 0; n < PV_ROWS_CALL && pv.row < pv.h; n++, pv.row++) {
            char *p = body + sprintf(body, "PR,%u,", pv.row);
            const uint8_t *bits = &pv.bits[pv.row * (pv.w / 8)];
            for(uint_fast16_t k = 0; k < pv.w / 8; k++)
                p += sprintf(p, "%02X", bits[k]);
            send_line(body);
        }
        if(pv.row == pv.h)
            pv_stop(pv.h);
        return;
    }

    char buf[64];
    size_t total = 0, n = 0;

    while(total < PV_BYTES_CALL && (n = vfs_read(buf, 1, sizeof(buf), pv.file)) > 0) {
        for(size_t k = 0; k < n; k++)
            pv_char(buf[k]);
        total += n;
    }

    if(n > 0)
        return;     // more to read in the next call

    pv_char('\n');

    if(pv.phase == Pv_Bounds) {
        if(!pv.any) {
            pv_stop(0);     // nothing is burned/cut
            return;
        }
        float dx = fmaxf(pv.x1 - pv.x0, 0.1f), dy = fmaxf(pv.y1 - pv.y0, 0.1f);
        pv.scale = fminf((PV_W_MAX - 1) / dx, (PV_H_MAX - 1) / dy);
        pv.w = ((uint16_t)(dx * pv.scale) + 8) & ~7;
        pv.h = (uint16_t)(dy * pv.scale) + 1;
        if(pv.w > PV_W_MAX) pv.w = PV_W_MAX;
        if(pv.h > PV_H_MAX) pv.h = PV_H_MAX;
        memset(pv.bits, 0, sizeof(pv.bits));
        vfs_seek(pv.file, 0);
        pv_reset_parser();
        pv.phase = Pv_Draw;
    } else {
        char body[96];
        vfs_close(pv.file);
        pv.file = NULL;
        snprintf(body, sizeof(body), "PB,%.3f,%.3f,%.3f,%.3f,%u,%u", pv.x0, pv.y0, pv.x1, pv.y1, pv.w, pv.h);
        send_line(body);
        pv.row = 0;
        pv.phase = Pv_Send;
    }
}

#else

static void pv_start (const char *name)
{
    send_line("PE,-1");
}

static void pv_poll (sys_state_t state)
{
}

#endif // SDCARD_ENABLE

// ------------------------------------------------------------------------
// Errors and settings
// ------------------------------------------------------------------------

static status_message_ptr prev_status_message;
static uint32_t sd_job_seen = 0;    // ms, last time an SD job was running

// Errors go to the display too, also those of SD jobs (the file stream reports them after it has
// ended the job, a job that ran until shortly before counts).
static status_code_t dl_status_message (status_code_t status)
{
    if(status != Status_OK && status != Status_Handled && dl_stream) {
        char body[24];
        snprintf(body, sizeof(body), "ER,%u,%d", (unsigned)status, hal.get_elapsed_ticks() - sd_job_seen < 500 ? 1 : 0);
        send_line(body);
    }

    return prev_status_message ? prev_status_message(status) : status;
}

#define SET_MAX 220

typedef struct {
    const setting_detail_t *setting;
    uint8_t offset;
} set_entry_t;

static set_entry_t set_all[SET_MAX];
static uint16_t set_n = 0, set_sent = 0;
static bool set_sending = false;

static bool set_collect (const setting_detail_t *setting, uint_fast16_t offset, void *data)
{
    if(set_n < SET_MAX) {
        set_all[set_n].setting = setting;
        set_all[set_n].offset = (uint8_t)offset;
        set_n++;
    }

    return true;
}

static int set_cmp (const void *a, const void *b)
{
    const set_entry_t *ea = (const set_entry_t *)a, *eb = (const set_entry_t *)b;

    return (int)(ea->setting->id + ea->offset) - (int)(eb->setting->id + eb->offset);
}

// Same selection as $$, sorted by number.
static void settings_collect (void)
{
    setting_details_t *details = settings_get_details();

    set_n = 0;

    do {
        for(uint_fast16_t i = 0; i < details->n_settings; i++) {
            const setting_detail_t *s = &details->settings[i];
            bool hidden = (s->id == Setting_HomingFeedRate || s->id == Setting_HomingSeekRate)
                           ? settings.homing.flags.per_axis_feedrates
                           : s->flags.hidden;

            if(hidden || s->value == NULL || s->flags.increment)
                continue;

            if(s->group == Group_Axis0)
                settings_iterator(s, set_collect, NULL);    // one entry per axis
            else if(s->is_available == NULL || s->is_available(s, 0))
                set_collect(s, 0, NULL);
        }
    } while((details = details->next));

    qsort(set_all, set_n, sizeof(set_entry_t), set_cmp);
}

// $SV|<id>|<group>|<editable>|<datatype>|<value>|<unit>|<min>|<max>|<name>|<format>
static void send_setting (const set_entry_t *e)
{
    const setting_detail_t *s = e->setting;
    char body[200], group[24], name[48];
    const char *value = setting_get_value(s, e->offset);
    bool editable = s->datatype != Format_String && s->datatype != Format_Password && s->datatype != Format_IPv4;
    bool list = s->datatype == Format_Bitfield || s->datatype == Format_XBitfield || s->datatype == Format_RadioButtons;

    if(s->group == Group_Axis0) {
        snprintf(group, sizeof(group), "%s-axis", axis_letter[e->offset]);
        snprintf(name, sizeof(name), "%s%s", axis_letter[e->offset], s->name);
    } else {
        const setting_group_detail_t *d = setting_get_group_details(settings_normalize_group(s->group));
        snprintf(group, sizeof(group), "%s", d && d->name ? d->name : "Other");
        snprintf(name, sizeof(name), "%s", s->name);
    }

    snprintf(body, sizeof(body), "SV|%u|%s|%d|%u|%.24s|%.10s|%.10s|%.10s|%.40s|%.50s", (unsigned)(s->id + e->offset), group,
              editable ? 1 : 0, (unsigned)s->datatype, value ? value : "", s->unit ? s->unit : "",
              s->min_value ? s->min_value : "", s->max_value ? s->max_value : "", name,
              list && s->format ? s->format : "");

    // the checksum separator and line ends may not appear in the text
    for(char *p = body; *p; p++) {
        if(*p == '*' || *p == '\n' || *p == '\r')
            *p = ' ';
    }

    send_line(body);
}

static void settings_start (void)
{
    settings_collect();
    set_sent = 0;
    set_sending = true;
}

// One setting again after a change (>SR,<id>)
static void setting_resend (unsigned id)
{
    for(uint_fast16_t i = 0; i < set_n; i++) {
        if((unsigned)(set_all[i].setting->id + set_all[i].offset) == id) {
            send_setting(&set_all[i]);
            return;
        }
    }
}

static void settings_poll (void)
{
    if(!set_sending)
        return;

    for(uint_fast8_t n = 0; n < 2 && set_sent < set_n; n++)
        send_setting(&set_all[set_sent++]);

    if(set_sent == set_n) {
        char body[16];
        snprintf(body, sizeof(body), "SE,%u", set_n);
        send_line(body);
        set_sending = false;
    }
}

// ------------------------------------------------------------------------
// Sender over WiFi: the display passes a TCP connection (port 23) through to a second UART.
// While a sender is connected that UART is the active grblHAL stream, like telnet.
// ------------------------------------------------------------------------

static const io_stream_t *sender = NULL;
static bool sender_active = false, sender_stop = false;

// While the UART is not the active stream its input is dropped, realtime commands included.
static bool sender_drop (uint8_t c)
{
    return true;
}

static void sender_connect (bool on)
{
    if(sender == NULL || on == sender_active)
        return;

    if(on) {
        sender->reset_read_buffer();
        sender_active = stream_connect(sender);
    } else {
        // the connection is gone while a job runs: hold, the laser/spindle is switched off once the
        // machine stands (see sender_poll), the job can be resumed or aborted on the display
        if(state_get() & (STATE_CYCLE | STATE_JOG)) {
            grbl.enqueue_realtime_command(CMD_FEED_HOLD);
            sender_stop = true;
        }
        stream_disconnect(sender);
        sender->set_enqueue_rt_handler(sender_drop);
        sender_active = false;
    }
}

static void sender_poll (sys_state_t state)
{
    // no line from the display for too long: the WiFi connection cannot be up any more
    if(sender_active && hal.get_elapsed_ticks() - last_rx > DL_TIMEOUT_MS)
        sender_connect(false);

    if(sender_stop && (state & STATE_HOLD) && sys.holding_state == Hold_Complete) {
        if(spindle_is_on())
            grbl.enqueue_realtime_command(CMD_OVERRIDE_SPINDLE_STOP);
        sender_stop = false;
    }
    if(sender_stop && !(state & (STATE_CYCLE | STATE_HOLD | STATE_JOG)))
        sender_stop = false;
}

// ------------------------------------------------------------------------
// Receiving
// ------------------------------------------------------------------------

static void handle_line (char *line)
{
    char *star = strrchr(line, '*');    // a command line may contain '*' itself
    uint8_t sum = 0;

    if(star == NULL)
        return;

    *star = '\0';
    for(char *p = line; *p; p++)
        sum ^= (uint8_t)*p;

    if(strtoul(star + 1, NULL, 16) != sum)
        return;

    last_rx = hal.get_elapsed_ticks();

    if(!strncmp(line, "RT,", 3))
        grbl.enqueue_realtime_command((char)strtoul(line + 3, NULL, 16));

    else if(!strncmp(line, "GC,", 3)) {
        char *id = line + 3, *cmd = strchr(id, ','), ack[24];
        if(cmd) {
            *cmd++ = '\0';
            snprintf(ack, sizeof(ack), "AK,%.8s,%d", id, *cmd && grbl.enqueue_gcode(cmd) ? 1 : 0);
            send_line(ack);
        }
    }

    else if(!strcmp(line, "FL"))
        send_file_list();

    else if(!strncmp(line, "LF,", 3))
        pulse_start_cmd(line + 3);

    else if(!strncmp(line, "PV,", 3))
        pv_start(line + 3);

    else if(!strcmp(line, "SL"))
        settings_start();

    else if(!strncmp(line, "SR,", 3))
        setting_resend(strtoul(line + 3, NULL, 10));

    else if(!strncmp(line, "TC,", 3))
        sender_connect(line[3] == '1');
}

static void receive (void)
{
    static char line[100];
    static uint8_t len = 0;
    int32_t c;

    while((c = dl_stream->read()) != SERIAL_NO_DATA) {
        if(c == '>')
            len = 0;
        else if(c == '\n' || c == '\r') {
            line[len] = '\0';
            if(len > 4)
                handle_line(line);
            len = 0;
        } else if(len < sizeof(line) - 1)
            line[len++] = (char)c;
        else
            len = 0;    // too long: discard
    }
}

// ------------------------------------------------------------------------
// Sending
// ------------------------------------------------------------------------

static uint16_t status_flags (sys_state_t state)
{
    uint16_t flags = 0;

    if(sys.blocking_event)
        flags |= DL_FLAG_BLOCKING;
    if((state & STATE_HOLD) && sys.holding_state == Hold_Complete)
        flags |= DL_FLAG_HOLD_DONE;
    if(spindle_is_on())
        flags |= DL_FLAG_SPINDLE_ON;
    if(!sys.blocking_event && settings.limits.flags.hard_enabled && settings.limits.flags.check_at_init &&
        (limit_signals_merge(hal.limits.get_state()).value & sys.hard_limits.mask))
        flags |= DL_FLAG_LIMIT_ON;
    if(!sys.blocking_event && (state & STATE_ALARM) && sys.alarm == Alarm_HomingRequired)
        flags |= DL_FLAG_HOMING_LOCK;
    if(settings.homing.flags.override_locks)
        flags |= DL_FLAG_FORCE_UNLOCK;
    if(hal.probe.get_state && hal.probe.get_state().triggered)
        flags |= DL_FLAG_PROBE_ON;
    if(sys.flags.probe_succeeded)
        flags |= DL_FLAG_PROBE_OK;
    if(pulse_on)
        flags |= DL_FLAG_LASER_TEST;

    return flags;
}

static void send_status (sys_state_t state)
{
    char body[200];
    float mpos[N_AXIS], wpos[3];
    int pct = -1;
    uint16_t flags = status_flags(state);
    spindle_ptrs_t *spindle = spindle_get(0);

    system_convert_array_steps_to_mpos(mpos, sys.position);
    for(uint_fast8_t i = 0; i < 3; i++)
        wpos[i] = mpos[i] - gc_get_offset(i, true);

#if SDCARD_ENABLE
    stream_job_t *job;
    if((job = stream_get_job_info()) && job->size) {
        pct = (int)((uint64_t)job->pos * 1000 / job->size);
        flags |= DL_FLAG_SD_JOB;
        sd_job_seen = hal.get_elapsed_ticks();
    }
#endif

    snprintf(body, sizeof(body), "DS,%u,%X,%u,%X,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.0f,%u,%u,%u,%d,%u,%u",
              seq++, (unsigned)state, (unsigned)sys.alarm, (unsigned)flags,
              wpos[0], wpos[1], wpos[2], mpos[0], mpos[1], mpos[2],
              st_get_realtime_rate(), gc_state.modal.spindle[0].rpm,
              (unsigned)sys.override.feed_rate, (unsigned)sys.override.rapid_rate,
              (unsigned)(spindle && spindle->param ? spindle->param->override_pct : 100),
              pct, (unsigned)settings.mode, (unsigned)gc_state.modal.g5x_offset.id);
    send_line(body);
}

static void send_info (void)
{
    char body[120];
    spindle_ptrs_t *spindle = spindle_get(0);

    snprintf(body, sizeof(body), "DI,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f", -settings.axis[X_AXIS].max_travel,
              -settings.axis[Y_AXIS].max_travel, -settings.axis[Z_AXIS].max_travel,
              sys.work_envelope.min.values[X_AXIS], sys.work_envelope.min.values[Y_AXIS],
              sys.work_envelope.max.values[X_AXIS], sys.work_envelope.max.values[Y_AXIS],
              sys.work_envelope.min.values[Z_AXIS], spindle ? spindle->rpm_max : 0.0f);
    send_line(body);

    strcpy(body, "DN,");
#if SDCARD_ENABLE
    stream_job_t *job;
    if((job = stream_get_job_info()) && job->size)
        snprintf(body + 3, sizeof(body) - 3, "%s", job->name);
#endif
    send_line(body);
}

static void dl_realtime (sys_state_t state)
{
    uint32_t now = hal.get_elapsed_ticks();

    on_execute_realtime(state);

    receive();
    pulse_poll(state);
    pv_poll(state);
    settings_poll();
    sender_poll(state);

    if(now - last_status >= DL_PERIOD_MS) {
        last_status = now;
        send_status(state);
    }

    if(now - last_info >= DL_INFO_MS) {
        last_info = now;
        send_info();
    }
}

static void dl_report_options (bool newopt)
{
    on_report_options(newopt);

    if(!newopt)
        report_plugin("Display link", dl_stream == NULL ? DL_VERSION " (no serial port)" :
                       (hal.get_elapsed_ticks() - last_rx < DL_TIMEOUT_MS ?
                        (sender_active ? DL_VERSION " (connected, WiFi sender active)" : DL_VERSION " (connected)") :
                        DL_VERSION " (no display)"));
}

// ------------------------------------------------------------------------

void display_link_init (void)
{
    on_report_options = grbl.on_report_options;
    grbl.on_report_options = dl_report_options;

    if((dl_stream = stream_open_instance(DISPLAY_LINK_STREAM, DISPLAY_LINK_BAUD, dl_rx_char, "Display")) == NULL)
        return;

    on_execute_realtime = grbl.on_execute_realtime;
    grbl.on_execute_realtime = dl_realtime;

#if DISPLAY_LINK_SENDER_STREAM != 255
    sender = stream_open_instance(DISPLAY_LINK_SENDER_STREAM, DISPLAY_LINK_SENDER_BAUD, sender_drop, "WiFi sender");
#endif

    prev_status_message = grbl.report.status_message;
    grbl.report.status_message = dl_status_message;
}

#endif // DISPLAY_LINK_ENABLE

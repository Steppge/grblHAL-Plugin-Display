// Web interface, see web.h

#include <Arduino.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#include "web.h"
#include "wlan.h"
#include "link.h"
#include "map.h"
#include "ui.h"
#include "web_index.h"      // generated from web/index.html by web/build_web.py

#define JOG_FEED        3000    // mm/min, as on the display
#define JOG_MAX         100.0f  // largest jog step accepted (mm)
#define LOGIN_PAUSE_MS  2000    // after a wrong password further attempts are refused this long

static WebServer *srv = NULL;
static bool stop_req = false, spindle_stop_sent = false, unlock_pending = false;
static uint32_t unlock_at = 0, login_failed_at = 0;

// ------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------

// JSON string content: quotes, backslashes and control characters escaped
static void jstr (char *out, size_t n, const char *s)
{
    size_t i = 0;

    while(*s && i + 7 < n) {
        unsigned char c = (unsigned char)*s++;
        if(c == '"' || c == '\\') {
            out[i++] = '\\';
            out[i++] = c;
        } else if(c < 0x20)
            i += snprintf(out + i, n - i, "\\u%04x", c);
        else
            out[i++] = c;
    }
    out[i] = '\0';
}

static void reply (int code, const char *text)
{
    srv->sendHeader("Cache-Control", "no-store");
    srv->send(code, "text/plain", text);
}

static void reply_json (const char *json)
{
    srv->sendHeader("Cache-Control", "no-store");
    srv->send(200, "application/json", json);
}

// Token for the commands: derived from the OTA password and the chip, valid until the password changes
static String web_token (void)
{
    uint8_t mac[6], hash[32];
    char buf[96], hex[17];

    WiFi.macAddress(mac);
    snprintf(buf, sizeof(buf), "grblhal-web:%s:%02x%02x%02x%02x%02x%02x", wifi_cfg.ota, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    mbedtls_sha256((const unsigned char *)buf, strlen(buf), hash, 0);
    for(int i = 0; i < 8; i++)
        sprintf(hex + i * 2, "%02x", hash[i]);

    return String(hex);
}

static bool may_control (void)
{
    return wifi_web_mode() == Web_Control && *wifi_cfg.ota && srv->header("X-Token") == web_token();
}

static bool web_on (void)
{
    if(wifi_web_mode() == Web_Off) {
        reply(403, "The web interface is off (display: Menu - WiFi - Web)");
        return false;
    }

    return true;
}

// ------------------------------------------------------------------------
// Handlers
// ------------------------------------------------------------------------

static void h_index (void)
{
    if(!web_on())
        return;

    srv->sendHeader("Content-Encoding", "gzip");
    srv->sendHeader("Cache-Control", "no-cache");
    srv->send_P(200, "text/html", (const char *)web_index_gz, sizeof(web_index_gz));
}

static void h_status (void)
{
    char job[100], sel[140], ref[100], msg[140], host[70], b[1500];

    if(!web_on())
        return;

    jstr(job, sizeof(job), mc.job);
    jstr(sel, sizeof(sel), ui_selected_file());
    jstr(ref, sizeof(ref), mc.refused);
    jstr(msg, sizeof(msg), mc.msg);
    jstr(host, sizeof(host), wifi_host());

    snprintf(b, sizeof(b),
        "{\"on\":%d,\"st\":%lu,\"al\":%u,\"fl\":%u,\"w\":[%.3f,%.3f,%.3f],\"m\":[%.3f,%.3f,%.3f],"
        "\"f\":%.0f,\"s\":%.0f,\"ov\":[%u,%u,%u],\"pct\":%d,\"mode\":%u,\"wcs\":%u,\"job\":\"%s\","
        "\"tr\":[%.1f,%.1f,%.1f],\"env\":[%d,%.1f,%.1f,%.1f,%.1f,%.1f],\"smax\":%.0f,"
        "\"snd\":%d,\"web\":%u,\"ctl\":%d,\"pv\":%u,\"pvl\":%d,\"pvok\":%d,\"sel\":\"%s\","
        "\"es\":%u,\"ec\":%u,\"ej\":%d,\"rs\":%u,\"rf\":\"%s\",\"ms\":%u,\"msg\":\"%s\","
        "\"host\":\"%s\",\"xl\":%d,\"yd\":%d}",
        mc.online ? 1 : 0, (unsigned long)mc.state, mc.alarm, mc.flags,
        mc.wpos[0], mc.wpos[1], mc.wpos[2], mc.mpos[0], mc.mpos[1], mc.mpos[2],
        mc.feed, mc.s, mc.ov_feed, mc.ov_rapid, mc.ov_spindle, mc.pct, mc.mode, mc.wcs, job,
        mc.travel[0], mc.travel[1], mc.travel[2],
        mc.env_valid ? 1 : 0, mc.env_min[0], mc.env_min[1], mc.env_max[0], mc.env_max[1], mc.env_min[2], mc.s_max,
        wifi_sender() ? 1 : 0, wifi_web_mode(), may_control() ? 1 : 0, preview.seq, preview.loading ? 1 : 0,
        preview.valid ? 1 : 0, sel,
        mc.err_seq, mc.err_code, mc.err_job ? 1 : 0, mc.refused_seq, ref, mc.msg_seq, msg,
        host, MAP_X_LEFT, MAP_Y_DOWN);

    reply_json(b);
}

// {"v":1,"name":"..","x0":..,"y0":..,"x1":..,"y1":..,"w":..,"h":..,"seq":..,"rows":["hex",..]}
static void h_preview (void)
{
    char name[140], buf[200];

    if(!web_on())
        return;

    jstr(name, sizeof(name), preview.name);
    srv->sendHeader("Cache-Control", "no-store");
    srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
    srv->send(200, "application/json", "");

    snprintf(buf, sizeof(buf), "{\"v\":%d,\"name\":\"%s\",\"x0\":%.3f,\"y0\":%.3f,\"x1\":%.3f,\"y1\":%.3f,\"w\":%u,\"h\":%u,\"seq\":%u,\"rows\":[",
              preview.valid ? 1 : 0, name, preview.x0, preview.y0, preview.x1, preview.y1,
              preview.valid ? preview.w : 0, preview.valid ? preview.h : 0, preview.seq);
    srv->sendContent(buf);

    if(preview.valid) {
        char row[PREVIEW_W_MAX / 4 + 4];
        int bytes = preview.w / 8;
        for(int y = 0; y < preview.h; y++) {
            int n = sprintf(row, "%s\"", y ? "," : "");
            for(int i = 0; i < bytes; i++)
                n += sprintf(row + n, "%02x", preview.bits[y * bytes + i]);
            strcpy(row + n, "\"");
            srv->sendContent(row);
        }
    }

    srv->sendContent("]}");
    srv->sendContent("");
}

// {"done":1,"count":n,"names":[..]}, count -1 = no SD card
static void h_files (void)
{
    char buf[160];

    if(!web_on())
        return;

    if(srv->method() == HTTP_POST) {
        files.count = 0;
        files.done = false;
        link_send("FL");
        reply(200, "ok");
        return;
    }

    srv->sendHeader("Cache-Control", "no-store");
    srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
    srv->send(200, "application/json", "");

    snprintf(buf, sizeof(buf), "{\"done\":%d,\"count\":%d,\"names\":[", files.done ? 1 : 0, files.done ? files.count : 0);
    srv->sendContent(buf);
    for(int i = 0; files.done && i < files.count; i++) {
        char name[140];
        jstr(name, sizeof(name), files.name[i]);
        snprintf(buf, sizeof(buf), "%s\"%s\"", i ? "," : "", name);
        srv->sendContent(buf);
    }
    srv->sendContent("]}");
    srv->sendContent("");
}

static void h_login (void)
{
    char buf[48];

    if(!web_on())
        return;

    if(wifi_web_mode() != Web_Control || !*wifi_cfg.ota) {
        reply(403, "Control is off (display: Menu - WiFi - Web)");
        return;
    }
    if(login_failed_at && millis() - login_failed_at < LOGIN_PAUSE_MS) {
        reply(429, "Wait a moment");
        return;
    }
    if(srv->arg("pass") != wifi_cfg.ota) {
        login_failed_at = millis();
        reply(403, "Wrong password");
        return;
    }

    login_failed_at = 0;
    snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", web_token().c_str());
    reply_json(buf);
}

static bool file_listed (const String &name)
{
    for(int i = 0; files.done && i < files.count; i++) {
        if(name == files.name[i])
            return true;
    }

    return false;
}

// The same actions and conditions as the buttons on the display
static void h_cmd (void)
{
    char cmd[96];
    bool ok = false;

    if(!web_on())
        return;
    if(!may_control()) {
        reply(403, "View only");
        return;
    }
    if(!mc.online) {
        reply(409, "No link to the controller");
        return;
    }

    String a = srv->arg("a");
    bool idle = mc.state == STATE_IDLE;
    bool alarm = !!(mc.state & (STATE_ALARM | STATE_ESTOP));
    bool job = !!(mc.state & (STATE_CYCLE | STATE_HOLD | STATE_TOOL_CHANGE));
    bool hold_done = (mc.state & STATE_HOLD) && (mc.flags & DL_FLAG_HOLD_DONE);

    if(a == "home")
        ok = (idle || alarm) && link_cmd("$H");
    else if(a == "unlock")
        ok = alarm && !(mc.flags & DL_FLAG_LIMIT_ON) && link_cmd("$X");
    else if(a == "reset") {
        link_rt(CMD_RESET);
        ok = true;
    } else if(a == "force_unlock") {
        if((ok = alarm && (mc.flags & DL_FLAG_HOMING_LOCK) && (mc.flags & DL_FLAG_FORCE_UNLOCK))) {
            link_rt(CMD_RESET);
            unlock_pending = true;
            unlock_at = millis();
        }
    } else if(a == "zero_xy")
        ok = idle && link_cmd("G10L20P0X0Y0");
    else if(a == "zero_z")
        ok = idle && link_cmd("G10L20P0Z0");
    else if(a == "hold") {
        if((ok = !!(mc.state & (STATE_CYCLE | STATE_JOG))))
            link_rt(CMD_FEED_HOLD);
    } else if(a == "resume") {
        if((ok = !!(mc.state & (STATE_HOLD | STATE_TOOL_CHANGE))))
            link_rt(CMD_CYCLE_START);
    } else if(a == "stop") {
        // first: feed hold, the laser/spindle goes off once the machine stands; then: abort
        if(hold_done)
            link_rt(CMD_RESET);
        else {
            link_rt(CMD_FEED_HOLD);
            stop_req = true;
            spindle_stop_sent = false;
        }
        ok = true;
    } else if(a == "feed-" || a == "feed+" || a == "feed0" || a == "power-" || a == "power+" || a == "power0") {
        static const char *const names[] = { "feed-", "feed+", "feed0", "power-", "power+", "power0" };
        static const uint8_t rt[] = { CMD_OVR_FEED_MINUS, CMD_OVR_FEED_PLUS, CMD_OVR_FEED_RESET,
                                      CMD_OVR_SPIN_MINUS, CMD_OVR_SPIN_PLUS, CMD_OVR_SPIN_RESET };
        for(int i = 0; i < 6; i++) {
            if(a == names[i])
                link_rt(rt[i]);
        }
        ok = true;
    } else if(a == "jog") {
        String ax = srv->arg("ax");
        float d = srv->arg("d").toFloat();
        if(ax.length() == 1 && strchr("XYZ", ax[0]) && d != 0.0f && fabsf(d) <= JOG_MAX && (idle || (mc.state & STATE_JOG))) {
            snprintf(cmd, sizeof(cmd), "$J=G91G21%c%.3fF%d", ax[0], d, JOG_FEED);
            ok = link_cmd(cmd);
        }
    } else if(a == "jog_cancel") {
        link_rt(CMD_JOG_CANCEL);
        ok = true;
    } else if(a == "load") {
        String f = srv->arg("f");
        if((ok = idle && file_listed(f)))
            ui_select_file(f.c_str());
    } else if(a == "unload") {
        ui_select_file("");
        ok = true;
    } else if(a == "start") {
        if((ok = idle && !job && *ui_selected_file())) {
            snprintf(cmd, sizeof(cmd), "$F=/%s", ui_selected_file());
            ok = link_cmd(cmd);
        }
    } else {
        reply(400, "Unknown action");
        return;
    }

    reply(ok ? 200 : 409, ok ? "ok" : "Not possible now");
}

// ------------------------------------------------------------------------

void web_begin (WebServer &server)
{
    static const char *headers[] = { "X-Token" };

    srv = &server;
    server.collectHeaders(headers, 1);
    server.on("/", HTTP_GET, h_index);
    server.on("/api/status", HTTP_GET, h_status);
    server.on("/api/preview", HTTP_GET, h_preview);
    server.on("/api/files", HTTP_GET, h_files);
    server.on("/api/files", HTTP_POST, h_files);
    server.on("/api/login", HTTP_POST, h_login);
    server.on("/api/cmd", HTTP_POST, h_cmd);
}

void web_poll (void)
{
    bool hold_done = (mc.state & STATE_HOLD) && (mc.flags & DL_FLAG_HOLD_DONE);

    // STOP: once the hold has come to an end the laser/spindle is switched off
    if(stop_req && hold_done) {
        if((mc.flags & DL_FLAG_SPINDLE_ON) && !spindle_stop_sent) {
            link_rt(CMD_OVR_SPIN_STOP);
            spindle_stop_sent = true;
        }
        stop_req = false;
    }
    if(!(mc.state & STATE_HOLD) && !(mc.state & STATE_CYCLE))
        stop_req = false;

    // forced unlock: the reset has cleared the homing lock, now unlock
    if(unlock_pending) {
        if(!(mc.state & STATE_ALARM) || millis() - unlock_at > 5000)
            unlock_pending = false;
        else if(!(mc.flags & DL_FLAG_BLOCKING) && mc.alarm != ALARM_HOMING_REQUIRED && link_cmd("$X"))
            unlock_pending = false;
    }
}

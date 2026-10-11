// Serial link to the grblHAL controller, protocol see display_link.c

#include "link.h"
#include "pins.h"

#define LINK_TIMEOUT_MS     500     // no status line for this long = offline
#define CMD_RETRY_MS        100     // busy: send the command line again after this time
#define CMD_ACK_MS          400     // no answer: send again
#define CMD_TRIES            30     // then give up
#define CMD_QUEUE             8
#define HEARTBEAT_MS        500     // the controller sees the display as connected

machine_t mc = {};
link_stats_t link_stats = {};
file_list_t files = {};
preview_t preview = {};
settings_list_t setlist = {};

static HardwareSerial &port = Serial1;

static struct {
    char line[96];
    uint16_t id;
} queue[CMD_QUEUE];
static uint8_t q_head = 0, q_count = 0, q_tries = 0;
static uint16_t next_id = 1;
static uint32_t q_sent = 0, q_wait = 0;
static bool q_await = false;

void link_send (const char *body)
{
    uint8_t sum = 0;
    char line[128];

    for(const char *p = body; *p; p++)
        sum ^= (uint8_t)*p;

    snprintf(line, sizeof(line), ">%s*%02X\n", body, sum);
    port.write(line);
}

void link_rt (uint8_t cmd)
{
    char body[8];

    snprintf(body, sizeof(body), "RT,%02X", cmd);
    link_send(body);
}

bool link_cmd (const char *line)
{
    if(q_count == CMD_QUEUE)
        return false;

    uint8_t i = (q_head + q_count) % CMD_QUEUE;
    strlcpy(queue[i].line, line, sizeof(queue[i].line));
    queue[i].id = next_id++;
    q_count++;

    return true;
}

bool link_idle (void)
{
    return q_count == 0;
}

static void queue_pop (void)
{
    q_head = (q_head + 1) % CMD_QUEUE;
    q_count--;
    q_tries = 0;
    q_await = false;
}

static void queue_poll (uint32_t now)
{
    if(!q_count || !mc.online)
        return;

    if(q_await && now - q_sent < CMD_ACK_MS)
        return;

    if(!q_await && now < q_wait)
        return;

    if(++q_tries > CMD_TRIES) {
        Serial.printf("link: gave up on '%s'\n", queue[q_head].line);
        strlcpy(mc.refused, queue[q_head].line, sizeof(mc.refused));
        mc.refused_seq++;
        queue_pop();
        return;
    }

    char body[112];
    snprintf(body, sizeof(body), "GC,%u,%s", queue[q_head].id, queue[q_head].line);
    link_send(body);
    q_sent = now;
    q_await = true;
}

static void handle_ack (char *args)
{
    unsigned id, ok;

    if(sscanf(args, "%u,%u", &id, &ok) != 2 || !q_count || id != queue[q_head].id)
        return;

    if(ok)
        queue_pop();
    else {
        q_await = false;
        q_wait = millis() + CMD_RETRY_MS;
    }
}

static void handle_status (const char *args)
{
    unsigned seq, state, alarm, flags, ovf, ovr, ovs, mode, wcs;
    int pct;
    float w[3], m[3], feed, s;

    if(sscanf(args, "%u,%x,%u,%x,%f,%f,%f,%f,%f,%f,%f,%f,%u,%u,%u,%d,%u,%u", &seq, &state, &alarm, &flags,
               &w[0], &w[1], &w[2], &m[0], &m[1], &m[2], &feed, &s, &ovf, &ovr, &ovs, &pct, &mode, &wcs) != 18)
        return;

    mc.state = state;
    mc.alarm = alarm;
    mc.flags = flags;
    memcpy(mc.wpos, w, sizeof(w));
    memcpy(mc.mpos, m, sizeof(m));
    mc.feed = feed;
    mc.s = s;
    mc.ov_feed = ovf;
    mc.ov_rapid = ovr;
    mc.ov_spindle = ovs;
    mc.pct = pct;
    mc.mode = mode;
    mc.wcs = wcs;
    mc.updated = millis();
    mc.online = true;
}

// <id>|<group>|<editable>|<datatype>|<value>|<unit>|<min>|<max>|<name>|<format>
static void parse_setting (char *args)
{
    char *f[10];
    int n = 0;

    if(setlist.item == NULL)
        return;

    f[n++] = args;
    for(char *p = args; *p && n < 10; p++) {
        if(*p == '|') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    if(n < 10)
        return;

    uint16_t id = (uint16_t)atoi(f[0]);
    int i = 0;
    while(i < setlist.count && setlist.item[i].id != id)
        i++;
    if(i == setlist.count) {
        if(setlist.count == SETTINGS_MAX)
            return;
        setlist.count++;
    }

    setting_t *s = &setlist.item[i];
    s->id = id;
    strlcpy(s->group, f[1], sizeof(s->group));
    s->editable = f[2][0] == '1';
    s->type = (uint8_t)atoi(f[3]);
    strlcpy(s->value, f[4], sizeof(s->value));
    strlcpy(s->unit, f[5], sizeof(s->unit));
    strlcpy(s->min, f[6], sizeof(s->min));
    strlcpy(s->max, f[7], sizeof(s->max));
    strlcpy(s->name, f[8], sizeof(s->name));
    strlcpy(s->format, f[9], sizeof(s->format));

    if(!setlist.loading)
        setlist.seq++;      // a single update after a change
}

void link_settings (void)
{
    setlist.count = 0;
    setlist.loading = true;
    link_send("SL");
}

void link_setting (uint16_t id)
{
    char body[16];

    snprintf(body, sizeof(body), "SR,%u", id);
    link_send(body);
}

static void handle_line (char *line)
{
    char *star = strrchr(line, '*');
    uint8_t sum = 0;

    strlcpy(link_stats.last, line, sizeof(link_stats.last));

    if(star == NULL) {
        link_stats.lines_bad++;
        return;
    }

    *star = '\0';
    for(char *p = line; *p; p++)
        sum ^= (uint8_t)*p;

    if(strtoul(star + 1, NULL, 16) != sum) {
        link_stats.lines_bad++;
        return;
    }
    link_stats.lines_ok++;

    if(!strncmp(line, "DS,", 3))
        handle_status(line + 3);
    else if(!strncmp(line, "AK,", 3))
        handle_ack(line + 3);
    else if(!strncmp(line, "DI,", 3)) {
        float t[3], mn[3], mx[2], smax;
        if(sscanf(line + 3, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &t[0], &t[1], &t[2], &mn[0], &mn[1], &mx[0], &mx[1], &mn[2], &smax) == 9) {
            memcpy(mc.travel, t, sizeof(t));
            mc.env_min[0] = mn[0]; mc.env_min[1] = mn[1]; mc.env_min[2] = mn[2];
            mc.env_max[0] = mx[0]; mc.env_max[1] = mx[1];
            mc.env_valid = mx[0] - mn[0] > 1.0f && mx[1] - mn[1] > 1.0f;
            mc.s_max = smax;
            mc.info = true;
        }
    } else if(!strncmp(line, "DN,", 3))
        strlcpy(mc.job, line + 3, sizeof(mc.job));
    else if(!strncmp(line, "FL,", 3)) {
        if(files.done) {
            files.done = false;
            files.count = 0;
        }
        if(files.count >= 0 && files.count < LINK_FILES_MAX)
            strlcpy(files.name[files.count++], line + 3, sizeof(files.name[0]));
    } else if(!strncmp(line, "FE,", 3)) {
        if(atoi(line + 3) < 0)
            files.count = -1;
        else if(files.done)
            files.count = 0;
        files.done = true;
        files.seq++;
    } else if(!strncmp(line, "PB,", 3)) {
        unsigned w, h;
        if(sscanf(line + 3, "%f,%f,%f,%f,%u,%u", &preview.x0, &preview.y0, &preview.x1, &preview.y1, &w, &h) == 6 &&
            w <= PREVIEW_W_MAX && h <= PREVIEW_H_MAX && w % 8 == 0) {
            preview.w = w;
            preview.h = h;
            memset(preview.bits, 0, sizeof(preview.bits));
        } else
            preview.loading = false;
    } else if(!strncmp(line, "PR,", 3)) {
        char *p;
        unsigned row = strtoul(line + 3, &p, 10);
        if(preview.loading && *p == ',' && row < preview.h) {
            uint8_t *bits = &preview.bits[row * (preview.w / 8)];
            for(unsigned k = 0; k < preview.w / 8 && p[1 + 2 * k] && p[2 + 2 * k]; k++) {
                char hex[3] = { p[1 + 2 * k], p[2 + 2 * k], 0 };
                bits[k] = (uint8_t)strtoul(hex, NULL, 16);
            }
        }
    } else if(!strncmp(line, "PE,", 3)) {
        if(preview.loading) {
            preview.valid = atoi(line + 3) > 0 && preview.w;
            preview.loading = false;
            preview.seq++;
        }
    } else if(!strncmp(line, "ER,", 3)) {
        unsigned code;
        int job;
        if(sscanf(line + 3, "%u,%d", &code, &job) == 2) {
            mc.err_code = code;
            mc.err_job = job == 1;
            mc.err_seq++;
        }
    } else if(!strncmp(line, "SV|", 3))
        parse_setting(line + 3);
    else if(!strncmp(line, "SE,", 3)) {
        setlist.loading = false;
        setlist.seq++;
    } else if(!strncmp(line, "MS,", 3)) {
        strlcpy(mc.msg, line + 3, sizeof(mc.msg));
        mc.msg_seq++;
    }
}

void link_preview (const char *name)
{
    char body[80];

    preview.valid = false;
    preview.w = preview.h = 0;
    preview.seq++;
    strlcpy(preview.name, name, sizeof(preview.name));
    if((preview.loading = *name)) {
        snprintf(body, sizeof(body), "PV,%s", name);
        link_send(body);
    }
}

void link_begin (void)
{
    setlist.item = (setting_t *)ps_calloc(SETTINGS_MAX, sizeof(setting_t));
    port.setRxBufferSize(8192);     // preview rows arrive in bursts while LVGL is drawing
    port.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);
}

void link_poll (void)
{
    static char line[220];
    static uint16_t len = 0;

    while(port.available()) {
        int c = port.read();
        link_stats.rx_bytes++;
        if(c == '$')
            len = 0;
        else if(c == '\n' || c == '\r') {
            line[len] = '\0';
            if(len > 4)
                handle_line(line);
            len = 0;
        } else if(len < sizeof(line) - 1)
            line[len++] = (char)c;
        else
            len = 0;
    }

    uint32_t now = millis();    // after reading, a status line just received is not older than now

    if(mc.online && now - mc.updated > LINK_TIMEOUT_MS)
        mc.online = false;

    static uint32_t last_hb = 0;
    if(now - last_hb >= HEARTBEAT_MS) {
        last_hb = now;
        link_send("HB");
    }

    queue_poll(now);
}

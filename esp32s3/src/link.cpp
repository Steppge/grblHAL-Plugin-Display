// Serial link to the grblHAL controller, protocol see display_link.c

#include "link.h"
#include "pins.h"

#define LINK_TIMEOUT_MS     500     // no status line for this long = offline
#define CMD_RETRY_MS        100     // busy: send the command line again after this time
#define CMD_ACK_MS          400     // no answer: send again
#define CMD_TRIES            30     // then give up
#define CMD_QUEUE             8

machine_t mc = {};
link_stats_t link_stats = {};
file_list_t files = {};
preview_t preview = {};

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

    queue_poll(now);
}

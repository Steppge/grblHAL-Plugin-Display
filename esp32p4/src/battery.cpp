// Battery voltage, see battery.h

#include <Arduino.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "pins.h"
#include "battery.h"

#define BATT_MS         2000    // measuring interval
#define BATT_SAMPLES    16      // readings averaged per measurement
#define BATT_DIV        2.0f    // 1 MOhm / 1 MOhm
#define BATT_MIN_V      1.0f    // below: no battery connected

static Preferences prefs;
static float cal = 1.0f;        // correction factor from "batt <volts>"
static float volts = 0.0f;      // filtered battery voltage
static bool valid = false;
static bool logging = false;
static uint32_t log_last = 0;

#define LOG_FILE        "/battlog.csv"
#define LOG_MS          60000UL

static void hist_poll (void);

static void log_line (const char *line)
{
    File f = LittleFS.open(LOG_FILE, FILE_APPEND);
    if(f) {
        f.println(line);
        f.close();
    }
}

static float measure (void)
{
    uint32_t mv = 0;
    for(int i = 0; i < BATT_SAMPLES; i++)
        mv += analogReadMilliVolts(PIN_BATT);
    return mv / (float)BATT_SAMPLES / 1000.0f * BATT_DIV * cal;
}

void battery_begin (void)
{
    prefs.begin("battery", false);
    cal = prefs.getFloat("cal", 1.0f);
    if(cal < 0.5f || cal > 1.5f)
        cal = 1.0f;
    // the default attenuation 11 dB measures up to about 3.1 V at the pin, 4.2 V gives 2.1 V
    // (analogSetPinAttenuation() before the first reading only gives "Pin is not configured as analog channel")
    volts = measure();
    valid = true;
    logging = prefs.getBool("log", false);
    log_last = 0;
}

void battery_poll (void)
{
    static uint32_t last = 0;
    if(millis() - last < BATT_MS)
        return;
    last = millis();

    float v = measure();
    // slow filter against load steps of the backlight and WiFi, a jump (battery plugged in or off) is taken at once
    if(!valid || fabsf(v - volts) > 0.5f)
        volts = v;
    else
        volts += (v - volts) * 0.2f;
    valid = true;
    hist_poll();

    // the first line after a start or restart at once (the file system is mounted by then), then every minute
    if(logging && (log_last == 0 || millis() - log_last >= LOG_MS)) {
        static bool restarted = true;
        if(restarted) {
            restarted = false;
            if(!LittleFS.begin(false))
                return;
            log_line("# start");
        }
        log_last = millis() | 1;
        char buf[64];
        snprintf(buf, sizeof(buf), "%lu,%.3f,%d", millis() / 1000UL, volts, battery_percent());
        log_line(buf);
    }
}

bool battery_logging (void)
{
    return logging;
}

void battlog_console (const char *arg)
{
    if(!strcmp(arg, "on")) {
        if(!LittleFS.begin(false)) {
            Serial.println("battlog: no file system");
            return;
        }
        LittleFS.remove(LOG_FILE);
        log_line("# seconds since start,volts,percent");
        logging = true;
        log_last = 0;
        prefs.putBool("log", true);
    } else if(!strcmp(arg, "off")) {
        logging = false;
        prefs.putBool("log", false);
    } else if(!strcmp(arg, "dump")) {
        File f = LittleFS.open(LOG_FILE, FILE_READ);
        if(!f) {
            Serial.println("battlog: no log");
            return;
        }
        while(f.available())
            Serial.write(f.read());
        f.close();
        Serial.println("# end");
        return;
    }
    File f = LittleFS.open(LOG_FILE, FILE_READ);
    Serial.printf("battlog %s, %u bytes\n", logging ? "on" : "off", f ? (unsigned)f.size() : 0);
    if(f)
        f.close();
}

bool battery_present (void)
{
    return valid && volts >= BATT_MIN_V;
}

float battery_volts (void)
{
    return volts;
}

// Remaining run time in percent, from the discharge log of 2026-11-10 (display on, not dimmed, 573 min
// from 4.14 V off the charger to 3.0 V): the share of the time left at each voltage
static float level (void)
{
    static const float v_tab[] = { 3.10f, 3.20f, 3.30f, 3.40f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.14f };
    static const uint8_t p_tab[] = { 0, 1, 2, 6, 18, 34, 47, 60, 72, 85, 97, 100 };
    const int n = sizeof(p_tab);

    if(volts <= v_tab[0])
        return 0.0f;
    for(int i = 1; i < n; i++) {
        if(volts < v_tab[i])
            return p_tab[i - 1] + (volts - v_tab[i - 1]) / (v_tab[i] - v_tab[i - 1]) * (p_tab[i] - p_tab[i - 1]);
    }
    return 100.0f;
}

int battery_percent (void)
{
    return (int)(level() + 0.5f);
}

// Remaining minutes: from the drop of the last RATE_MIN minutes, before that from the logged run time.
// -1 while the level rises (charging).
#define RUN_MIN         573     // logged run time, display on
#define RATE_MIN        30      // minutes of history
#define RATE_MIN_USED   20      // at least this many for the measured drop

static float hist[RATE_MIN + 1];
static int hist_n = 0;
static uint32_t hist_last = 0;
static bool charging = false;
static int rises = 0;

// once a minute. Charging: a jump of more than 5 % or a rise in 3 minutes in a row (a load step, such as
// dimming, rises once only); it ends with a drop (charger off), then the history starts again.
static void hist_poll (void)
{
    if(hist_n && millis() - hist_last < 60000UL)
        return;
    hist_last = millis();
    float l = level();
    if(hist_n) {
        float prev = hist[hist_n - 1];
        rises = l > prev + 0.2f ? rises + 1 : 0;
        // above 4.16 V only on the charger (a full cell off it reads 4.13..4.15 V)
        if(!charging && (l > prev + 5.0f || rises >= 3 || volts > 4.16f))
            charging = true;
        else if(charging && l < prev - 0.5f) {
            charging = false;
            rises = 0;
            hist_n = 0;
        }
    }
    if(charging)
        hist_n = 0;             // only the last value, for the drop at the end
    if(hist_n == RATE_MIN + 1) {
        memmove(hist, hist + 1, RATE_MIN * sizeof(float));
        hist_n--;
    }
    hist[hist_n++] = l;
}

bool battery_charging (void)
{
    return charging;
}

int battery_minutes (void)
{
    if(charging)
        return -1;
    float l = level();
    int mins = (int)(l * RUN_MIN / 100.0f);
    if(hist_n > RATE_MIN_USED) {
        float drop = hist[0] - l;
        if(drop > 0.5f)
            mins = (int)(l * (hist_n - 1) / drop);
        else
            mins = 5999;
    }
    return min(mins, 5999);
}

void battery_console (const char *arg)
{
    float want = atof(arg);
    if(*arg && want == 0.0f) {
        cal = 1.0f;
        prefs.putFloat("cal", cal);
        Serial.println("battery calibration reset");
    } else if(want > 0.0f) {
        float raw = measure() / cal;
        if(raw > BATT_MIN_V && want / raw > 0.5f && want / raw < 1.5f) {
            cal = want / raw;
            prefs.putFloat("cal", cal);
            volts = measure();
            Serial.printf("battery calibrated, factor %.4f\n", cal);
        } else
            Serial.println("battery: calibration not possible (no battery or too far off)");
    }
    Serial.printf("battery %.3f V, %d %%, %d min%s, factor %.4f, pin %u mV\n", volts, battery_percent(), battery_minutes(), charging ? " (charging)" : "", cal,
                  (unsigned)analogReadMilliVolts(PIN_BATT));
}

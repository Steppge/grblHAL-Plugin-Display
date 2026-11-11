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

// Remaining run time in percent, from the discharge log of 2026-11-11 (CNC mode the whole time, display on,
// not dimmed, 488 min from 4.23 V off the charger to 3.10 V): the share of the time left at each voltage.
// From BATT_FULL_V on 100 %; the second run with mixed use gave the same shares.
static float level (void)
{
    static const float v_tab[] = { 3.10f, 3.20f, 3.30f, 3.40f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, BATT_FULL_V };
    static const uint8_t p_tab[] = { 0, 1, 2, 6, 19, 35, 46, 58, 71, 86, 97, 100 };
    const int n = sizeof(p_tab);

    if(volts <= v_tab[0])
        return 0.0f;
    for(int i = 1; i < n; i++) {
        if(volts < v_tab[i])
            return p_tab[i - 1] + (volts - v_tab[i - 1]) / (v_tab[i] - v_tab[i - 1]) * (p_tab[i] - p_tab[i - 1]);
    }
    return 100.0f;
}

static bool charging = false;

// On the charger the voltage reads higher than without load: 100 % only from BATT_CHARGED_V on, 99 % at most below
int battery_percent (void)
{
    int p = (int)(level() + 0.5f);
    if(charging && p > 99 && volts < BATT_CHARGED_V)
        p = 99;
    return p;
}

// Remaining minutes: from the drop of the last RATE_MIN minutes, else (too short, or on the flat part at the
// top) from the logged run time. -1 while charging.
#define RUN_MIN         488     // logged run time in CNC mode, display on
#define RATE_MIN        30      // minutes of history
#define RATE_MIN_USED   20      // at least this many for the measured drop

static float hist[RATE_MIN + 1];    // level once a minute
static int hist_n = 0;
static uint32_t hist_last = 0;
static float v_prev = 0.0f;         // voltage a minute ago
static int rises = 0;

// once a minute. Charging: above BATT_CHARGE_V (only on the charger, a full cell off it drops below at once)
// or a rise of the voltage in 3 minutes in a row (a load step, such as dimming, rises once only); it ends with
// a drop (charger off), then the history starts again.
static void hist_poll (void)
{
    if(hist_n && millis() - hist_last < 60000UL)
        return;
    hist_last = millis();
    if(hist_n) {
        rises = volts > v_prev + 0.003f ? rises + 1 : 0;
        if(!charging && (rises >= 3 || volts > BATT_CHARGE_V))
            charging = true;
        else if(charging && volts < v_prev - 0.02f && volts <= BATT_CHARGE_V) {
            charging = false;
            rises = 0;
            hist_n = 0;
        }
    }
    v_prev = volts;
    if(charging)
        hist_n = 0;             // only the last value
    if(hist_n == RATE_MIN + 1) {
        memmove(hist, hist + 1, RATE_MIN * sizeof(float));
        hist_n--;
    }
    hist[hist_n++] = level();
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
    float mins = l * RUN_MIN / 100.0f;
    // the measured drop; not on the flat part at the top, and at most half again the logged time
    // (the PC-Deck mode needs less)
    if(hist_n > RATE_MIN_USED && l < 99.0f) {
        float drop = hist[0] - l;
        if(drop > 0.5f)
            mins = min(l * (hist_n - 1) / drop, mins * 1.5f);
    }
    return (int)mins;
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

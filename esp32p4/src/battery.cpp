// Battery voltage, see battery.h

#include <Arduino.h>
#include <Preferences.h>
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
}

bool battery_present (void)
{
    return valid && volts >= BATT_MIN_V;
}

float battery_volts (void)
{
    return volts;
}

// Li-ion discharge curve under light load; a full cell off the charger reads about 4.17 V with the display on
int battery_percent (void)
{
    static const float v_tab[] = { 3.30f, 3.40f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.08f, 4.15f };
    static const uint8_t p_tab[] = { 0, 5, 10, 20, 35, 50, 65, 80, 92, 100 };
    const int n = sizeof(p_tab);

    if(volts <= v_tab[0])
        return 0;
    for(int i = 1; i < n; i++) {
        if(volts < v_tab[i])
            return p_tab[i - 1] + (int)((volts - v_tab[i - 1]) / (v_tab[i] - v_tab[i - 1]) * (p_tab[i] - p_tab[i - 1]) + 0.5f);
    }
    return 100;
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
    Serial.printf("battery %.3f V, %d %%, factor %.4f, pin %u mV\n", volts, battery_percent(), cal,
                  (unsigned)analogReadMilliVolts(PIN_BATT));
}

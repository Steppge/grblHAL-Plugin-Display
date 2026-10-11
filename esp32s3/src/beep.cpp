// Beeper on the TS35 (EXP1-1, transistor Q2 drives the buzzer), tones from a LEDC PWM channel

#include "beep.h"
#include "pins.h"

#define BEEP_HZ     4000
#define CLICK_MS      15

static uint8_t left = 0;            // tones still to play
static bool on = false;
static uint16_t tone_ms, gap_ms;
static uint32_t t_next = 0;

static void out (bool state)
{
    ledcWrite(PIN_BEEPER, state ? 128 : 0);
    on = state;
}

void beep_begin (void)
{
    ledcAttach(PIN_BEEPER, BEEP_HZ, 8);
    out(false);
}

void beep (uint8_t count, uint16_t ms, uint16_t gap)
{
    left = count;
    tone_ms = ms;
    gap_ms = gap;
    out(false);
    t_next = millis();
}

void beep_click (void)
{
    if(left == 0 && !on)
        beep(1, CLICK_MS, 0);
}

void beep_poll (void)
{
    if((left == 0 && !on) || (int32_t)(millis() - t_next) < 0)
        return;

    if(on) {
        out(false);
        t_next = millis() + gap_ms;
    } else if(left) {
        out(true);
        left--;
        t_next = millis() + tone_ms;
    }
}

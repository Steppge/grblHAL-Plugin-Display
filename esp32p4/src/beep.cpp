// Click and beeps on the speaker (I2S amplifier), same interface as the TS35 beeper of the ESP32-S3.
// A small task plays the requests. The amplifier stays on: it needs some 10 ms after switching on,
// a click would be over before it can be heard. Between the tones the I2S output sends silence.

#include <Arduino.h>
#include <math.h>
#include <driver/i2s_std.h>
#include "beep.h"
#include "pins.h"

#define RATE        16000
#define TONE_HZ      2700
#define AMPLITUDE    4000       // of 32767
#define CLICK_MS       40
#define FADE_MS         8       // soft start and end of each tone

struct beep_req_t {
    uint8_t count;
    uint16_t ms, gap_ms;
};

static i2s_chan_handle_t tx = NULL;
static QueueHandle_t queue = NULL;
static float phase = 0.0f;

static void play (uint16_t ms, bool tone)
{
    int16_t buf[256];
    size_t total = (size_t)RATE * ms / 1000, samples = total, fade = (size_t)RATE * FADE_MS / 1000, written;

    if(fade > total / 2)
        fade = total / 2;

    while(samples) {
        size_t n = min(samples, sizeof(buf) / sizeof(buf[0]));
        for(size_t i = 0; i < n; i++) {
            size_t k = total - samples + i;     // sample index in the tone
            float env = 1.0f;
            if(fade && k < fade)
                env = (float)k / fade;
            else if(fade && total - k <= fade)
                env = (float)(total - k) / fade;
            buf[i] = tone ? (int16_t)(AMPLITUDE * env * sinf(phase)) : 0;
            phase += 2.0f * (float)M_PI * TONE_HZ / RATE;
            if(phase > 2.0f * (float)M_PI)
                phase -= 2.0f * (float)M_PI;
        }
        i2s_channel_write(tx, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
        samples -= n;
    }
}

static void beep_task (void *arg)
{
    beep_req_t r;

    for(;;) {
        if(xQueueReceive(queue, &r, portMAX_DELAY) != pdTRUE)
            continue;

        for(uint8_t i = 0; i < r.count; i++) {
            play(r.ms, true);
            if(i + 1 < r.count)
                play(r.gap_ms, false);
        }
    }
}

void beep_begin (void)
{
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, HIGH);     // off until the I2S output runs

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;         // silence when nothing is written
    if(i2s_new_channel(&chan_cfg, &tx, NULL) != ESP_OK) {
        Serial.println("beep: no I2S channel");
        tx = NULL;
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_I2S_BCLK,
            .ws = (gpio_num_t)PIN_I2S_LRCLK,
            .dout = (gpio_num_t)PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    if(i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
        Serial.println("beep: I2S setup failed");
        tx = NULL;
        return;
    }

    digitalWrite(PIN_AMP_EN, LOW);      // amplifier on

    queue = xQueueCreate(4, sizeof(beep_req_t));
    xTaskCreate(beep_task, "beep", 3072, NULL, 3, NULL);
}

void beep (uint8_t count, uint16_t ms, uint16_t gap_ms)
{
    beep_req_t r = { count, ms, gap_ms };

    if(queue)
        xQueueSend(queue, &r, 0);
}

void beep_click (void)
{
    if(queue && uxQueueMessagesWaiting(queue) == 0)
        beep(1, CLICK_MS, 0);
}

void beep_poll (void)
{
    // the task plays by itself
}

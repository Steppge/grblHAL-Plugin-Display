// grblHAL touch display: ESP32-S3 + MKS TS35 (ST7796 480x320, XPT2046), LovyanGFX + LVGL.
// Talks to the controller over a serial link, see link.cpp and display_link.c.
// Touch calibration (4 corners) runs on first start and is stored in NVS.
// Hold BOOT (GPIO0) during reset to calibrate again.

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include "lgfx_ts35.h"
#include "link.h"
#include "ui.h"
#include "beep.h"

#define CAL_MAGIC   0x54533335  // "TS35"
#define UI_MS       100         // ui_update() interval
#define BUF_LINES   40          // LVGL draw buffer height

static LGFX_TS35 lcd;
static Preferences prefs;

// Power and gamma settings of the old Octopus display driver (Marlin st7796s.h) instead of the
// LovyanGFX defaults, 0 = LovyanGFX
#define PANEL_GAMMA_MARLIN 0

static void panel_gamma (void)
{
#if PANEL_GAMMA_MARLIN
    static const uint8_t seq[] = {
        0xF0, 1, 0xC3, 0xF0, 1, 0x96,                       // enable command 2
        0xC1, 1, 0x15, 0xC2, 1, 0xAF, 0xC5, 1, 0x22, 0xC6, 1, 0x00,
        0xE0, 14, 0xF0, 0x04, 0x08, 0x09, 0x08, 0x15, 0x2F, 0x42, 0x46, 0x28, 0x15, 0x16, 0x29, 0x2D,
        0xE1, 14, 0xF0, 0x04, 0x09, 0x09, 0x08, 0x15, 0x2E, 0x46, 0x46, 0x28, 0x15, 0x15, 0x29, 0x2D,
        0xF0, 1, 0x3C, 0xF0, 1, 0x69                        // disable command 2
    };

    lcd.startWrite();
    for(size_t i = 0; i < sizeof(seq); ) {
        lcd.writeCommand(seq[i]);
        for(int j = 0; j < seq[i + 1]; j++)
            lcd.writeData(seq[i + 2 + j]);
        i += 2 + seq[i + 1];
    }
    lcd.endWrite();
#endif
}

static void calibrate (void)
{
    uint16_t cal[8];

    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE);
    lcd.setTextDatum(middle_center);
    lcd.drawString("Touch the arrow tips", lcd.width() / 2, lcd.height() / 2, &fonts::FreeSansBold12pt7b);
    lcd.calibrateTouch(cal, TFT_MAGENTA, TFT_BLACK, 20);
    prefs.putBytes("cal", cal, sizeof(cal));
    prefs.putUInt("magic", CAL_MAGIC);
    Serial.printf("touch cal: %u %u %u %u %u %u %u %u\n", cal[0], cal[1], cal[2], cal[3], cal[4], cal[5], cal[6], cal[7]);
}

static void flush (lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    // LVGL renders RGB565 little endian. The strip goes out by DMA while LVGL draws the next one
    // into the other buffer, pushImageDMA() waits for the previous transfer before it starts.
    if(lcd.getStartCount() == 0)
        lcd.startWrite();
    lcd.pushImageDMA(area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area), (const lgfx::rgb565_t *)px);
    lv_display_flush_ready(disp);
}

static void touch_read (lv_indev_t *indev, lv_indev_data_t *data)
{
    int32_t x, y;

    if(lcd.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else
        data->state = LV_INDEV_STATE_RELEASED;
}

static uint32_t tick (void)
{
    return millis();
}

void setup (void)
{
    Serial.begin(115200);
    pinMode(0, INPUT_PULLUP);

    lcd.init();
    lcd.setRotation(3);     // same orientation as on the Octopus
    panel_gamma();
    lcd.setBrightness(255);
    lcd.fillScreen(TFT_BLACK);

    Serial.printf("grblHAL display, %d x %d, PSRAM %u bytes\n", lcd.width(), lcd.height(), ESP.getPsramSize());

    prefs.begin("display", false);
    uint16_t cal[8];
    if(digitalRead(0) == LOW || prefs.getUInt("magic", 0) != CAL_MAGIC || prefs.getBytes("cal", cal, sizeof(cal)) != sizeof(cal))
        calibrate();
    else
        lcd.setTouchCalibrate(cal);

    lv_init();
    lv_tick_set_cb(tick);

    size_t buf_size = lcd.width() * BUF_LINES * 2;
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    lv_display_t *disp = lv_display_create(lcd.width(), lcd.height());
    lv_display_set_flush_cb(disp, flush);
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);

    beep_begin();
    link_begin();
    ui_init();
}

void loop (void)
{
    static uint32_t last_ui = 0;

    link_poll();
    beep_poll();

    if(millis() - last_ui >= UI_MS) {
        last_ui = millis();
        ui_update();
    }

    lv_timer_handler();

    // release the bus after a refresh, the touch controller shares it
    if(lcd.getStartCount()) {
        lcd.waitDMA();
        lcd.endWrite();
    }

    delay(2);
}

// GT911 capacitive touch controller on I2C (address 0x5D), coordinates match the panel directly.
// Arduino SD_MMC is not used anywhere: it drives GPIO45, the touch SDA.

#include <Arduino.h>
#include <Wire.h>
#include "touch.h"
#include "pins.h"

#define GT911_ADDR      0x5D
#define REG_PRODUCT_ID  0x8140
#define REG_STATUS      0x814E      // bit 7: new data, bits 0-3: number of points
#define REG_POINT1      0x8150      // x low, x high, y low, y high, ...

static bool pressed = false;
static int16_t last_x = 0, last_y = 0;

static bool reg_read (uint16_t reg, uint8_t *buf, uint8_t len)
{
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    if(Wire.endTransmission(false) != 0)
        return false;

    if(Wire.requestFrom((uint8_t)GT911_ADDR, len) != len)
        return false;
    for(uint8_t i = 0; i < len; i++)
        buf[i] = Wire.read();

    return true;
}

static void reg_write (uint16_t reg, uint8_t value)
{
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    Wire.write(value);
    Wire.endTransmission();
}

bool touch_begin (void)
{
    // reset with INT low selects address 0x5D
    pinMode(PIN_TOUCH_INT, OUTPUT);
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_INT, LOW);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(10);
    pinMode(PIN_TOUCH_INT, INPUT);
    delay(50);

    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000);

    char id[5] = {};
    if(!reg_read(REG_PRODUCT_ID, (uint8_t *)id, 4)) {
        Serial.println("touch: GT911 does not answer");
        return false;
    }
    Serial.printf("touch: GT%s\n", id);

    return true;
}

static void touch_read (lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t status;

    // without new data the last state stays
    if(reg_read(REG_STATUS, &status, 1) && (status & 0x80)) {
        uint8_t pt[4];
        pressed = (status & 0x0F) > 0 && reg_read(REG_POINT1, pt, 4);
        if(pressed) {
            last_x = pt[0] | (pt[1] << 8);
            last_y = pt[2] | (pt[3] << 8);
        }
        reg_write(REG_STATUS, 0);
    }

    data->point.x = last_x;
    data->point.y = last_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void touch_indev (lv_display_t *disp)
{
    lv_indev_t *indev = lv_indev_create();

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_display(indev, disp);
}

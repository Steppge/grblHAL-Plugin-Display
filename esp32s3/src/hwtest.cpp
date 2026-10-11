// Display and touch test with LovyanGFX. After the panel init the ST7796 data output is switched
// off (B0h = 80h, SDA_EN) so that only the XPT2046 drives MISO. Prints raw touch values.

#include <Arduino.h>
#include "lgfx_ts35.h"

static LGFX_TS35 lcd;

static void tft_reg (uint8_t cmd, uint8_t val)
{
    lcd.startWrite();
    lcd.writeCommand(0xF0); lcd.writeData(0xC3);    // enable command 2
    lcd.writeCommand(0xF0); lcd.writeData(0x96);
    lcd.writeCommand(cmd);  lcd.writeData(val);
    lcd.writeCommand(0xF0); lcd.writeData(0x3C);
    lcd.writeCommand(0xF0); lcd.writeData(0x69);
    lcd.endWrite();
}

void setup (void)
{
    Serial.begin(115200);
    lcd.init();
    lcd.setRotation(3);
    lcd.setBrightness(255);
    tft_reg(0xB0, 0x80);

    static const uint32_t bars[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };
    for(int i = 0; i < 4; i++)
        lcd.fillRect(i * 120, 0, 120, 160, lcd.color888(bars[i] >> 16, (bars[i] >> 8) & 0xFF, bars[i] & 0xFF));
    lcd.fillRect(0, 160, 480, 160, TFT_NAVY);
    lcd.setTextColor(TFT_WHITE);
    lcd.setTextDatum(middle_center);
    lcd.drawString("RED GREEN BLUE WHITE", 240, 200, &fonts::FreeSansBold12pt7b);
    lcd.drawString("touch test", 240, 260, &fonts::FreeSansBold18pt7b);
}

void loop (void)
{
    lgfx::touch_point_t tp;
    int n = lcd.getTouchRaw(&tp, 1);

    Serial.printf("touch raw: n=%d x=%d y=%d size=%d  int=%d\n", n, tp.x, tp.y, tp.size, digitalRead(PIN_TOUCH_INT));
    delay(300);
}

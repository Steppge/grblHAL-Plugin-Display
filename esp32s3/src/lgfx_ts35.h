#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "pins.h"

#ifndef TFT_SPI_HZ
#define TFT_SPI_HZ  80000000  // max. of the S3 SPI on its native pins, 40000000 if the picture shows errors
#endif

// ST7796 480x320 and XPT2046 touch share one SPI bus
class LGFX_TS35 : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7796 panel;
    lgfx::Bus_SPI bus;
    lgfx::Light_PWM light;
    lgfx::Touch_XPT2046 touch;

public:
    LGFX_TS35()
    {
        {
            auto cfg = bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = TFT_SPI_HZ;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = PIN_SPI_SCK;
            cfg.pin_mosi = PIN_SPI_MOSI;
            cfg.pin_miso = PIN_SPI_MISO;
            cfg.pin_dc = PIN_TFT_DC;
            bus.config(cfg);
            panel.setBus(&bus);
        }
        {
            auto cfg = panel.config();
            cfg.pin_cs = PIN_TFT_CS;
            cfg.pin_rst = PIN_TFT_RST;
            cfg.pin_busy = -1;
            cfg.panel_width = 320;
            cfg.panel_height = 480;
            cfg.readable = true;
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.bus_shared = true;
            panel.config(cfg);
        }
        {
            auto cfg = light.config();
            cfg.pin_bl = PIN_TFT_BL;
            cfg.invert = false;
            cfg.freq = 12000;
            cfg.pwm_channel = 7;
            light.config(cfg);
            panel.setLight(&light);
        }
        {
            auto cfg = touch.config();
            cfg.spi_host = SPI2_HOST;
            cfg.freq = 2000000;     // XPT2046 is specified up to 2.5 MHz
            cfg.pin_sclk = PIN_SPI_SCK;
            cfg.pin_mosi = PIN_SPI_MOSI;
            cfg.pin_miso = PIN_SPI_MISO;
            cfg.pin_cs = PIN_TOUCH_CS;
            cfg.pin_int = PIN_TOUCH_INT;
            cfg.bus_shared = true;
            cfg.x_min = 0;
            cfg.x_max = 4095;
            cfg.y_min = 0;
            cfg.y_max = 4095;
            touch.config(cfg);
            panel.setTouch(&touch);
        }
        setPanel(&panel);
    }
};

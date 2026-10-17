// Panel of the CrowPanel Advanced 9": EK79007, 1024x600, MIPI-DSI with 2 lanes at 1000 Mbps, RGB565.
// LVGL renders into two strip buffers in PSRAM, DMA2D copies them into the panel's frame buffer.

#include <Arduino.h>
#include <esp_ldo_regulator.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "display.h"
#include "pins.h"

#define LANE_MBPS       1000
#define DPI_MHZ           51
#define BUF_LINES        120    // LVGL strip height

static esp_lcd_panel_handle_t panel = NULL;

// EK79007 initialization, from Espressif's esp_lcd_ek79007 (Apache-2.0): 2 data lanes, vendor registers, sleep out
static const struct {
    uint8_t cmd, data, len;
    uint16_t delay_ms;
} ek79007_init[] = {
    { 0xB2, 0x10, 1, 0 },       // pad control: 2 DSI lanes
    { 0x80, 0x8B, 1, 0 },
    { 0x81, 0x78, 1, 0 },
    { 0x82, 0x84, 1, 0 },
    { 0x83, 0x88, 1, 0 },
    { 0x84, 0xA8, 1, 0 },
    { 0x85, 0xE3, 1, 0 },
    { 0x86, 0x88, 1, 0 },
    { 0x11, 0x00, 0, 120 },     // sleep out
};

// The strip has been copied into the frame buffer: LVGL may draw into it again
static bool IRAM_ATTR trans_done (esp_lcd_panel_handle_t p, esp_lcd_dpi_panel_event_data_t *e, void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);

    return false;
}

static void flush (lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    esp_lcd_panel_draw_bitmap(panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

void display_backlight (uint8_t pct)
{
    ledcWrite(PIN_LCD_BL, (uint32_t)min<uint8_t>(pct, 100) * 255 / 100);
}

#define CHECK(x, what) do { esp_err_t e_ = (x); if(e_ != ESP_OK) { Serial.printf("display: %s failed (%s)\n", what, esp_err_to_name(e_)); return NULL; } } while(0)

lv_display_t *display_begin (void)
{
    ledcAttach(PIN_LCD_BL, 20000, 8);
    display_backlight(0);

    // supplies: LDO3 2.5 V for the MIPI DSI PHY, LDO4 3.3 V
    esp_ldo_channel_handle_t ldo3, ldo4;
    esp_ldo_channel_config_t ldo3_cfg = {}, ldo4_cfg = {};
    ldo3_cfg.chan_id = 3;
    ldo3_cfg.voltage_mv = 2500;
    ldo4_cfg.chan_id = 4;
    ldo4_cfg.voltage_mv = 3300;
    CHECK(esp_ldo_acquire_channel(&ldo3_cfg, &ldo3), "LDO3");
    CHECK(esp_ldo_acquire_channel(&ldo4_cfg, &ldo4), "LDO4");

    // panel reset
    pinMode(PIN_LCD_RST, OUTPUT);
    digitalWrite(PIN_LCD_RST, LOW);
    delay(10);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(20);

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {};
    bus_cfg.bus_id = 0;
    bus_cfg.num_data_lanes = 2;
    bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_cfg.lane_bit_rate_mbps = LANE_MBPS;
    CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &bus), "DSI bus");

    // commands to the panel controller
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t io_cfg = {};
    io_cfg.virtual_channel = 0;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    CHECK(esp_lcd_new_panel_io_dbi(bus, &io_cfg, &io), "DBI io");

    for(auto &c : ek79007_init) {
        CHECK(esp_lcd_panel_io_tx_param(io, c.cmd, c.len ? &c.data : NULL, c.len), "panel init");
        if(c.delay_ms)
            delay(c.delay_ms);
    }

    // video: timing of the CrowPanel 9" (VFP 12, the 21 of the Elecrow examples makes the picture twitch)
    esp_lcd_dpi_panel_config_t dpi = {};
    dpi.virtual_channel = 0;
    dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi.dpi_clock_freq_mhz = DPI_MHZ;
    dpi.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi.num_fbs = 1;
    dpi.video_timing.h_size = LCD_W;
    dpi.video_timing.v_size = LCD_H;
    dpi.video_timing.hsync_pulse_width = 70;
    dpi.video_timing.hsync_back_porch = 160;
    dpi.video_timing.hsync_front_porch = 160;
    dpi.video_timing.vsync_pulse_width = 10;
    dpi.video_timing.vsync_back_porch = 23;
    dpi.video_timing.vsync_front_porch = 12;
    dpi.flags.use_dma2d = 1;
    CHECK(esp_lcd_new_panel_dpi(bus, &dpi, &panel), "DPI panel");
    CHECK(esp_lcd_panel_init(panel), "panel start");

    // LVGL: two strips in PSRAM, aligned for DMA
    size_t buf_size = LCD_W * BUF_LINES * 2;
    void *buf1 = heap_caps_aligned_alloc(128, buf_size, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_aligned_alloc(128, buf_size, MALLOC_CAP_SPIRAM);
    if(buf1 == NULL || buf2 == NULL) {
        Serial.println("display: no memory for the draw buffers");
        return NULL;
    }

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(disp, flush);
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = trans_done;
    CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, disp), "callbacks");

    display_backlight(100);

    return disp;
}

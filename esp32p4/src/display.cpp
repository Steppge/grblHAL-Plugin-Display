// Panel of the CrowPanel Advanced 9": EK79007, 1024x600, MIPI-DSI with 2 lanes at 1000 Mbps, RGB565.
// LVGL renders into two strip buffers in internal RAM, DMA2D copies them into the panel's frame buffer in PSRAM.
//
// The video output (DPI) reads the frame buffer from PSRAM all the time. When it does not get a line in time
// ("underrun", logged by the IDF from its interrupt) the picture stays shifted sideways. Against it:
// the strips are in internal RAM (half the PSRAM traffic while drawing), a lower pixel clock, and after an
// underrun the video output is restarted with the picture kept (display_poll()).

#include <Arduino.h>
#include <esp_ldo_regulator.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_cache.h>
#include <esp_memory_utils.h>
#include <esp_rom_sys.h>
#include <esp_rom_uart.h>
#include "display.h"
#include "pins.h"

#define LANE_MBPS       1000
// pixel clock: 46 MHz gives about 50 frames per second (1414 x 645 pixels per frame with the porches).
// 51 MHz (56 fps) left the PSRAM too little room now and then.
#define DPI_MHZ           46
#define BUF_LINES         32    // LVGL strip height in internal RAM (2 x 64 KB)
#define BUF_LINES_PSRAM  120    // fallback when the internal RAM is short
#define FB_BYTES        (LCD_W * LCD_H * 2)
#define RESYNC_GAP_MS   1000    // at most one automatic resync per second

static esp_lcd_panel_handle_t panel = NULL;
static esp_lcd_dsi_bus_handle_t bus = NULL;
static esp_lcd_panel_io_handle_t io = NULL;
static uint8_t backlight = 0;           // percent, restored after a resync
static esp_lcd_dpi_panel_config_t dpi = {};
static lv_display_t *disp_lv = NULL;
static volatile bool underrun = false;
static uint32_t underruns = 0, resyncs = 0;

// esp_rom_printf, which the IDF uses in interrupts, goes through here: watch for the underrun message
static void IRAM_ATTR rom_putc (char c)
{
    static const char key[] = "underrun";
    static uint8_t pos = 0;

    pos = c == key[pos] ? pos + 1 : (c == key[0] ? 1 : 0);
    if(key[pos] == '\0') {
        underrun = true;
        pos = 0;
    }
    esp_rom_output_putc(c);
}

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
    if(panel == NULL || esp_lcd_panel_draw_bitmap(panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px) != ESP_OK)
        lv_display_flush_ready(disp);
}

// panel reset, DSI bus and the init commands of the panel controller
static esp_err_t link_start (void)
{
    pinMode(PIN_LCD_RST, OUTPUT);
    digitalWrite(PIN_LCD_RST, LOW);
    delay(10);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(20);

    esp_lcd_dsi_bus_config_t bus_cfg = {};
    bus_cfg.bus_id = 0;
    bus_cfg.num_data_lanes = 2;
    bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_cfg.lane_bit_rate_mbps = LANE_MBPS;
    esp_err_t e = esp_lcd_new_dsi_bus(&bus_cfg, &bus);

    esp_lcd_dbi_io_config_t io_cfg = {};
    io_cfg.virtual_channel = 0;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    if(e == ESP_OK)
        e = esp_lcd_new_panel_io_dbi(bus, &io_cfg, &io);

    for(auto &c : ek79007_init) {
        if(e != ESP_OK)
            break;
        e = esp_lcd_panel_io_tx_param(io, c.cmd, c.len ? &c.data : NULL, c.len);
        if(c.delay_ms)
            delay(c.delay_ms);
    }
    return e;
}

// video output (DPI) with its frame buffer, optionally filled with a kept picture before the video starts
static esp_err_t dpi_start (const uint8_t *keep)
{
    void *fb = NULL;
    esp_err_t e = esp_lcd_new_panel_dpi(bus, &dpi, &panel);
    if(e == ESP_OK && keep && esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) == ESP_OK && fb) {
        memcpy(fb, keep, FB_BYTES);
        esp_cache_msync(fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);   // to PSRAM before the video reads it
    }
    if(e == ESP_OK)
        e = esp_lcd_panel_init(panel);
    if(e == ESP_OK) {
        esp_lcd_dpi_panel_event_callbacks_t cbs = {};
        cbs.on_color_trans_done = trans_done;
        e = esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, disp_lv);
    }
    return e;
}

// Everything as after a start (panel reset, DSI bus, init commands, video), the picture is kept: copied out of
// the old frame buffer and into the new one before the video starts, so no full redraw follows (a full redraw
// is just the load that causes an underrun). LVGL waits for the last flush at the end of each refresh, so from
// loop() no copy is running. The backlight is off meanwhile (about 0.2 s).
bool display_resync (void)
{
    if(panel == NULL)
        return false;

    void *fb = NULL;
    uint8_t *keep = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if(keep && esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) == ESP_OK && fb) {
        esp_cache_msync(fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);   // written by DMA2D, not by the CPU
        memcpy(keep, fb, FB_BYTES);
    } else if(keep) {
        heap_caps_free(keep);
        keep = NULL;
    }

    uint8_t bl = backlight;
    ledcWrite(PIN_LCD_BL, 0);
    esp_lcd_panel_del(panel);
    panel = NULL;
    esp_lcd_panel_io_del(io);
    io = NULL;
    esp_lcd_del_dsi_bus(bus);
    bus = NULL;

    esp_err_t e = link_start();
    if(e == ESP_OK)
        e = dpi_start(keep);
    display_backlight(bl);
    if(e != ESP_OK)
        Serial.printf("display: resync failed (%s)\n", esp_err_to_name(e));
    else if(keep == NULL) {     // nothing kept: draw everything again
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
    }
    heap_caps_free(keep);
    resyncs++;
    return e == ESP_OK;
}

// from loop(): after an underrun restart the video output, the flag set again by the restart itself is dropped
void display_poll (void)
{
    static uint32_t last = 0;

    if(!underrun)
        return;
    underrun = false;
    underruns++;
    if(millis() - last < RESYNC_GAP_MS)
        return;
    last = millis();
    display_resync();
    underrun = false;
}

void display_report (void)
{
    Serial.printf("display: %u underruns, %u resyncs, pixel clock %u MHz, strips of %u lines in %s\n",
                  (unsigned)underruns, (unsigned)resyncs, DPI_MHZ,
                  (unsigned)(lv_display_get_buf_active(disp_lv) ? lv_display_get_buf_active(disp_lv)->header.h : 0),
                  esp_ptr_external_ram(lv_display_get_buf_active(disp_lv) ? lv_display_get_buf_active(disp_lv)->data : NULL) ? "PSRAM" : "internal RAM");
}

void display_backlight (uint8_t pct)
{
    backlight = min<uint8_t>(pct, 100);
    ledcWrite(PIN_LCD_BL, (uint32_t)backlight * 255 / 100);
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

    CHECK(link_start(), "panel start");

    // video: timing of the CrowPanel 9" (VFP 12, the 21 of the Elecrow examples makes the picture twitch)
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

    // LVGL: two strips in internal RAM (in PSRAM if short), aligned for DMA
    size_t buf_size = LCD_W * BUF_LINES * 2;
    void *buf1 = heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *buf2 = heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if(buf1 == NULL || buf2 == NULL) {
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        buf_size = LCD_W * BUF_LINES_PSRAM * 2;
        buf1 = heap_caps_aligned_alloc(128, buf_size, MALLOC_CAP_SPIRAM);
        buf2 = heap_caps_aligned_alloc(128, buf_size, MALLOC_CAP_SPIRAM);
        Serial.println("display: draw buffers in PSRAM (internal RAM short)");
    }
    if(buf1 == NULL || buf2 == NULL) {
        Serial.println("display: no memory for the draw buffers");
        return NULL;
    }
    esp_rom_install_channel_putc(1, rom_putc);

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(disp, flush);
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    disp_lv = disp;

    CHECK(dpi_start(NULL), "DPI panel");

    display_backlight(100);

    return disp;
}

// grblHAL touch display: Elecrow CrowPanel Advanced 9" ESP32-P4 (1024x600, EK79007 + GT911).
// Talks to the controller over USB to the Octopus-Bridge (usb_link.cpp, the gamepad receiver passed through)
// or its machine network; shared code in ../esp32common: link, WiFi with OTA and web interface.
// Firmware updates also from the microSD card at start (sd_update.cpp).

#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "touch.h"
#include "beep.h"
#include "link.h"
#include "wlan.h"
#include "ui.h"
#include "usb_link.h"
#include "sd_update.h"
#include "deck.h"
#include "battery.h"

#define UI_MS   100     // ui_update() interval

static uint32_t tick (void)
{
    return millis();
}

// Called by a firmware update, which blocks the main loop
static void ota_progress (unsigned pct)
{
    ui_ota_progress(pct);
    lv_refr_now(NULL);
}

// Console "shot": the screen as raw RGB565 (little endian) over the console, for tools/shot.py.
// "SHOT <w> <h> <bytes>\n", the pixel data, "\nSHOTEND\n". Blocks the main loop for about 14 s at 921600 baud.
static void shot (void)
{
    lv_obj_t *scr = lv_screen_active();
    int32_t w = lv_obj_get_width(scr), h = lv_obj_get_height(scr);
    uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
    uint32_t size = stride * h;
    uint8_t *mem = (uint8_t *)heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
    lv_draw_buf_t db;

    if(!mem) {
        Serial.println("shot: no memory");
        return;
    }
    lv_draw_buf_init(&db, w, h, LV_COLOR_FORMAT_RGB565, stride, mem, size);
    if(lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &db) != LV_RESULT_OK)
        Serial.println("shot: failed");
    else {
        Serial.printf("SHOT %ld %ld %lu\n", (long)w, (long)h, (unsigned long)size);
        for(uint32_t i = 0; i < size; i += 4096) {
            Serial.write(mem + i, min((uint32_t)4096, size - i));
            delay(1);
        }
        Serial.flush();
        Serial.print("\nSHOTEND\n");
    }
    heap_caps_free(mem);
}

// Settings over the serial console (lower case lines; the PC-Deck app sends upper case ones, see deck.cpp):
//   ssid <network>, pass <password>, ota <password>, name <name>, web 0|1|2, wifi on|off, batt [volts], status,
//   mode pc|cnc (only with BIGDECK 1), shot (screen image, see tools/shot.py),
//   resync (restarts the video output when the picture is shifted sideways),
//   gpupdate <size> <crc32> (gamepad receiver firmware, see tools/gp_update.py)
static void console (char *line)
{
    {
        char *arg = strchr(line, ' ');
        if(arg)
            *arg++ = '\0';
        else
            arg = (char *)"";

        if(!strcmp(line, "ssid"))
            strlcpy(wifi_cfg.ssid, arg, sizeof(wifi_cfg.ssid));
        else if(!strcmp(line, "pass"))
            strlcpy(wifi_cfg.pass, arg, sizeof(wifi_cfg.pass));
        else if(!strcmp(line, "ota"))
            strlcpy(wifi_cfg.ota, arg, sizeof(wifi_cfg.ota));
        else if(!strcmp(line, "name"))
            wifi_set_host(arg);
        else if(!strcmp(line, "web"))
            wifi_set_web_mode((uint8_t)atoi(arg));
        else if(!strcmp(line, "wifi"))
            wifi_cfg.enabled = !strcmp(arg, "on");
        else if(!strcmp(line, "linklog")) {
            link_log = !strcmp(arg, "on");
            Serial.printf("link log %s\n",link_log ? "on" : "off");
            return;
        }
        else if(!strcmp(line, "scan")) {
            wifi_scan_console();
            return;
        }
        else if(!strcmp(line, "mode")) {
            ui_set_deck_mode(!strcmp(arg, "pc"));
            return;
        }
        else if(!strcmp(line, "batt")) {
            battery_console(arg);
            return;
        }
        else if(!strcmp(line, "battlog")) {
            battlog_console(arg);
            return;
        }
        else if(!strcmp(line, "shot")) {
            shot();
            return;
        }
        else if(!strcmp(line, "gplog")) {
            gp_log = !strcmp(arg, "on");
            Serial.printf("gamepad log %s\n", gp_log ? "on" : "off");
            return;
        }
        else if(!strcmp(line, "gpupdate")) {
            gp_update_console(arg);
            return;
        }
        else if(!strcmp(line, "resync")) {
            uint32_t t = millis();
            bool ok = display_resync();
            Serial.printf("resync %s, %lu ms\n", ok ? "done" : "failed", (unsigned long)(millis() - t));
            return;
        }
        else if(strcmp(line, "status")) {
            Serial.println("commands: ssid <network>, pass <password>, ota <password>, name <name>, web 0|1|2, wifi on|off, batt [volts], battlog on|off|dump, mode pc|cnc, scan, shot, resync, gplog on|off, gpupdate, status");
            return;
        }

        if(strcmp(line, "status") && strcmp(line, "web"))
            wifi_apply();       // stores the settings and (re)starts WiFi

        // passwords are not echoed
        Serial.printf("WiFi %s, network '%s', password %s, OTA password %s, name %s.local, web %u, state %d, IP %s\n",
                      wifi_cfg.enabled ? "on" : "off", wifi_cfg.ssid, *wifi_cfg.pass ? "set" : "empty",
                      *wifi_cfg.ota ? "set" : "empty", wifi_host(), wifi_web_mode(), (int)wifi_state(), wifi_ip());
        usb_link_report();
        display_report();
    }
}

void setup (void)
{
    Serial.setRxBufferSize(16384);  // a whole PC-Deck icon (8 KB) and the commands around it
    Serial.begin(921600);           // the speed of the PC-Deck app, also for the console
    // the receive interrupt by default only comes at 120 of the 128 FIFO bytes: 8 bytes = 87 us at 921600 baud,
    // too little room against other interrupts, PC-Deck blocks of 4 KB lost bytes. 32 leaves 1 ms.
    Serial.setRxFIFOFull(32);
    delay(200);
    Serial.printf("\ngrblHAL display P4 %s, built %s %s, chip rev %d, %u MHz, PSRAM %u bytes\n", DISPLAY_VERSION, __DATE__, __TIME__,
                  ESP.getChipRevision(), ESP.getCpuFreqMHz(), ESP.getPsramSize());
    display_boot_info();

    lv_init();
    lv_tick_set_cb(tick);

    lv_display_t *disp = display_begin();
    Serial.println(disp ? "display ok" : "display FAILED");
    if(disp && touch_begin()) {
        touch_indev(disp);
        Serial.println("touch ok");
    }

    sd_update_check(disp != NULL);  // before WiFi: the card and the WiFi chip share the SDMMC host

    beep_begin();
    battery_begin();
    link_begin();
    usb_link_begin();
    wifi_begin(ota_progress);       // before the user interface: the WiFi chip link needs internal DMA memory
    deck_begin(console);
    if(disp)
        ui_init();
    Serial.printf("free internal RAM %u bytes, PSRAM %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    wifi_boot_done();               // WiFi connects last, some seconds after the rest is up
}

void loop (void)
{
    static uint32_t last_ui = 0;

    link_poll();
    deck_poll();                    // also reads the console
    display_poll();                 // video output: resync after an underrun
    battery_poll();

    // firmware updates only while the machine is idle, in alarm or not connected
    wifi_poll(!mc.online || mc.state == STATE_IDLE || (mc.state & (STATE_ALARM | STATE_ESTOP)));
    wifi_time_poll();

    // the machine network only without the USB cable (the bridge has its access point off meanwhile);
    // at start the bridge gets 3 s to open the USB channel
    static uint32_t usb_seen = 3000;
    if(usb_link_mounted())
        usb_seen = millis();
    wifi_machine_allowed(millis() - usb_seen > 300);

    static int last_wifi = -1;      // for the link log: WiFi state changes
    if(link_log && (int)wifi_state() != last_wifi) {
        last_wifi = (int)wifi_state();
        Serial.printf("[%lu] wifi state %d, IP %s\n",(unsigned long)millis(), last_wifi, wifi_ip());
    }

    // redraw as soon as a status line came in, so the position follows the controller's timing;
    // without status lines every UI_MS
    static uint32_t last_count = 0;
    if(mc.status_count != last_count || millis() - last_ui >= UI_MS) {
        last_count = mc.status_count;
        last_ui = millis();
        ui_update();
    }

    lv_timer_handler();
    delay(2);
}

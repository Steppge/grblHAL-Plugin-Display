// grblHAL touch display: Elecrow CrowPanel Advanced 9" ESP32-P4 (1024x600, EK79007 + GT911).
// Talks to the controller over a serial link (header J7), shared code in ../esp32common: link, WiFi with OTA,
// sender bridge (second UART on J7) and web interface.

#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "touch.h"
#include "beep.h"
#include "link.h"
#include "wlan.h"
#include "ui.h"

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

// Settings over the serial console until the display has its own WiFi screen:
//   ssid <network>, pass <password>, ota <password>, name <name>, web 0|1|2, wifi on|off, status
static void console (void)
{
    static char line[100];
    static uint8_t len = 0;

    while(Serial.available()) {
        char c = Serial.read();
        if(c != '\n' && c != '\r') {
            if(len < sizeof(line) - 1)
                line[len++] = c;
            continue;
        }
        if(len == 0)
            continue;
        line[len] = '\0';
        len = 0;

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
        else if(strcmp(line, "status")) {
            Serial.println("commands: ssid <network>, pass <password>, ota <password>, name <name>, web 0|1|2, wifi on|off, status");
            continue;
        }

        if(strcmp(line, "status") && strcmp(line, "web"))
            wifi_apply();       // stores the settings and (re)starts WiFi

        // passwords are not echoed
        Serial.printf("WiFi %s, network '%s', password %s, OTA password %s, name %s.local, web %u, state %d, IP %s\n",
                      wifi_cfg.enabled ? "on" : "off", wifi_cfg.ssid, *wifi_cfg.pass ? "set" : "empty",
                      *wifi_cfg.ota ? "set" : "empty", wifi_host(), wifi_web_mode(), (int)wifi_state(), wifi_ip());
    }
}

void setup (void)
{
    Serial.begin(115200);
    delay(200);
    Serial.printf("\ngrblHAL display P4, chip rev %d, %u MHz, PSRAM %u bytes\n", ESP.getChipRevision(), ESP.getCpuFreqMHz(), ESP.getPsramSize());

    lv_init();
    lv_tick_set_cb(tick);

    lv_display_t *disp = display_begin();
    Serial.println(disp ? "display ok" : "display FAILED");
    if(disp && touch_begin()) {
        touch_indev(disp);
        Serial.println("touch ok");
    }

    beep_begin();
    link_begin();
    wifi_begin(ota_progress);       // before the user interface: the WiFi chip link needs internal DMA memory
    if(disp)
        ui_init();
    Serial.printf("free internal RAM %u bytes, PSRAM %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop (void)
{
    static uint32_t last_ui = 0;

    link_poll();
    console();

    // firmware updates only while the machine is idle, in alarm or not connected
    wifi_poll(!mc.online || mc.state == STATE_IDLE || (mc.state & (STATE_ALARM | STATE_ESTOP)));

    if(millis() - last_ui >= UI_MS) {
        last_ui = millis();
        ui_update();
    }

    lv_timer_handler();
    delay(2);
}

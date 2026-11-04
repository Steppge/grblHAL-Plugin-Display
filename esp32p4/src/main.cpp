// grblHAL touch display: Elecrow CrowPanel Advanced 9" ESP32-P4 (1024x600, EK79007 + GT911).
// Talks to the controller over USB to the Octopus-Bridge (usb_link.cpp, the gamepad receiver passed through)
// or a serial cable on header J7; shared code in ../esp32common: link, WiFi with OTA and web interface.

#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "touch.h"
#include "beep.h"
#include "link.h"
#include "wlan.h"
#include "ui.h"
#include "usb_link.h"

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
        usb_link_report();
    }
}

void setup (void)
{
    Serial.begin(115200);
    delay(200);
    Serial.printf("\ngrblHAL display P4 %s, built %s %s, chip rev %d, %u MHz, PSRAM %u bytes\n", DISPLAY_VERSION, __DATE__, __TIME__,
                  ESP.getChipRevision(), ESP.getCpuFreqMHz(), ESP.getPsramSize());

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
    usb_link_begin();
    Serial.printf("link over %s\n", usb_link_selected() ? "USB" : "the cable on J7");
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
    wifi_time_poll();

    // the machine network only without the USB cable (the bridge has its access point off meanwhile);
    // at start the bridge gets 3 s to open the USB channel
    static uint32_t usb_seen = 3000;
    if(usb_link_mounted())
        usb_seen = millis();
    wifi_machine_allowed(millis() - usb_seen > 300);

    if(millis() - last_ui >= UI_MS) {
        last_ui = millis();
        ui_update();
    }

    lv_timer_handler();
    delay(2);
}

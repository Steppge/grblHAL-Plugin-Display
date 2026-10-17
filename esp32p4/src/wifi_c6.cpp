// WiFi of the ESP32-P4 comes from an ESP32-C6 co-processor (ESP-Hosted over SDIO). The reset line of the
// C6 differs between board revisions (IO54 on V1.2, IO32 in the schematic): both are tried.
// The C6 firmware is not updated, even when ESP-Hosted reports a newer version.

#include <Arduino.h>
#include <WiFi.h>
#include "esp32-hal-hosted.h"
#include "wlan.h"
#include "pins.h"

void wifi_board_init (void)
{
    static const int8_t rst[] = { PIN_C6_RST, PIN_C6_RST_ALT };

    for(int8_t pin : rst) {
        WiFi.setPins(PIN_C6_CLK, PIN_C6_CMD, PIN_C6_D0, PIN_C6_D1, PIN_C6_D2, PIN_C6_D3, pin);
        if(hostedInitWiFi()) {
            uint32_t major, minor, patch;
            hostedGetSlaveVersion(&major, &minor, &patch);
            Serial.printf("WiFi: %s found (reset IO%d), ESP-Hosted %lu.%lu.%lu\n", hostedGetSlaveTargetName(), pin,
                          (unsigned long)major, (unsigned long)minor, (unsigned long)patch);
            return;
        }
    }

    Serial.println("WiFi: ESP32-C6 does not answer");
}

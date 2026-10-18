// WiFi of the ESP32-P4 comes from an ESP32-C6 co-processor (ESP-Hosted over SDIO).
// The C6 keeps running when only the P4 restarts (reset button, USB plug-in), so it is reset first:
// otherwise the SDIO link does not come up and ESP-Hosted restarts the P4 over and over.
// The C6 firmware is not updated, even when ESP-Hosted reports a newer version.

#include <Arduino.h>
#include <WiFi.h>
#include "esp32-hal-hosted.h"
#include "wlan.h"
#include "pins.h"

#define C6_RESET_MS     100
#define C6_BOOT_MS     1500

void wifi_board_init (void)
{
    pinMode(PIN_C6_RST, OUTPUT);
    digitalWrite(PIN_C6_RST, LOW);
    delay(C6_RESET_MS);
    digitalWrite(PIN_C6_RST, HIGH);
    delay(C6_BOOT_MS);

    WiFi.setPins(PIN_C6_CLK, PIN_C6_CMD, PIN_C6_D0, PIN_C6_D1, PIN_C6_D2, PIN_C6_D3, PIN_C6_RST);
    if(hostedInitWiFi()) {
        uint32_t major, minor, patch;
        hostedGetSlaveVersion(&major, &minor, &patch);
        Serial.printf("WiFi: %s found, ESP-Hosted %lu.%lu.%lu\n", hostedGetSlaveTargetName(),
                      (unsigned long)major, (unsigned long)minor, (unsigned long)patch);
    } else
        Serial.println("WiFi: ESP32-C6 does not answer");
}

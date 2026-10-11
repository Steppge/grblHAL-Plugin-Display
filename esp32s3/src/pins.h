#pragma once

// MKS TS35 on the ESP32-S3 (SPI2 on its native pins 11/12/13, no GPIO matrix delay).
// EXP pin numbers as printed on the Octopus (the TS35 cables are plugged in rotated).
#define PIN_SPI_SCK     12  // Octopus EXP2-2
#define PIN_SPI_MOSI    11  // Octopus EXP2-6
#define PIN_SPI_MISO    13  // Octopus EXP2-1
#define PIN_TFT_CS      10  // Octopus EXP1-7
#define PIN_TFT_DC       9  // Octopus EXP1-8
#define PIN_TFT_RST     14  // Octopus EXP1-4
#define PIN_TFT_BL      21  // Octopus EXP1-3
#define PIN_TOUCH_CS     8  // Octopus EXP1-5
#define PIN_TOUCH_INT   18  // Octopus EXP1-6
#define PIN_BEEPER       7  // Octopus EXP1-1 (gray cable: EXP1 wire 10)

// Link to the Octopus USART2 header (PD5 TX2 / PD6 RX2 next to USB-C, grblHAL stream instance 1)
#define PIN_LINK_TX     17  // to Octopus PD6 (RX)
#define PIN_LINK_RX     16  // from Octopus PD5 (TX)
#define LINK_BAUD   115200

// Second UART for a sender (LightBurn) over WiFi, Octopus WIFI socket (USART3, grblHAL stream instance 2)
#define PIN_SENDER_TX    1  // to Octopus PD9 (RX), WIFI socket pin 16
#define PIN_SENDER_RX    2  // from Octopus PD8 (TX), WIFI socket pin 15
#define SENDER_BAUD 921600
#define SENDER_PORT     23  // TCP port for the sender (telnet)

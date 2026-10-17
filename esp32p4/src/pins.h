#pragma once

// Elecrow CrowPanel Advanced 9" ESP32-P4 (board V1.2), 1024x600

// Panel: EK79007 over MIPI-DSI (2 lanes), see display.cpp
#define PIN_LCD_RST     41
#define PIN_LCD_BL      31      // backlight PWM, active high

// Touch: GT911 on I2C, address 0x5D (INT low during reset)
#define PIN_TOUCH_SDA   45
#define PIN_TOUCH_SCL   46
#define PIN_TOUCH_RST   40
#define PIN_TOUCH_INT   42

// Speaker: I2S amplifier, enable active low
#define PIN_I2S_BCLK    22
#define PIN_I2S_LRCLK   21
#define PIN_I2S_DOUT    23
#define PIN_AMP_EN      30

// Link to the controller: J10 "UART3-IN" (level shifted to 5 V), to the Octopus USART2 (PD6 RX / PD5 TX)
#define PIN_LINK_TX     34
#define PIN_LINK_RX     33
#define LINK_BAUD   115200

// Second UART for a sender over WiFi: J2 "UART1" (3.3 V), to the Octopus USART3 on the WIFI socket (PD9 RX / PD8 TX)
#define PIN_SENDER_TX   47
#define PIN_SENDER_RX   48
#define SENDER_BAUD 921600
#define SENDER_PORT     23      // TCP port for the sender (telnet)

// ESP32-C6 WiFi co-processor (ESP-Hosted over SDIO); reset is IO54 on board V1.2, IO32 in the schematic
#define PIN_C6_CLK      18
#define PIN_C6_CMD      19
#define PIN_C6_D0       17
#define PIN_C6_D1       16
#define PIN_C6_D2       15
#define PIN_C6_D3       14
#define PIN_C6_RST      54
#define PIN_C6_RST_ALT  32

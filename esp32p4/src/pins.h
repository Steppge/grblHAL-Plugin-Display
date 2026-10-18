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

// Link to the controller and the second UART for a sender over WiFi, both on the 24-pin header J7
// (3.3 V, direct GPIOs; J10 "UART3-IN" and J2 "UART1" are not used):
//   J7 pin 20 GPIO50 TX -> Octopus PD6 (USART2 RX)     J7 pin 18 GPIO51 RX <- Octopus PD5 (USART2 TX)
//   J7 pin 16 GPIO52 TX -> Octopus PD9 (USART3 RX)     J7 pin 19 GPIO49 RX <- Octopus PD8 (USART3 TX)
//   J7 pins 2/4/6 VDD5V (supply from the Octopus), pins 7/8 GND
#define PIN_LINK_TX     50
#define PIN_LINK_RX     51
#define LINK_BAUD   115200

#define PIN_SENDER_TX   52
#define PIN_SENDER_RX   49
#define SENDER_BAUD 921600
#define SENDER_PORT     23      // TCP port for the sender (telnet)

// ESP32-C6 WiFi co-processor (ESP-Hosted over SDIO); its enable (reset) is IO32 (schematic V1.2),
// the IO54 default of ESP-Hosted is a free pin on J7 here
#define PIN_C6_CLK      18
#define PIN_C6_CMD      19
#define PIN_C6_D0       17
#define PIN_C6_D1       16
#define PIN_C6_D2       15
#define PIN_C6_D3       14
#define PIN_C6_RST      32

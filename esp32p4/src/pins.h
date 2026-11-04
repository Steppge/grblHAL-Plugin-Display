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

// Link to the controller: over USB-C "USB 2.0" (J16) to the Octopus-Bridge (ESP32-S3 at the controller),
// two CDC channels: 0 = display link, 1 = gamepad (passed through to the D1 Mini below), see usb_link.cpp.
// As a last resort the link runs over a cable on the 24-pin header J7 (3.3 V, direct GPIOs), display only:
//   J7 pin 20 GPIO50 TX -> Octopus PD6 (USART2 RX)     J7 pin 18 GPIO51 RX <- Octopus PD5 (USART2 TX)
// The sender (LightBurn over WiFi) is taken by the Octopus-Bridge, GPIO49/52 on J7 stay free.
#define LINK_USB        1
#define PIN_LINK_TX     50
#define PIN_LINK_RX     51
#define LINK_BAUD   921600

// Gamepad receiver (D1 Mini ESP32 in the display housing) on J7, passed through to USB channel 1:
//   J7 pin 9 GPIO2 TX -> D1 IO16 (RX)    J7 pin 11 GPIO3 RX <- D1 IO17 (TX)    J7 pin 1 3.3 V -> D1 3V3, pin 7 GND
// (J2 "UART1" would fit as well, but its RX pad is torn off on this board)
#define PIN_GAMEPAD_TX  2
#define PIN_GAMEPAD_RX  3
#define GAMEPAD_BAUD 921600

// ESP32-C6 WiFi co-processor (ESP-Hosted over SDIO); its enable (reset) is IO32 (schematic V1.2),
// the IO54 default of ESP-Hosted is a free pin on J7 here
#define PIN_C6_CLK      18
#define PIN_C6_CMD      19
#define PIN_C6_D0       17
#define PIN_C6_D1       16
#define PIN_C6_D2       15
#define PIN_C6_D3       14
#define PIN_C6_RST      32

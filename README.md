# grblHAL-Plugin-Display

A touch display for a [grblHAL](https://github.com/grblHAL) CNC or laser machine: an **ESP32-S3** drives an
**MKS TS35** (3.5", 480x320, ST7796 + XPT2046 resistive touch) and talks to the grblHAL board over a UART.

The display shows the machine state, the position and a map of the machine area with the live tool path and a
**preview of the G-code file** to be run from the SD card. It starts and stops jobs, jogs, probes Z, fires laser
test pulses and edits the grblHAL settings.

Developed for a BTT Octopus Pro v1.0 (STM32F446), see
[Steppge/grblHAL-OctopusPro-CNC-Laser](https://github.com/Steppge/grblHAL-OctopusPro-CNC-Laser).
The plugin only uses grblHAL core APIs and is not tied to a driver or MCU.

> **Prototype.** Keep a hand on the power or emergency stop switch when trying it on your machine.
> The STOP button on the display is a software stop, it does not replace an emergency stop.

## Contents

| File | |
|---|---|
| [`display_link.c`](display_link.c) | grblHAL plugin (`DISPLAY_LINK_ENABLE`): status to the display, commands from it |
| [`my_plugin.c.example`](my_plugin.c.example) | Starts the plugin from `my_plugin_init()` |
| [`esp32s3/`](esp32s3/) | ESP32-S3 firmware (PlatformIO, Arduino, LovyanGFX, LVGL 9) |

## Screens

- **Main**: state (with alarm names such as `HARD LIM`, `NO HOME`), feed, power/speed, work offset, link state;
  map card and X/Y/Z card; six buttons that change between idle and job:
  - idle: Home (hold), Unlock / Reset / Hold 3s, X0 Y0 (hold), Z0 (hold), Hold, Load / Start (hold)
  - job: Feed -/+ and Power -/+ (tap ±10 %, hold 100 %), Hold / Stop (hold), Resume
- **Map**: the machine area with a frame, the live trace of the tool while the laser/spindle is on, the tool
  crosshair and the G54 origin. After a file has been loaded it switches to the **G-code preview** (to do: beige,
  done: colored). The button next to the zoom button toggles between preview and machine area.
- **Zoom**: the map full screen with two buttons, back and STOP. First tap on STOP: feed hold, the laser/spindle
  is switched off once the machine stands; the button then becomes ABORT (reset).
- **Menu**: Move (jog X/Y/Z, steps 0.01-10 mm), Probe Z (plate thickness, feed), Laser (test pulse power and
  time), SD card (file list, Load), Settings (groups, list, edit with keypad), Unlock.
- Errors of commands and SD jobs appear in a banner. After an SD job has stopped with an error grblHAL refuses
  jogs until a reset, the main screen then offers Reset.
- Beeper: click on button presses, three beeps on an alarm or error, two when a job of at least 10 s has finished.

## Installation (PlatformIO)

1. Add this repository to your grblHAL driver, next to the other plugins:
   ```
   git submodule add https://github.com/Steppge/grblHAL-Plugin-Display display
   ```
2. `platformio.ini`, in your environment: add `display` to `lib_deps` and `-D ADD_MY_PLUGIN=1` to `build_flags`.
3. Copy `my_plugin.c.example` to `Src/my_plugin.c` (or add its call to your existing `my_plugin_init()`).
4. `Inc/my_machine.h`:
   ```c
   #define DISPLAY_LINK_ENABLE     1 // ESP32-S3 touch display
   //#define DISPLAY_LINK_STREAM   1 // serial stream instance of the UART the ESP32-S3 is connected to
   //#define DISPLAY_LINK_BAUD 115200
   ```
5. Build and flash the ESP32-S3 firmware: open `esp32s3/` in PlatformIO, environment `display`.
   `esp32s3/platformio.ini` uses the pioarduino platform (Arduino 3.3, ESP-IDF 5.5) for an ESP32-S3-DevKitC-1
   N16R8 (16 MB flash, 8 MB octal PSRAM), the PSRAM is needed.

For CMake based builds use the included `CMakeLists.txt` (library `display`).

`DISPLAY_LINK_STREAM` is the instance number of a serial stream of your driver. On the BTT Octopus Pro with the
main connection on USB: 0 = USART1 (TFT header), 1 = USART2 (PD5 TX2 / PD6 RX2 header next to the USB-C
socket), 2 = USART3 (WIFI socket). The port must not be used by anything else.

## Wiring

The TS35 is connected with its two EXP ribbon cables. Wire numbers count from the marked wire 1 of the cable
(red stripe) and assume the cables are plugged into the TS35 with the key, as they come.

| Cable | Wire | TS35 signal | ESP32-S3 |
|---|---|---|---|
| EXP1 | 1 | 5 V | 5Vin |
| EXP1 | 2 | GND | GND |
| EXP1 | 3 | LCD DC | GPIO9 |
| EXP1 | 4 | LCD CS | GPIO10 |
| EXP1 | 5 | Touch INT | GPIO18 |
| EXP1 | 6 | Touch CS | GPIO8 |
| EXP1 | 7 | LCD reset | GPIO14 |
| EXP1 | 8 | Backlight | GPIO21 |
| EXP1 | 10 | Beeper | GPIO15 |
| EXP2 | 2 | GND | GND |
| EXP2 | 5 | SPI MOSI | GPIO11 |
| EXP2 | 9 | SPI SCK | GPIO12 |
| EXP2 | 10 | SPI MISO | GPIO13 |

All other wires stay open. EXP2 wire 1 carries the 3.3 V of the TS35 (useful to check its supply), EXP2 wire 3
is the reset button of the TS35 (to GND), do not connect it to a supply.

| grblHAL board UART | ESP32-S3 |
|---|---|
| TX | GPIO16 (RX) |
| RX | GPIO17 (TX) |
| GND | GND |

Supply: 5 V from the controller board (e.g. EXP1 5V/GND of the Octopus) to 5Vin of the ESP32-S3 and to EXP1 wire 1.

### Lessons from the bring-up

- **5Vin of the ESP32-S3 DevKitC-1 clones is an input only** while the `IN-OUT` solder bridge is open: with USB
  power the TS35 then gets no supply, it is only fed through the signal pins. Touch may still work, the display
  stays dark. Either close the bridge for USB tests or supply 5 V from outside. With the bridge closed never
  connect the 5 V of the controller and USB at the same time.
- Check the supply first: EXP2 wire 1 must show 3.3 V against GND.
- The MKS TS35 V2.0 can route LCD CS and DC to EXP1 or EXP2 (solder bridges JP1-JP4), the table above is the
  default (EXP1). On the V2.0_002 board the LCD data output is not connected, reading the panel ID returns FFFFFFFF.
- LVGL renders RGB565 little endian, the flush must hand it to LovyanGFX as `lgfx::rgb565_t`, otherwise colors
  are wrong and gradients show rings.
- The beeper draws noticeable current: on USB power the USB serial chip of the ESP32-S3 board may drop out.
- Map orientation: `MAP_X_LEFT` and `MAP_Y_DOWN` in `esp32s3/src/map.cpp` (defaults for a machine with +X to
  the left and +Y to the front, seen from the front).

## Notes for the grblHAL side

- grblHAL waits for the USB host to read its replies while the USB port is open (DTR set). A program that
  closes the port without dropping DTR leaves commands from the display stalled until the next reboot.
- Lines like `[...]` or `#` are evaluated as expressions when grblHAL is built with `NGC_EXPRESSIONS_ENABLE`,
  a G-code file with such text stops with error 75.

## Protocol (UART, 115200 baud, ASCII lines)

Lines from the board start with `$`, lines from the display with `>`, both end with `*<hh>`, the XOR of all
characters between the start character and `*`. The full description is in the header of
[`display_link.c`](display_link.c):

```
board -> display: $DS (status, 10/s), $DI (machine size), $DN (job name), $AK (command accepted),
                  $FL/$FE (file list), $PB/$PR/$PE (preview), $SV/$SE (settings), $ER (error), $MS (message)
display -> board: >RT (realtime command), >GC (command line), >FL, >LF (laser pulse), >PV (preview),
                  >SL/>SR (settings), >HB (heartbeat)
```

## Status

`$I` shows `[PLUGIN:Display link v0.1 (connected)]`, `(no display)` or `(no serial port)`.

## License

GPL v3 or later, see [LICENSE](LICENSE). Copyright (c) 2026 Steppge.
grblHAL by Terje Io and contributors, LovyanGFX by lovyan03 (FreeBSD license), LVGL (MIT).

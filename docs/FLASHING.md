# Flashing all firmwares

The CrowPanel 9" ESP32-P4 setup has four programmable devices. Each one is flashed **once over its own USB
cable**, after that all updates go over the SD cards, without opening anything.

```
                        USB cable or WiFi                    UART 921600 (display link)
PS4 controller ─BT─ D1 Mini ─UART─ P4 display ═══════════ Octopus-Bridge ───────────────────────── BTT Octopus Pro
                  (receiver)     (CrowPanel 9")            (ESP32-S3)     ───────────────────────── (grblHAL)
                                                                         UART 230400 (gamepad)
```

| Device | Firmware (repository) | USB chip on its "UART" socket | First flash | Updates later |
|---|---|---|---|---|
| BTT Octopus Pro | grblHAL ([grblHAL-OctopusPro-CNC-Laser](https://github.com/Steppge/grblHAL-OctopusPro-CNC-Laser)), with the display link and gamepad plugins | STM32 USB (virtual COM) | `firmware.bin` on its SD card | `upload_firmware.py --reboot` |
| Octopus-Bridge, ESP32-S3-DevKitC-1 N16R8 | [grblHAL-Octopus-Bridge](https://github.com/Steppge/grblHAL-Octopus-Bridge) | CH343 | USB, PlatformIO | `upload_firmware.py --bridge` (over the Octopus SD card) |
| CrowPanel Advanced 9" ESP32-P4 | `esp32p4/` of this repository | CH340 | USB, PlatformIO | `firmware.bin` on the display SD card, or over WiFi |
| D1 Mini ESP32 (gamepad receiver) | `esp32/` of [grblHAL-Plugin-Gamepad](https://github.com/Steppge/grblHAL-Plugin-Gamepad) | CH9102F | USB, PlatformIO (Bluepad32 template) | `gamepad.bin` on the display SD card |

What you need: VS Code with PlatformIO, Python 3 with `pyserial` (`pip install pyserial`), a microSD card for
the Octopus (it has one anyway) and one for the display. Clone every repository **with submodules**
(`git clone --recursive ...`); "Download ZIP" on GitHub leaves them out.

**Order for a new build:** Octopus, Bridge, P4 display, gamepad receiver. Each step can be tested on its own.

> Before any upload close every program that holds the COM port: LightBurn, a serial monitor, the PC-Deck app.
> "Could not open COMx, the port is busy" means one of them still has it.

---

## 1. BTT Octopus Pro (grblHAL)

1. In [`Inc/my_machine.h`](https://github.com/Steppge/grblHAL-OctopusPro-CNC-Laser/blob/octopus-pro-joy/Inc/my_machine.h)
   of the grblHAL project check at least:
   ```c
   #define SDCARD_ENABLE           2       // SD card with YModem upload: needed for the updates over USB
   #define DISPLAY_LINK_ENABLE     1       // display link plugin (this repository, submodule "display")
   #define DISPLAY_LINK_BAUD  921600       // as the Octopus-Bridge
   #define GAMEPAD_ENABLE          1       // gamepad plugin (submodule "gamepad")
   #define GAMEPAD_BAUD       230400       // as the Octopus-Bridge, see the gamepad README "Baud rate"
   ```
2. Build the PlatformIO environment `octopus_pro_f446` (32K bootloader offset of the BTT bootloader).
3. **First time:** copy `.pio/build/octopus_pro_f446/firmware.bin` as `firmware.bin` to the SD card of the
   Octopus, insert it and switch on. The BTT bootloader flashes it and renames it to `FIRMWARE.CUR`.
4. **Updates** over USB (the machine idle, or in an alarm with `--alarm-ok`; nothing moves):
   ```
   python tools/upload_firmware.py --reboot
   ```
   The script copies the file over YModem to the SD card, reboots, and shows `$I` afterwards
   (e.g. `[PLUGIN:Display link v1.2 (connected)]`).

---

## 2. Octopus-Bridge (ESP32-S3)

Wiring and the machine network: see the [bridge README](https://github.com/Steppge/grblHAL-Octopus-Bridge#wiring).

1. **First time over USB:** connect the USB-C socket **"UART"** of the S3 (CH343) to the PC, open the bridge
   project in PlatformIO and upload:
   ```
   pio run -t upload --upload-port COM19
   ```
   (your COM port). This writes the bootloader, the partition table with two app partitions, and the app.
   The console on the same port (115200) shows `Octopus-Bridge 0.4` and, with `wifi`, the password of the
   machine network it made on its first start.
2. **Updates** without a cable at the bridge, from the grblHAL project (Octopus on USB, display link 1.2):
   ```
   python tools/upload_firmware.py --bridge
   ```
   It copies the bridge build `../Octopus-Bridge/.pio/build/bridge/firmware.bin` as `bridge.bin` to the
   Octopus SD card and runs `$BRIDGE`: the Octopus sends it over the display link UART, the bridge checks it
   and restarts (about 20 s, the display link pauses meanwhile). Another file: `--file <path>`.
   A bridge before 0.4 does not answer: flash it once over USB as in step 1.

---

## 3. CrowPanel 9" ESP32-P4 display

Only for **ESP32-P4 chip revision v1.x** (the boards sold so far).

1. Settings: copy the options you want to change from [`esp32p4/src/display_config.h`](../esp32p4/src/display_config.h)
   into a new file `esp32p4/src/my_display_config.h` (not in git, survives updates), e.g.
   ```c
   #define BATTERY 0           // no battery built in
   #define TIME_TZ "GMT0BST,M3.5.0/1,M10.5.0"
   ```
2. **First time over USB:** connect the USB-C socket **"UART"** of the display (CH340), open `esp32p4/` in
   PlatformIO (environment `display`) and upload:
   ```
   pio run -t upload --upload-port COM30
   ```
   Or without PlatformIO: flash `firmware.factory.bin` at address `0x0` with esptool.
3. Connect the USB-C socket **"USB 2.0"** of the display to the "USB" socket of the bridge. On this first
   plug-in the bridge passes the name and password of its machine network; from then on the display also
   connects without the cable.
4. **Updates**, either way:
   - copy `esp32p4/.pio/build/display/firmware.bin` as `firmware.bin` to the display's SD card, switch the
     display off and on: it writes the update with a progress bar and renames the file to `firmware.done`
     (`firmware.bad` when it failed, the old firmware keeps running);
   - or over the home network: enter it on the WiFi screen (with an OTA password), then
     Settings - Display firmware - OK, and upload `firmware.bin` on `http://<name>.local/update`
     (user `admin`, the OTA password). Only while the machine is idle.

---

## 4. Gamepad receiver (D1 Mini ESP32)

The receiver sits in the display housing on J7 (GPIO2/3), see the [gamepad receiver README](https://github.com/Steppge/grblHAL-Plugin-Gamepad/tree/main/esp32).

1. **Build:** Bluepad32 is built from its official template, only `sketch.cpp` and `partitions_ota.csv` come
   from the gamepad repository. The steps (template, platform version, partition table, rollback) are in the
   [receiver README, Build](https://github.com/Steppge/grblHAL-Plugin-Gamepad/tree/main/esp32#build). The first
   build takes some minutes.
2. **First time over USB:** connect the D1 Mini's USB-C (CH9102F) and upload the environment `esp32dev`:
   ```
   pio run -e esp32dev -t upload --upload-port COM10
   ```
   Disconnect its 5 V wire from the display meanwhile, or leave the display switched off. The console
   (115200) shows `esp32_gamepad 1.5 ready, n allowed controller(s)`.
   If Windows says "a device attached to the system is not functioning", unplug and plug the cable again.
3. **Pairing** a controller: on the display Settings - Gamepad - Pair controller, then on the PS4 controller
   hold Share + PS until the light bar flashes.
4. **Updates** without opening the housing: copy `.pio/build/esp32dev/firmware.bin` of the template as
   **`gamepad.bin`** to the display's SD card and switch the display off and on. It sends the file to the
   receiver while it starts (about 15 s, progress on the screen) and renames it to `gamepad.done` or
   `gamepad.bad`. A receiver before sketch 1.4 does not answer: flash it once over USB.

---

## Updates at a glance

| Device | From the PC | Over an SD card |
|---|---|---|
| Octopus | `python tools/upload_firmware.py --reboot` | `firmware.bin` on the Octopus card, switch on |
| Bridge | `python tools/upload_firmware.py --bridge` | (the script uses the Octopus card) |
| P4 display | WiFi: Settings - Display firmware | `firmware.bin` on the display card, switch off/on |
| Gamepad receiver | - | `gamepad.bin` on the display card, switch off/on |

All ESP32 updates check the image before they use it, and the new firmware has to come up once: if it does
not, the device starts the previous one again at the next start (app rollback).

## Other setups

- **ESP32-S3 + MKS TS35 instead of the P4:** no bridge, the S3 sits on the Octopus UART directly. First
  flash over USB (`esp32s3/`, environment `display`), updates over WiFi (environment `ota` or the upload
  page), see the main [README](../README.md#firmware-updates-over-wifi).
- **Gamepad receiver directly on the Octopus** (without the P4): flash it over USB only, there is no update
  path through the display. Set `UART_BAUD` in the sketch and `GAMEPAD_BAUD` to at most 460800
  (see the gamepad README "Baud rate").

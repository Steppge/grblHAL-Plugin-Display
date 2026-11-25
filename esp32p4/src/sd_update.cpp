// Firmware update from the display's own microSD card at start: /firmware.bin (the app image from the
// PlatformIO build folder, not the factory image) is written to the other OTA partition, renamed to
// firmware.done and the display restarts. A broken or wrong file is renamed to firmware.bad.
//
// /gamepad.bin is the firmware of the gamepad receiver (D1 Mini ESP32 on J7, sketch 1.4 or newer with two
// OTA partitions): it is sent over the gamepad UART in blocks, the receiver checks and starts it. Renamed
// to gamepad.done or gamepad.bad. Protocol see grblHAL-Plugin-Gamepad esp32/sketch.cpp ($FW, $FA, $FK, $FD).
//
// The card is on SDMMC slot 0 in 1-bit mode (CLK IO43, CMD IO44, D0 IO39, as Elecrow's V1.2 example).
// The WiFi chip uses slot 1 of the same SDMMC host, so this runs before wifi_begin() and releases the
// host again. Arduino SD_MMC is not used: its default pins drive GPIO45, the touch SDA.

#include <Arduino.h>
#include <Update.h>
#include <lvgl.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <esp_rom_crc.h>
#include "sd_update.h"
#include "display.h"
#include "pins.h"
#include "usb_link.h"

#define GP_FILE         SD_MOUNT "/gamepad.bin"
#define GP_DONE         SD_MOUNT "/gamepad.done"
#define GP_BAD          SD_MOUNT "/gamepad.bad"
#define GP_BLOCK        1024        // as FW_BLOCK in the sketch
#define GP_RESENDS      5           // a block at most this many times again in a row (FW_RESENDS in the sketch)
#define GP_MAX          0x1A0000   // app partition of the receiver (partitions_ota.csv)
#define GP_HELLO_MS     8000        // the receiver starts together with the display, it gets this long to answer
#define GP_ACK_MS       2500        // per block, less than the 3 s after which the receiver gives up (FD,0)
#define GP_END_MS       8000        // image check at the end
#define GP_RX_BLOCK     4096        // gpupdate: the PC sends blocks of this size, each acknowledged with "K <bytes>"
#define GP_RX_MS        3000        // gpupdate: no data this long = given up

#define DIM_PCT     8       // backlight while writing: the picture flickers turquoise meanwhile

#define SD_MOUNT    "/sdcard"
#define SD_FILE     SD_MOUNT "/firmware.bin"
#define SD_DONE     SD_MOUNT "/firmware.done"
#define SD_BAD      SD_MOUNT "/firmware.bad"

#define PIN_SD_CLK  43
#define PIN_SD_CMD  44
#define PIN_SD_D0   39

static lv_obj_t *scr_box, *scr_lbl, *scr_bar;

static void screen (const char *text, int pct)
{
    if(scr_box == NULL) {
        scr_box = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(scr_box);
        lv_obj_set_size(scr_box, LV_PCT(100), LV_PCT(100));
        // black with only the progress: writing the flash stalls the PSRAM frame buffer and the picture twitches
        lv_obj_set_style_bg_color(scr_box, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr_box, LV_OPA_COVER, 0);
        scr_lbl = lv_label_create(scr_box);
        lv_obj_set_style_text_font(scr_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(scr_lbl, lv_color_hex(0x808080), 0);
        lv_obj_set_style_text_align(scr_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(scr_lbl, LV_ALIGN_CENTER, 0, -50);
        scr_bar = lv_bar_create(scr_box);
        lv_obj_set_size(scr_bar, 600, 22);
        lv_obj_align(scr_bar, LV_ALIGN_CENTER, 0, 30);
        lv_bar_set_range(scr_bar, 0, 100);
    }
    lv_label_set_text(scr_lbl, text);
    if(pct < 0)
        lv_obj_add_flag(scr_bar, LV_OBJ_FLAG_HIDDEN);
    else
        lv_bar_set_value(scr_bar, pct, LV_ANIM_OFF);
    lv_refr_now(NULL);
}

static void screen_close (void)
{
    if(scr_box) {
        lv_obj_delete(scr_box);
        scr_box = NULL;
        lv_obj_invalidate(lv_screen_active());
    }
}

static void file_rename (const char *to, const char *from = SD_FILE)
{
    remove(to);
    rename(from, to);
}

// ------------------------------------------------------------------------ gamepad receiver

static void gp_send (const char *body)
{
    uint8_t sum = 0;
    for(const char *p = body; *p; p++)
        sum ^= (uint8_t)*p;
    Serial2.printf("\n$%s*%02X\n", body, sum);     // leading \n: ends a line cut short
}

static char gp_other[64];        // the last skipped line that was no gamepad packet, for the diagnosis

// the next line "$<prefix>...*hh" with a valid checksum within ms, body without '$' and checksum into out;
// other lines (the gamepad packets $GP) are skipped
static bool gp_wait (const char *prefix, char *out, size_t n, uint32_t ms)
{
    static char line[64];
    static size_t len = 0;
    uint32_t t0 = millis();

    while(millis() - t0 < ms) {
        if(!Serial2.available()) {
            delay(1);
            continue;
        }
        char c = Serial2.read();
        if(c == '$')
            len = 0;
        else if(c == '\n' || c == '\r') {
            line[len] = '\0';
            len = 0;
            char *star = strrchr(line, '*');
            if(!star || strncmp(line, prefix, strlen(prefix))) {
                if(*line && strncmp(line, "GP,", 3))
                    strlcpy(gp_other, line, sizeof(gp_other));
                continue;
            }
            *star = '\0';
            uint8_t sum = 0;
            for(char *p = line; *p; p++)
                sum ^= (uint8_t)*p;
            if(strtoul(star + 1, NULL, 16) == sum) {
                strlcpy(out, line, n);
                return true;
            }
        } else if(len < sizeof(line) - 1)
            line[len++] = c;
    }
    return false;
}

// After a block without acknowledge: what the receiver sends in the next 4 s, on the console. FD,0 = it gave
// up (bytes lost), $GP packets with a low sequence number = it restarted, nothing = it hangs.
static void gp_diagnose (long sent, size_t n)
{
    char line[96];
    size_t len = 0;
    int lines = 0, gp = 0, first_seq = -1;
    uint32_t t0 = millis();

    Serial.printf("gamepad update: no acknowledge for the block at %ld (%u bytes), last other line '%s', listening 4 s ...\n",
                  sent, (unsigned)n, gp_other);
    while(millis() - t0 < 4000) {
        if(!Serial2.available()) {
            delay(1);
            continue;
        }
        char c = Serial2.read();
        if(c == '\n' || c == '\r') {
            line[len] = '\0';
            if(len) {
                if(!strncmp(line, "$GP,", 4)) {
                    if(gp++ == 0)
                        first_seq = atoi(line + 4);
                } else if(lines++ < 8)
                    Serial.printf("  %5lu ms: %s\n", (unsigned long)(millis() - t0), line);
            }
            len = 0;
        } else if(len < sizeof(line) - 1)
            line[len++] = c >= ' ' && c < 127 ? c : '.';
    }
    Serial.printf("gamepad update: %d other lines, %d gamepad packets (first seq %d)%s\n", lines, gp, first_seq,
                  gp ? (first_seq >= 0 && first_seq < 100 ? " - the receiver restarted" : " - the receiver runs normally again")
                     : lines ? "" : " - the receiver is silent");
}

// The image to the receiver, NULL when done, else why not. running: the UART is already open (usb_link,
// held meanwhile) and stays open, else it is opened and closed here (at start, before usb_link_begin()).
static const char *gp_send_image (const uint8_t *img, long size, bool show, bool running)
{
    char body[32];
    uint32_t crc = esp_rom_crc32_le(0, img, size);

    if(running) {
        while(Serial2.available())
            Serial2.read();
    } else {
        Serial2.setRxBufferSize(1024);
        Serial2.begin(GAMEPAD_BAUD, SERIAL_8N1, PIN_GAMEPAD_RX, PIN_GAMEPAD_TX);
    }

    // the receiver may still be starting: ask again every second, by turns with GAMEPAD_BAUD and with the
    // 921600 of receivers before sketch 1.7 (the update to 1.7 itself comes from one of them)
    const char *why = "receiver does not answer (old firmware: flash it once over USB)";
    snprintf(body, sizeof(body), "FW,%ld,%08lx", size, (unsigned long)crc);
    for(uint32_t t0 = millis(), i = 0; millis() - t0 < GP_HELLO_MS; i++) {
        char ans[16];
        Serial2.updateBaudRate(GAMEPAD_BAUD != 921600 && (i & 1) ? 921600 : GAMEPAD_BAUD);
        gp_send(body);
        if(gp_wait("FA,", ans, sizeof(ans), 1000)) {
            why = ans[3] == '1' ? NULL : "refused by the receiver";
            break;
        }
    }

    int resends = 0, resends_all = 0;
    for(long sent = 0; why == NULL && sent < size; ) {
        size_t n = min((long)GP_BLOCK, size - sent);
        char ans[24];
        Serial2.write(img + sent, n);
        if(!gp_wait("FK,", ans, sizeof(ans), GP_ACK_MS)) {
            why = "no answer";
            gp_diagnose(sent, n);
            break;
        }
        long got = atol(ans + 3);
        if(got == sent && ++resends <= GP_RESENDS) {    // the receiver lost bytes: the block again
            resends_all++;
            Serial.printf("gamepad update: block at %ld sent again\n", sent);
            continue;
        }
        if(got != sent + (long)n) {
            why = got == sent ? "bytes lost again and again" : "out of step";
            break;
        }
        resends = 0;
        sent += n;
        if(sent * 20 / size != (sent - n) * 20 / size)     // every 5 % on the console
            Serial.printf("gamepad update: %ld of %ld bytes\n", sent, size);
        if(show) {
            static int shown = -1;
            int pct = sent * 100 / size;
            if(pct != shown) {
                char t[48];
                shown = pct;
                snprintf(t, sizeof(t), "Gamepad receiver\n%d %%", pct);
                screen(t, pct);
            }
        }
    }

    if(why == NULL) {
        char ans[48] = "";
        if(!gp_wait("FD,", ans, sizeof(ans), GP_END_MS))
            why = "no answer at the end";
        else if(ans[3] != '1')
            why = "image check failed";
        Serial.printf("gamepad update: receiver says '%s', %d blocks sent again\n", ans, resends_all);
    }

    if(running)
        Serial2.updateBaudRate(GAMEPAD_BAUD);
    else
        Serial2.end();      // usb_link_begin() opens it again
    return why;
}

// NULL when done, else why not
static const char *gp_flash (FILE *f, long size, bool show)
{
    if(size <= 0 || size > GP_MAX)
        return "file size";
    uint8_t *img = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if(img == NULL)
        return "no memory";
    const char *why = fread(img, 1, size, f) == (size_t)size ? gp_send_image(img, size, show, false) : "read error";
    heap_caps_free(img);
    return why;
}

void gp_update_console (const char *arg)
{
    long size = 0;
    unsigned long crc = 0;

    if(!GAMEPAD || sscanf(arg, "%ld %lx", &size, &crc) != 2 || size <= 0 || size > GP_MAX) {
        Serial.println(GAMEPAD ? "gpupdate failed: usage gpupdate <size> <crc32 hex>" : "gpupdate failed: no gamepad");
        return;
    }
    uint8_t *img = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if(img == NULL) {
        Serial.println("gpupdate failed: no memory");
        return;
    }

    // receive in blocks, each acknowledged: the console buffer (16 KB) never runs over
    Serial.printf("gpupdate ready %u\n", GP_RX_BLOCK);
    long got = 0;
    uint32_t last = millis();
    while(got < size) {
        int a = Serial.available();
        if(a <= 0) {
            if(millis() - last > GP_RX_MS)
                break;
            delay(1);
            continue;
        }
        long end = min((got / GP_RX_BLOCK + 1) * GP_RX_BLOCK, size);
        got += Serial.read(img + got, min((long)a, end - got));
        last = millis();
        if(got == end)
            Serial.printf("K %ld\n", got);
    }

    const char *why = NULL;
    if(got < size)
        why = "no data from the PC";
    else if(esp_rom_crc32_le(0, img, size) != crc)
        why = "checksum of the received file";
    else {
        Serial.printf("gpupdate: %ld bytes received, updating the gamepad receiver\n", size);
        usb_link_gamepad_hold(true);
        screen("Gamepad receiver\n0 %", 0);
        why = gp_send_image(img, size, true, true);
        screen(why ? "Gamepad receiver update failed" : "Gamepad receiver updated", -1);
        delay(why ? 3000 : 1500);
        screen_close();
        usb_link_gamepad_hold(false);
    }
    heap_caps_free(img);
    if(why)
        Serial.printf("gpupdate failed: %s\n", why);
    else
        Serial.println("gpupdate done, the receiver restarts");
}

static void gp_update_check (bool show)
{
    FILE *f = fopen(GP_FILE, "rb");
    if(f == NULL)
        return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    Serial.printf("SD card: gamepad.bin %ld bytes, updating the gamepad receiver\n", size);
    if(show)
        screen("Gamepad receiver\n0 %", 0);
    const char *why = gp_flash(f, size, show);
    fclose(f);
    file_rename(why ? GP_BAD : GP_DONE, GP_FILE);

    if(why == NULL) {
        Serial.println("Gamepad receiver updated, it restarts");
        if(show) {
            screen("Gamepad receiver updated", -1);
            delay(1500);
        }
    } else {
        Serial.printf("Gamepad receiver update failed: %s, file renamed to gamepad.bad\n", why);
        if(show) {
            char t[160];
            snprintf(t, sizeof(t), "Gamepad receiver update failed\n%s\ngamepad.bin renamed to gamepad.bad", why);
            screen(t, -1);
            delay(4000);
        }
    }
}

static bool flash (FILE *f, size_t size)
{
    const size_t chunk = 8192;
    size_t done = 0;
    int shown = -1;

    // from PSRAM: the WiFi chip link needs the internal memory afterwards
    uint8_t *buf = (uint8_t *)heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);
    if(buf == NULL || !Update.begin(size)) {
        Serial.printf("SD update: %s\n", buf ? Update.errorString() : "no memory");
        free(buf);
        return false;
    }

    display_backlight(DIM_PCT);
    while(done < size) {
        size_t n = fread(buf, 1, chunk, f);
        if(n == 0 || Update.write(buf, n) != n) {
            Serial.printf("SD update: write failed at %u bytes, %s\n", done, Update.errorString());
            Update.abort();
            free(buf);
            display_backlight(100);
            return false;
        }
        done += n;
        int pct = done * 100 / size;
        if(pct != shown) {
            shown = pct;
            char text[48];
            snprintf(text, sizeof(text), "%d %%", pct);
            screen(text, pct);
        }
    }
    free(buf);
    display_backlight(100);

    // checks the image (header, chip, checksum) and makes it the boot partition
    if(!Update.end(true)) {
        Serial.printf("SD update: %s\n", Update.errorString());
        return false;
    }
    return true;
}

void sd_update_check (bool show)
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 2;
    mount.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = 10000;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)PIN_SD_CLK;
    slot.cmd = (gpio_num_t)PIN_SD_CMD;
    slot.d0 = (gpio_num_t)PIN_SD_D0;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount, &card);
    if(err != ESP_OK) {
        Serial.printf("SD card: none (%s)\n", esp_err_to_name(err));   // the mount released the host again
        return;
    }

    bool ok = false, found = false;
    FILE *f = fopen(SD_FILE, "rb");
    if(f) {
        found = true;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        Serial.printf("SD card: firmware.bin %ld bytes, updating\n", size);
        if(show)
            screen("0 %", 0);
        ok = size > 0 && flash(f, (size_t)size);
        fclose(f);
        file_rename(ok ? SD_DONE : SD_BAD);
    } else
        Serial.println("SD card: no firmware.bin");

    if(GAMEPAD && !found)       // after a display update this comes at the next start
        gp_update_check(show);

    esp_vfs_fat_sdcard_unmount(SD_MOUNT, card);     // also releases the SDMMC host

    if(ok) {
        Serial.println("SD update done, restarting");
        if(show)
            screen("Update done, restarting", -1);
        delay(1000);
        ESP.restart();
    }

    if(found) {
        Serial.println("SD update failed, file renamed to firmware.bad");
        if(show) {
            screen("Update failed\nfirmware.bin renamed to firmware.bad\nstarting the old firmware", -1);
            delay(4000);
        }
    }

    if(scr_box) {
        lv_obj_delete(scr_box);
        scr_box = NULL;
    }
}

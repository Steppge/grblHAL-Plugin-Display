// Firmware update from the display's own microSD card at start: /firmware.bin (the app image from the
// PlatformIO build folder, not the factory image) is written to the other OTA partition, renamed to
// firmware.done and the display restarts. A broken or wrong file is renamed to firmware.bad.
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
#include "sd_update.h"
#include "display.h"

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

static void file_rename (const char *to)
{
    remove(to);
    rename(SD_FILE, to);
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

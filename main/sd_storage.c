// sd_storage.c — FATFS over SDSPI, shares the SPI2 bus with the EPD.
//
// The SDSPI driver inside esp-idf takes care of slowing the clock to the
// 400 kHz initialisation rate and re-negotiating up to the configured ceiling
// (20 MHz here).  Each SPI device — SD vs. EPD — has its own CS and its own
// clock setting; the bus driver swaps them transparently when one acquires
// the bus.
//
// On boards where the EPD is the only SPI peripheral that has been brought
// up at boot, we let the EPD driver own bus initialisation and skip it here.

#include "sd_storage.h"
#include "board_pins.h"

#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card = NULL;
static bool s_littlefs_mounted = false;
static const char *MOUNT_POINT = "/sdcard";
static const char *INTERNAL_MOUNT_POINT = "/littlefs";
static const char *INTERNAL_PARTITION   = "littlefs";

int sd_storage_mount(bool init_bus)
{
    if (init_bus) {
        spi_bus_config_t bus = {
            .mosi_io_num = EPD_PIN_MOSI,
            .miso_io_num = EPD_PIN_MISO,
            .sclk_io_num = EPD_PIN_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        esp_err_t err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
            return -1;
        }
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = EPD_SPI_HOST;
    host.max_freq_khz = SD_SPI_CLOCK_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = EPD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "initializing SD card on SPI, no hardware card-detect precheck");
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mc, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD init/mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return -1;
    }

    ESP_LOGI(TAG, "SD mounted at %s, %s %lluMB",
             MOUNT_POINT, s_card->cid.name,
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024ULL * 1024ULL));
    return 0;
}

int sd_storage_mount_internal(void)
{
    // joltwallet/littlefs.  Random writes are O(log n) instead of SPIFFS's
    // full-partition GC scan, so the upload latency stays predictable as the
    // store fills up.  format_if_mount_failed lets first boot (or anyone
    // upgrading from the old SPIFFS image) format the partition once.
    int64_t t0 = esp_timer_get_time();
    esp_vfs_littlefs_conf_t conf = {
        .base_path = INTERNAL_MOUNT_POINT,
        .partition_label = INTERNAL_PARTITION,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return -1;
    }
    s_littlefs_mounted = true;
    int64_t t1 = esp_timer_get_time();

    size_t total = 0, used = 0;
    if (esp_littlefs_info(conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s in %lld ms (%u/%u bytes used)",
                 INTERNAL_MOUNT_POINT, (long long)((t1 - t0) / 1000),
                 (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "LittleFS mounted at %s in %lld ms",
                 INTERNAL_MOUNT_POINT, (long long)((t1 - t0) / 1000));
    }
    return 0;
}

void sd_storage_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
    }
    if (s_littlefs_mounted) {
        esp_vfs_littlefs_unregister(INTERNAL_PARTITION);
        s_littlefs_mounted = false;
    }
}

static bool ends_with_ci(const char *s, const char *suf)
{
    size_t ls = strlen(s), lsu = strlen(suf);
    if (ls < lsu) return false;
    return strcasecmp(s + ls - lsu, suf) == 0;
}

bool sd_storage_find_first_image(char *out_path, int out_path_len)
{
    DIR *d = opendir(MOUNT_POINT);
    if (!d) {
        ESP_LOGE(TAG, "opendir %s failed", MOUNT_POINT);
        return false;
    }
    struct dirent *ent;
    bool found = false;
    while ((ent = readdir(d)) != NULL) {
        // Skip dotfiles (macOS leaves "._*" siblings on SD cards).
        if (ent->d_name[0] == '.') continue;
        const char *n = ent->d_name;
        if (ends_with_ci(n, ".jpg") || ends_with_ci(n, ".jpeg") ||
            ends_with_ci(n, ".bmp")) {
            snprintf(out_path, out_path_len, "%s/%s", MOUNT_POINT, n);
            ESP_LOGI(TAG, "found image: %s", out_path);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

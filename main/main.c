// main.c — PaperE6 photo viewer with on-device web UI.
//
// Boot order:
//   1. color_pipeline_init / palette_init.
//   2. Mount SD on shared SPI bus.
//   3. Bring up EPD as a second device on the same bus.
//   4. Init NVS (config_store) and create /sdcard/photos.
//   5. Start the EPD worker task (loop_display).
//   6. SoftAP up, DNS hijack on :53, HTTP server on :80.
//   7. main() exits and lets FreeRTOS run the worker + httpd forever.
//
// The display worker auto-advances every config_get_loop_interval_s() while
// the AP stays online for users to drop new photos in.

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "board_pins.h"
#include "palette.h"
#include "color_pipeline.h"
#include "epd_4in0e.h"
#include "sd_storage.h"
#include "photo_store.h"
#include "config_store.h"
#include "loop_display.h"
#include "wifi_ap.h"
#include "dns_hijack.h"
#include "http_server.h"
#include "pm1.h"

static const char *TAG = "main";

static void log_heap(const char *tag)
{
    ESP_LOGI(TAG, "[%s] heap free: internal=%u, SPIRAM=%u",
             tag,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void)
{
    ESP_LOGI(TAG, "PaperE6 photo viewer — boot");
    log_heap("boot");

    // Colour-pipeline LUT must exist before palette_init() consumes it.
    color_pipeline_init();
    palette_init();

    // NVS holds the loop-interval setting.
    config_store_init();

    // Enable EPD + SD rails through the M5PM1 PMIC.  Without this the SD
    // card stays unpowered and sdmmc_init_sd_if_cond returns 0x108.
    if (pm1_init() != 0) {
        ESP_LOGE(TAG, "M5PM1 init failed — abort");
        return;
    }

    // SD on the shared SPI bus. If no card is present, fall back to internal
    // SPIFFS so the web UI can still save a small number of photos.
    if (sd_storage_mount(true) != 0) {
        ESP_LOGW(TAG, "SD mount failed — using internal SPIFFS fallback");
        if (sd_storage_mount_internal() != 0 ||
            photo_store_set_mount_point("/spiffs") != 0) {
            ESP_LOGE(TAG, "internal storage fallback failed — abort");
            return;
        }
    } else {
        photo_store_set_mount_point("/sdcard");
    }
    if (photo_store_init() != 0) {
        ESP_LOGE(TAG, "photo store init failed — abort");
        sd_storage_unmount();
        return;
    }

    // EPD attaches as a second device on the same bus.
    if (epd_init(true) != 0) {
        ESP_LOGE(TAG, "EPD init failed — abort");
        sd_storage_unmount();
        return;
    }

    // Display worker — serialises all panel refreshes.
    if (loop_display_start() != 0) {
        ESP_LOGE(TAG, "loop_display_start failed");
        return;
    }

    // Bring up the captive-portal stack.
    wifi_ap_start();
    dns_hijack_start(wifi_ap_get_ip());
    http_server_start();

    log_heap("after-init");
    ESP_LOGI(TAG, "ready — connect to SSID '" WIFI_AP_SSID "' (open)");

    // The worker handles its own timing; main() can exit.
}

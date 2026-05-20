// main.c — PaperColor photo frame with on-device web UI.
//
// Boot order:
//   1. color_pipeline_init / palette_init.
//   2. Mount SD on shared SPI bus.
//   3. Init NVS (config_store) and create /sdcard/photos.
//   4. Start the EPD worker task (loop_display).
//   5. SoftAP up, DNS hijack on :53, HTTP server on :80.
//   6. Start power manager for buttons, LEDs, and deep sleep.
//   7. main() exits and lets FreeRTOS run the worker + httpd forever.
//
// The display worker auto-advances every config_get_loop_interval_s() while
// awake. In low-power mode the ESP also uses that interval as its timer wake.

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"

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
#include "power_manager.h"

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
    ESP_LOGI(TAG, "PaperColor photo frame — boot");
    log_heap("boot");

    // Colour-pipeline LUT must exist before palette_init() consumes it.
    color_pipeline_init();
    palette_init();

    // NVS holds the loop-interval setting.
    config_store_init();

    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "wake cause=%d", (int)wake);

    // Enable the SD rail through the M5PM1 PMIC.  The EPD rail intentionally
    // stays off here and is powered only inside the display refresh path.
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
    if (strcmp(photo_store_dir(), "/spiffs") == 0) {
        pm1_set_sd_power(false);
    }
    if (photo_store_init() != 0) {
        ESP_LOGE(TAG, "photo store init failed — abort");
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

    if (power_manager_start() != 0) {
        ESP_LOGE(TAG, "power_manager_start failed");
        return;
    }

    if (wake == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "timer wake: queueing scheduled refresh");
        (void)loop_display_request_next();
    }

    log_heap("after-init");
    ESP_LOGI(TAG, "ready — connect to SSID '%s' (%s)",
             config_get_wifi_ap_ssid(),
             config_get_wifi_ap_password()[0] ? "password set" : "open");

    // The worker handles its own timing; main() can exit.
}

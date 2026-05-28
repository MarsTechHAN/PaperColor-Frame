// main.c — PaperColor photo frame with on-device web UI.
//
// Boot order:
//   1. color_pipeline_init / palette_init.
//   2. Mount SD on shared SPI bus.
//   3. Init NVS (config_store) and create /sdcard/photos.
//   4. Start the EPD worker task (loop_display).
//   5. Start SoftAP/DNS/HTTP only for reset boots and top-button wakeups.
//   6. Queue one-shot refreshes for timer or side-button wakeups.
//   7. Start power manager for buttons, optional LEDs, and deep sleep.
//   8. main() exits and lets FreeRTOS run the worker + httpd forever.
//
// The display worker auto-advances every config_get_loop_interval_s() while
// awake. In low-power mode the ESP also uses that interval as its timer wake.

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
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
#if CONFIG_EPD_PROBE
#include "epd_probe.h"
#endif

static const char *TAG = "main";

static void log_heap(const char *tag)
{
    ESP_LOGI(TAG, "[%s] heap free: internal=%u, SPIRAM=%u",
             tag,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static bool wake_mask_has(uint64_t mask, int gpio)
{
    return (mask & (1ULL << gpio)) != 0;
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
    uint64_t ext1_mask = esp_sleep_get_ext1_wakeup_status();
    bool wake_by_top = (wake == ESP_SLEEP_WAKEUP_EXT0) ||
                       (wake == ESP_SLEEP_WAKEUP_EXT1 &&
                        wake_mask_has(ext1_mask, BOARD_BTN_TOP_GPIO));
    bool wake_by_side = (wake == ESP_SLEEP_WAKEUP_EXT1 &&
                         (wake_mask_has(ext1_mask, BOARD_BTN_UP_GPIO) ||
                          wake_mask_has(ext1_mask, BOARD_BTN_DOWN_GPIO)));
    bool wake_by_timer = (wake == ESP_SLEEP_WAKEUP_TIMER);
    bool wifi_enabled = (wake == ESP_SLEEP_WAKEUP_UNDEFINED) || wake_by_top;
    ESP_LOGI(TAG, "wake cause=%d ext1=0x%llx wifi=%d",
             (int)wake, (unsigned long long)ext1_mask, wifi_enabled);

    // Enable the SD rail through the M5PM1 PMIC.  The EPD rail intentionally
    // stays off here and is powered only inside the display refresh path.
    if (pm1_init() != 0) {
        ESP_LOGE(TAG, "M5PM1 init failed — abort");
        return;
    }

#if CONFIG_EPD_PROBE
    // Reverse-engineering probe: replaces the normal stack. Self-powers rails
    // and inits the bus/panel internally, so it must run before SD mount.
    epd_probe_run();
    return;
#endif

    // SD on the shared SPI bus. If no card is present, fall back to internal
    // LittleFS so the web UI can still save photos to flash.
    if (sd_storage_mount(true) != 0) {
        ESP_LOGW(TAG, "SD mount failed — using internal LittleFS fallback");
        if (sd_storage_mount_internal() != 0 ||
            photo_store_set_mount_point("/littlefs") != 0) {
            ESP_LOGE(TAG, "internal storage fallback failed — abort");
            return;
        }
    } else {
        photo_store_set_mount_point("/sdcard");
    }
    // The SD and EPD rails share one SPI bus, so they must stay at the same
    // power state (see pm1.c). Even in the LittleFS fallback we leave the SD
    // rail powered: cutting it while the EPD rail comes up for a refresh would
    // put the shared bus in a mixed state and corrupt the panel write.
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

    // Bring up the captive-portal stack only when the user explicitly wants
    // Wi-Fi (reset boot or top-button wake). Timer and side-button wakeups
    // are one-shot refreshes and stay radio-off.
    if (wifi_enabled) {
        wifi_ap_start();
        if (wifi_ap_has_ap()) {
            dns_hijack_start(wifi_ap_get_ip());
        }
        http_server_start();
    }

    bool queued_refresh = false;
    if (wake_by_timer) {
        ESP_LOGI(TAG, "timer wake: queueing scheduled refresh");
        queued_refresh = (loop_display_request_next() == 0);
    } else if (wake_by_side) {
        ESP_LOGI(TAG, "side-button wake: queueing refresh");
        queued_refresh = (loop_display_request_next() == 0);
    } else if (wifi_enabled) {
        // Fresh power-on / reset: if the library is empty, push the welcome
        // card immediately so the user sees the per-device Wi-Fi credentials
        // without having to press anything.
        photo_meta_t one = {0};
        if (photo_store_list(&one, 1) == 0) {
            ESP_LOGI(TAG, "empty library: queueing welcome refresh");
            queued_refresh = (loop_display_request_next() == 0);
        }
    }

    power_manager_config_t pm_cfg = {
        .wifi_enabled = wifi_enabled,
        .sleep_when_display_idle = !wifi_enabled && !wake_by_side,
        .button_refresh_wake = !wifi_enabled && wake_by_side,
    };
    if ((wake_by_timer || wake_by_side) && !queued_refresh) {
        ESP_LOGW(TAG, "wake refresh queue failed; will sleep when display is idle");
        pm_cfg.sleep_when_display_idle = true;
        pm_cfg.button_refresh_wake = false;
    }

    if (power_manager_start(&pm_cfg) != 0) {
        ESP_LOGE(TAG, "power_manager_start failed");
        return;
    }

    log_heap("after-init");
    if (wifi_enabled) {
        if (wifi_ap_has_ap()) {
            ESP_LOGI(TAG, "ready — AP SSID '%s' (%s)",
                     config_get_wifi_ap_ssid(),
                     config_get_wifi_ap_password()[0] ? "password set" : "open");
        }
        if (wifi_ap_has_sta()) {
            ESP_LOGI(TAG, "ready — STA mode connecting; use http://papercolor.local/ after it gets an IP");
        }
    } else {
        ESP_LOGI(TAG, "ready — Wi-Fi off for one-shot low-power refresh");
    }

    // The worker handles its own timing; main() can exit.
}

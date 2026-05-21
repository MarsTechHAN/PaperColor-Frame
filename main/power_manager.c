#include "power_manager.h"

#include "board_pins.h"
#include "config_store.h"
#include "loop_display.h"
#include "pm1.h"
#include "status_led.h"
#include "wifi_ap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

#define BUTTON_REFRESH_SLEEP_S 3
#define STATUS_LED_BRIGHTNESS  24
// Top-button hold duration that forces the welcome card on-screen, regardless
// of whether the photo library is empty. Deliberately long so it can't fire
// from accidental presses; this is the recovery path for "I forgot the AP
// password and can't get to the web UI".
#define TOP_FORCE_WELCOME_HOLD_US (15LL * 1000000LL)

typedef struct {
    gpio_num_t gpio;
    bool raw_pressed;
    bool stable_pressed;
    int64_t raw_changed_us;
} button_state_t;

static TaskHandle_t s_task = NULL;
static volatile bool s_post_refresh_sleep_armed = false;
static volatile bool s_sleep_when_display_idle = false;
static volatile int64_t s_last_activity_us = 0;
static bool s_sleeping = false;
static bool s_wifi_enabled = false;

static uint64_t button_wakeup_mask(void)
{
    return (1ULL << BOARD_BTN_TOP_GPIO) |
           (1ULL << BOARD_BTN_UP_GPIO) |
           (1ULL << BOARD_BTN_DOWN_GPIO);
}

static void configure_buttons(void)
{
    const uint64_t mask = (1ULL << BOARD_BTN_TOP_GPIO) |
                          (1ULL << BOARD_BTN_UP_GPIO) |
                          (1ULL << BOARD_BTN_DOWN_GPIO);
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static bool poll_button_press(button_state_t *b, int64_t now_us)
{
    bool raw = gpio_get_level(b->gpio) == 0;
    if (raw != b->raw_pressed) {
        b->raw_pressed = raw;
        b->raw_changed_us = now_us;
    }
    if (now_us - b->raw_changed_us < 40 * 1000) {
        return false;
    }
    if (raw != b->stable_pressed) {
        b->stable_pressed = raw;
        return raw;
    }
    return false;
}

static button_state_t init_button_state(gpio_num_t gpio, int64_t now_us)
{
    bool raw = gpio_get_level(gpio) == 0;
    return (button_state_t) {
        .gpio = gpio,
        .raw_pressed = raw,
        .stable_pressed = raw,
        .raw_changed_us = now_us,
    };
}

static void enter_deep_sleep(const char *reason)
{
    if (s_sleeping) return;
    s_sleeping = true;

    ESP_LOGI(TAG, "entering deep sleep: %s", reason);
    status_led_off();

    // Never cut power while the electrophoretic panel is actively refreshing.
    loop_display_wait_idle();
    status_led_off();

    if (s_wifi_enabled) {
        (void)esp_wifi_stop();
    }
    pm1_prepare_deep_sleep();

    configure_buttons();
    const gpio_num_t wake_gpios[] = {
        BOARD_BTN_TOP_GPIO,
        BOARD_BTN_UP_GPIO,
        BOARD_BTN_DOWN_GPIO,
    };
    for (size_t i = 0; i < sizeof(wake_gpios) / sizeof(wake_gpios[0]); i++) {
        (void)rtc_gpio_init(wake_gpios[i]);
        (void)rtc_gpio_set_direction(wake_gpios[i], RTC_GPIO_MODE_INPUT_ONLY);
        (void)rtc_gpio_pullup_en(wake_gpios[i]);
        (void)rtc_gpio_pulldown_dis(wake_gpios[i]);
    }
    esp_err_t e = esp_sleep_enable_ext1_wakeup_io(button_wakeup_mask(),
                                                  ESP_EXT1_WAKEUP_ANY_LOW);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "button GPIO wake failed: %s", esp_err_to_name(e));
    }

    uint32_t refresh_s = config_get_loop_interval_s();
    if (refresh_s > 0) {
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)refresh_s * 1000000ULL));
    }

    ESP_LOGI(TAG, "wake sources: buttons=G%d/G%d/G%d low, timer=%us",
             BOARD_BTN_TOP_GPIO, BOARD_BTN_UP_GPIO, BOARD_BTN_DOWN_GPIO,
             (unsigned)refresh_s);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

static void drive_status_led(int station_count, int64_t now_us)
{
    static int station_last = -1;
    static int blink_last = -1;
    static uint8_t brightness_last = 0xFF;
    static int64_t last_update_us = 0;

    uint8_t brightness = STATUS_LED_BRIGHTNESS;
    int blink = station_count > 0 ? (int)((now_us / 450000) & 1) : 1;
    bool periodic = now_us - last_update_us > 5 * 1000 * 1000LL;
    if (!periodic &&
        station_count == station_last &&
        blink == blink_last &&
        brightness == brightness_last) {
        return;
    }

    station_last = station_count;
    blink_last = blink;
    brightness_last = brightness;
    last_update_us = now_us;

    if (brightness == 0) {
        status_led_off();
    } else if (station_count > 0) {
        if (blink) {
            (void)status_led_show(0, 80, 255, brightness);
        } else {
            status_led_off();
        }
    } else {
        (void)status_led_show(0, 180, 30, brightness);
    }
}

static void manager_task(void *arg)
{
    (void)arg;
    configure_buttons();
    if (s_wifi_enabled) {
        (void)status_led_init();
    }

    int64_t now = esp_timer_get_time();
    s_last_activity_us = now;
    int64_t no_client_since_us = now;
    int64_t display_idle_since_us = 0;

    button_state_t top = init_button_state(BOARD_BTN_TOP_GPIO, now);
    button_state_t up = init_button_state(BOARD_BTN_UP_GPIO, now);
    button_state_t down = init_button_state(BOARD_BTN_DOWN_GPIO, now);

    // 15-second top-button hold → force welcome card. Latched so it fires
    // exactly once per hold; the latch resets when the button is released.
    int64_t top_press_start_us = top.stable_pressed ? now : 0;
    bool    top_long_press_fired = false;

    for (;;) {
        now = esp_timer_get_time();

        bool top_was_stable_pressed = top.stable_pressed;
        if (poll_button_press(&top, now)) {
            ESP_LOGI(TAG, "top button pressed");
            s_last_activity_us = now;
        }
        // Track stable_pressed edges so the long-press timer is debounced and
        // doesn't restart on raw GPIO noise.
        if (top.stable_pressed && !top_was_stable_pressed) {
            top_press_start_us = now;
            top_long_press_fired = false;
        } else if (!top.stable_pressed && top_was_stable_pressed) {
            top_press_start_us = 0;
            top_long_press_fired = false;
        }
        if (top.stable_pressed && !top_long_press_fired &&
            top_press_start_us != 0 &&
            now - top_press_start_us >= TOP_FORCE_WELCOME_HOLD_US) {
            ESP_LOGI(TAG, "top button held 15s — forcing welcome card");
            if (loop_display_request_welcome() != 0) {
                ESP_LOGW(TAG, "display queue full; welcome request dropped");
            }
            top_long_press_fired = true;
            s_last_activity_us = now;
        }
        bool up_pressed = poll_button_press(&up, now);
        bool down_pressed = poll_button_press(&down, now);
        if (up_pressed || down_pressed) {
            ESP_LOGI(TAG, "side button refresh");
            s_last_activity_us = now;
            display_idle_since_us = 0;
            if (loop_display_request_next() == 0) {
                // With Wi-Fi off, a physical refresh is a quick one-shot:
                // refresh, wait 3 seconds for another press, then sleep.
                if (!s_wifi_enabled) {
                    s_post_refresh_sleep_armed = true;
                    s_sleep_when_display_idle = false;
                }
            } else {
                ESP_LOGW(TAG, "display queue full; refresh button ignored");
            }
        }

        int station_count = s_wifi_enabled ? wifi_ap_station_count() : 0;
        if (s_wifi_enabled) {
            drive_status_led(station_count, now);
        }

        if (station_count > 0) {
            no_client_since_us = now;
            display_idle_since_us = 0;
            s_post_refresh_sleep_armed = false;
            s_sleep_when_display_idle = false;
        } else {
            bool display_idle = loop_display_is_idle();
            if (!display_idle) {
                display_idle_since_us = 0;
            } else if (display_idle_since_us == 0) {
                display_idle_since_us = now;
            }

            if (s_sleep_when_display_idle && display_idle) {
                enter_deep_sleep("no Wi-Fi client after refresh");
            } else if (s_post_refresh_sleep_armed && display_idle_since_us != 0) {
                int64_t idle_base = display_idle_since_us > s_last_activity_us
                                        ? display_idle_since_us : s_last_activity_us;
                if (now - idle_base >= (int64_t)BUTTON_REFRESH_SLEEP_S * 1000000LL) {
                    enter_deep_sleep("post-refresh idle");
                }
            } else if (s_wifi_enabled) {
                if (now - no_client_since_us >=
                    (int64_t)config_get_wifi_idle_sleep_s() * 1000000LL) {
                    if (display_idle) {
                        enter_deep_sleep("no Wi-Fi client");
                    } else {
                        s_sleep_when_display_idle = true;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

int power_manager_start(const power_manager_config_t *config)
{
    if (s_task) return 0;
    s_wifi_enabled = config ? config->wifi_enabled : false;
    s_sleep_when_display_idle = config ? config->sleep_when_display_idle : false;
    s_post_refresh_sleep_armed = config ? config->button_refresh_wake : false;
    ESP_LOGI(TAG, "power policy: wifi=%d sleep_when_idle=%d button_refresh_wake=%d",
             s_wifi_enabled, s_sleep_when_display_idle, s_post_refresh_sleep_armed);
    BaseType_t ok = xTaskCreate(manager_task, "power_mgr", 4096, NULL,
                                tskIDLE_PRIORITY + 4, &s_task);
    return ok == pdPASS ? 0 : -1;
}

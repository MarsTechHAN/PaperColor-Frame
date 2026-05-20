#include "power_manager.h"

#include "board_pins.h"
#include "config_store.h"
#include "loop_display.h"
#include "pm1.h"
#include "status_led.h"
#include "wifi_ap.h"

#include <stdbool.h>
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
static bool s_button_wake_session = false;

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

static void enter_deep_sleep(const char *reason)
{
    if (s_sleeping) return;
    s_sleeping = true;

    ESP_LOGI(TAG, "entering deep sleep: %s", reason);
    status_led_off();

    // Never cut power while the electrophoretic panel is actively refreshing.
    loop_display_wait_idle();
    status_led_off();

    (void)esp_wifi_stop();
    pm1_prepare_deep_sleep();

    configure_buttons();
    (void)rtc_gpio_init((gpio_num_t)BOARD_BTN_TOP_GPIO);
    (void)rtc_gpio_set_direction((gpio_num_t)BOARD_BTN_TOP_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    (void)rtc_gpio_pullup_en((gpio_num_t)BOARD_BTN_TOP_GPIO);
    (void)rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BTN_TOP_GPIO);
    esp_err_t e = esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_BTN_TOP_GPIO, 0);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "top-button GPIO wake failed: %s", esp_err_to_name(e));
    }

    uint32_t refresh_s = config_get_loop_interval_s();
    if (refresh_s > 0) {
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)refresh_s * 1000000ULL));
    }

    ESP_LOGI(TAG, "wake sources: top=G%d low, timer=%us",
             BOARD_BTN_TOP_GPIO, (unsigned)refresh_s);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

static void drive_status_led(int station_count, int64_t now_us)
{
    static int station_last = -1;
    static int blink_last = -1;
    static uint8_t brightness_last = 0xFF;
    static int64_t last_update_us = 0;

    uint8_t brightness = config_get_status_led_brightness();
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
    (void)status_led_init();

    int64_t now = esp_timer_get_time();
    s_last_activity_us = now;
    int64_t no_client_since_us = now;
    int64_t display_idle_since_us = 0;

    button_state_t top = {.gpio = BOARD_BTN_TOP_GPIO, .raw_changed_us = now};
    button_state_t up = {.gpio = BOARD_BTN_UP_GPIO, .raw_changed_us = now};
    button_state_t down = {.gpio = BOARD_BTN_DOWN_GPIO, .raw_changed_us = now};

    for (;;) {
        now = esp_timer_get_time();

        if (poll_button_press(&top, now)) {
            ESP_LOGI(TAG, "top button pressed");
            s_last_activity_us = now;
        }
        bool up_pressed = poll_button_press(&up, now);
        bool down_pressed = poll_button_press(&down, now);
        if (up_pressed || down_pressed) {
            ESP_LOGI(TAG, "side button refresh");
            s_last_activity_us = now;
            display_idle_since_us = 0;
            if (loop_display_request_next() == 0) {
                // The short post-refresh sleep only applies to sessions that
                // were woken by the top button. Timer/normal wake keeps the
                // global no-client sleep deadline anchored at wake time.
                if (s_button_wake_session) {
                    s_post_refresh_sleep_armed = true;
                }
            } else {
                ESP_LOGW(TAG, "display queue full; refresh button ignored");
            }
        }

        int station_count = wifi_ap_station_count();
        drive_status_led(station_count, now);

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
                uint32_t delay_s = config_get_button_sleep_s();
                if (now - idle_base >= (int64_t)delay_s * 1000000LL) {
                    enter_deep_sleep("post-refresh idle");
                }
            } else {
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

int power_manager_start(void)
{
    if (s_task) return 0;
    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    s_button_wake_session = (wake == ESP_SLEEP_WAKEUP_EXT0);
    ESP_LOGI(TAG, "button-wake session=%d", s_button_wake_session);
    BaseType_t ok = xTaskCreate(manager_task, "power_mgr", 4096, NULL,
                                tskIDLE_PRIORITY + 4, &s_task);
    return ok == pdPASS ? 0 : -1;
}

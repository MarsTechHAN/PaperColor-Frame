#include "status_led.h"

#include "board_pins.h"
#include "pm1.h"

#include <stdbool.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static bool s_enabled = false;

int status_led_init(void)
{
    if (s_chan && s_encoder) return 0;

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = BOARD_NEOPIXEL_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 0.1 us ticks.
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_chan), TAG,
                        "rmt_new_tx_channel");

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 4,   // WS2812: T0H ~= 0.4 us
            .level1 = 0,
            .duration1 = 8,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 8,   // WS2812: T1H ~= 0.8 us
            .level1 = 0,
            .duration1 = 4,
        },
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &s_encoder), TAG,
                        "rmt_new_bytes_encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "rmt_enable");
    s_enabled = true;
    status_led_off();
    return 0;
}

static uint8_t scale8(uint8_t v, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)v * brightness) / 255u);
}

int status_led_show(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    if (status_led_init() != 0) return -1;

    uint8_t sr = scale8(r, brightness);
    uint8_t sg = scale8(g, brightness);
    uint8_t sb = scale8(b, brightness);
    bool any = (sr | sg | sb) != 0;

    if (any) {
        if (pm1_set_rgb_power(true) != 0) {
            ESP_LOGW(TAG, "RGB_EN on failed");
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    uint8_t grb[BOARD_NEOPIXEL_COUNT * 3];
    for (int i = 0; i < BOARD_NEOPIXEL_COUNT; i++) {
        grb[i * 3 + 0] = sg;
        grb[i * 3 + 1] = sr;
        grb[i * 3 + 2] = sb;
    }
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_chan, s_encoder, grb, sizeof grb, &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_chan, pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt LED transmit failed: %s", esp_err_to_name(err));
        return -1;
    }

    if (!any) {
        // Latch reset is >50 us; wait a little longer before removing power.
        vTaskDelay(pdMS_TO_TICKS(2));
        (void)pm1_set_rgb_power(false);
    }
    return 0;
}

void status_led_off(void)
{
    if (s_chan && s_encoder && s_enabled) {
        (void)status_led_show(0, 0, 0, 0);
    } else {
        (void)pm1_set_rgb_power(false);
    }
}

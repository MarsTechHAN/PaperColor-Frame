// epd_4in0e.c — Waveshare 4-inch-E (Spectra 6) panel driver, pure ESP-IDF.
//
// Translates the existing Arduino reference (EPD_4in0e.cpp) into IDF's
// spi_master API.  Differences worth highlighting:
//
//   * SPI bus is configured once and shared with the SD card.  Each device
//     (EPD, SD) owns its own spi_device_handle_t with its own CS.
//
//   * Commands and data are issued with explicit DC line transitions.
//     We use the per-transaction `user` callback to flip DC just before
//     transmit, which is the IDF idiomatic way.
//
//   * Large data writes (the 120 KB framebuffer) go through one
//     polling transaction broken into ≤4 KB chunks.  DMA capable.
//
//   * BUSY is active low ("LOW = busy" — same polarity as the panel).

#include "epd_4in0e.h"
#include "board_pins.h"
#include "palette.h"

#include <stdbool.h>
#include <string.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "epd";
static spi_device_handle_t s_spi = NULL;

static void IRAM_ATTR pre_transfer_cb(spi_transaction_t *t)
{
    int dc = (int)(intptr_t)t->user;   // 0 = command, 1 = data
    gpio_set_level(EPD_PIN_DC, dc);
}

// ----------------------------------------------------------------- low level

static void epd_send_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void *)0,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void epd_send_data(const uint8_t *buf, size_t len)
{
    if (len == 0) return;
    spi_transaction_t t = {
        .length = (size_t)len * 8u,
        .tx_buffer = buf,
        .user = (void *)1,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void epd_send_data1(uint8_t v)
{
    epd_send_data(&v, 1);
}

// Returns 0 once BUSY released, -1 if it stayed low past the timeout.
static int epd_wait_busy_high(void)
{
    // The panel datasheet pulls BUSY low while it is internally working.
    // We loop with a yield so the IDLE task doesn't starve the watchdog.
    //
    // Hard timeout: a full E6 refresh holds BUSY low ~16 s, so 30 s is well
    // past any legitimate operation. Without this cap, a marginal wake-from-
    // sleep or a missed BUSY edge spins this loop forever and wedges the EPD
    // worker task permanently — the panel never updates again until a power
    // cycle (observed as a refresh that logs "displaying"/"bin read" but never
    // "epd_disp"). On timeout we now return -1 so the caller can bail out of
    // the rest of the command sequence and trigger a power-cycle recovery,
    // instead of plowing on into more 30 s stalls on a dead controller.
    const int timeout_ms = 30000;
    int waited_ms = 0;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
        if (waited_ms >= timeout_ms) {
            ESP_LOGE(TAG, "BUSY stuck low for %d ms — aborting wait", waited_ms);
            return -1;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return 0;
}

static void epd_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// ------------------------------------------------------------------ refresh

// Returns 0 on a completed refresh, -1 if BUSY hung at any step (POWER_ON,
// DISPLAY_REFRESH, or POWER_OFF) — a wedged controller the caller must recover.
static int epd_turn_on(void)
{
    epd_send_cmd(0x04);     // POWER_ON
    if (epd_wait_busy_high() != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(200));

    // Second setting (per panel reference code).
    epd_send_cmd(0x06);
    epd_send_data1(0x6F);
    epd_send_data1(0x1F);
    epd_send_data1(0x17);
    epd_send_data1(0x27);
    vTaskDelay(pdMS_TO_TICKS(200));

    epd_send_cmd(0x12);     // DISPLAY_REFRESH
    epd_send_data1(0x00);
    if (epd_wait_busy_high() != 0) return -1;

    epd_send_cmd(0x02);     // POWER_OFF
    epd_send_data1(0x00);
    if (epd_wait_busy_high() != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(200));
    return 0;
}

// ------------------------------------------------------------------ public

int epd_init(bool bus_already_inited)
{
    // GPIOs
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in_cfg);

    if (!bus_already_inited && !s_spi) {
        spi_bus_config_t bus = {
            .mosi_io_num = EPD_PIN_MOSI,
            .miso_io_num = EPD_PIN_MISO,
            .sclk_io_num = EPD_PIN_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            // Big enough for a row of the framebuffer in one shot.
            .max_transfer_sz = 4096,
        };
        esp_err_t bus_err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (bus_err != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(bus_err, TAG, "spi_bus_initialize");
        }
    }

    if (!s_spi) {
        spi_device_interface_config_t dev = {
            .clock_speed_hz = EPD_SPI_CLOCK_HZ,
            .mode = 0,
            .spics_io_num = EPD_PIN_CS,
            .queue_size = 1,
            .pre_cb = pre_transfer_cb,
            .flags = SPI_DEVICE_HALFDUPLEX,
        };
        ESP_RETURN_ON_ERROR(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi),
                            TAG, "spi_bus_add_device");
    }

    epd_reset();
    if (epd_wait_busy_high() != 0) {
        // Controller didn't come up after reset (classic wedged-after-deep-
        // sleep symptom). Report it so panel_power_on / display_fb can trigger
        // a hard power-cycle recovery instead of sending init bytes into a dead
        // controller and stalling another 30 s on the final wait.
        ESP_LOGE(TAG, "epd_init: BUSY low after reset — controller not responding");
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(30));

    // Initialisation sequence — taken verbatim from EPD_4in0e.cpp.
    epd_send_cmd(0xAA); { uint8_t d[] = {0x49,0x55,0x20,0x08,0x09,0x18}; epd_send_data(d, sizeof d); }
    epd_send_cmd(0x01); epd_send_data1(0x3F);
    epd_send_cmd(0x00); { uint8_t d[] = {0x5F,0x69}; epd_send_data(d, sizeof d); }
    epd_send_cmd(0x05); { uint8_t d[] = {0x40,0x1F,0x1F,0x2C}; epd_send_data(d, sizeof d); }
    epd_send_cmd(0x08); { uint8_t d[] = {0x6F,0x1F,0x1F,0x22}; epd_send_data(d, sizeof d); }
    epd_send_cmd(0x06); { uint8_t d[] = {0x6F,0x1F,0x17,0x17}; epd_send_data(d, sizeof d); }
    epd_send_cmd(0x03); { uint8_t d[] = {0x00,0x54,0x00,0x44}; epd_send_data(d, sizeof d); }
    epd_send_cmd(0x60); { uint8_t d[] = {0x02,0x00};          epd_send_data(d, sizeof d); }
    epd_send_cmd(0x30); epd_send_data1(0x08);
    epd_send_cmd(0x50); epd_send_data1(0x3F);
    epd_send_cmd(0x61); { uint8_t d[] = {0x01,0x90,0x02,0x58}; epd_send_data(d, sizeof d); }
    epd_send_cmd(0xE3); epd_send_data1(0x2F);
    epd_send_cmd(0x84); epd_send_data1(0x01);

    if (epd_wait_busy_high() != 0) {
        ESP_LOGE(TAG, "epd_init: BUSY low after init sequence");
        return -1;
    }
    return 0;
}

void epd_clear(uint8_t ink_code)
{
    const int W2 = EPD_4IN0E_WIDTH / 2;
    const int H  = EPD_4IN0E_HEIGHT;

    epd_send_cmd(0x10);
    uint8_t fill[256];
    memset(fill, (ink_code << 4) | (ink_code & 0x0F), sizeof fill);
    for (int j = 0; j < H; j++) {
        int remaining = W2;
        while (remaining > 0) {
            int n = remaining > (int)sizeof(fill) ? (int)sizeof(fill) : remaining;
            epd_send_data(fill, n);
            remaining -= n;
        }
    }
    (void)epd_turn_on();
}

int epd_display(const uint8_t *fb)
{
    const size_t total = (size_t)(EPD_4IN0E_WIDTH / 2) * EPD_4IN0E_HEIGHT;
    epd_send_cmd(0x10);

    // Send in 4 KB chunks — matches the bus DMA limit configured at init.
    const size_t CHUNK = 4096;
    size_t sent = 0;
    while (sent < total) {
        size_t n = (total - sent) > CHUNK ? CHUNK : (total - sent);
        epd_send_data(fb + sent, n);
        sent += n;
    }
    return epd_turn_on();
}

void epd_sleep(void)
{
    epd_send_cmd(0x07);
    epd_send_data1(0xA5);
}

void epd_prepare_power_off(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_RST, 0);
}

struct spi_device_t *epd_spi_handle(void)
{
    return s_spi;
}

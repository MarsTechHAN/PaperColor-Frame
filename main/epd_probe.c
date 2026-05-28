// epd_probe.c — Phase 1 read-only controller probe. Active only when
// CONFIG_EPD_PROBE=y; otherwise this file compiles to nothing.
#include "sdkconfig.h"

#if CONFIG_EPD_PROBE

#include "epd_probe.h"
#include "board_pins.h"
#include "epd_4in0e.h"
#include "pm1.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"         // esp_rom_delay_us

static const char *TAG = "epd_probe";

// Candidate read commands. 0x71 (Get Status) is the strongest "is the read
// path alive?" test on the UC81xx family; 0x20-0x25 are the LUT register
// addresses we hope to read back. These are hypotheses — the dump decides.
static const uint8_t k_cmds[]    = { 0x71, 0x70, 0x2E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 };
// Short reads for status-like commands, longer for LUT-like commands.
static size_t read_len_for(uint8_t cmd) { return (cmd == 0x71 || cmd == 0x70) ? 4 : 64; }

static void hexdump(const char *via, uint8_t cmd, const uint8_t *buf, size_t n)
{
    printf("PROBE %s cmd=0x%02x [%u]:", via, cmd, (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        if ((i % 16) == 0) printf("\n  %04x: ", (unsigned)i);
        printf("%02x ", buf[i]);
    }
    printf("\n");
}

// "Alive" heuristic: not all identical, or a non-rail constant. All-0x00 or
// all-0xFF means the line was almost certainly floating/undriven.
static bool looks_alive(const uint8_t *b, size_t n)
{
    bool all_same = true;
    for (size_t i = 1; i < n; i++) if (b[i] != b[0]) { all_same = false; break; }
    if (!all_same) return true;
    return (b[0] != 0x00 && b[0] != 0xFF);
}

// ---- Attempt A: half-duplex read on MISO (GPIO14) -------------------------
// Command phase (DC=0) holds CS active into the read phase (DC=1) via
// CS_KEEP_ACTIVE + an explicit bus acquire, matching how multi-phase EPD
// reads expect CS to stay low across cmd+data.
static esp_err_t read_miso(uint8_t cmd, uint8_t *rx, size_t n)
{
    spi_device_handle_t dev = (spi_device_handle_t)epd_spi_handle();
    if (!dev) return ESP_ERR_INVALID_STATE;

    esp_err_t err = spi_device_acquire_bus(dev, portMAX_DELAY);
    if (err != ESP_OK) return err;

    spi_transaction_t tc = {
        .length     = 8,
        .tx_buffer  = &cmd,
        .user       = (void *)0,                 // DC=0 (command)
        .flags      = SPI_TRANS_CS_KEEP_ACTIVE,
    };
    err = spi_device_polling_transmit(dev, &tc);
    if (err == ESP_OK) {
        memset(rx, 0, n);
        spi_transaction_t tr = {
            .rxlength  = n * 8,
            .rx_buffer = rx,
            .user      = (void *)1,              // DC=1 (read data)
        };
        err = spi_device_polling_transmit(dev, &tr);
    }
    spi_device_release_bus(dev);
    return err;
}

// ---- Attempt B: bit-banged SIO read on the MOSI data line (GPIO13) --------
// SPI mode 0 (CPOL=0, CPHA=0): clock idles low, master samples on the rising
// edge. ~1us per half-period keeps us well inside the controller's read
// timing while staying fast enough for 64-byte dumps.
#define BB_DELAY_US 1

static void bb_begin(void)
{
    // Reclaim the pads from the SPI peripheral so gpio_set_level drives them.
    gpio_reset_pin(EPD_PIN_SCLK);
    gpio_reset_pin(EPD_PIN_MOSI);
    gpio_reset_pin(EPD_PIN_CS);
    gpio_reset_pin(EPD_PIN_DC);
    gpio_set_direction(EPD_PIN_SCLK, GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_PIN_CS,   GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_PIN_DC,   GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_PIN_MOSI, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(EPD_PIN_SCLK, 0);
    gpio_set_level(EPD_PIN_CS, 1);   // idle: CS high
}

static void bb_write_byte(uint8_t b)
{
    gpio_set_direction(EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(EPD_PIN_MOSI, (b >> i) & 1);
        gpio_set_level(EPD_PIN_SCLK, 1); esp_rom_delay_us(BB_DELAY_US);
        gpio_set_level(EPD_PIN_SCLK, 0); esp_rom_delay_us(BB_DELAY_US);
    }
}

static uint8_t bb_read_byte(void)
{
    uint8_t v = 0;
    gpio_set_direction(EPD_PIN_MOSI, GPIO_MODE_INPUT);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(EPD_PIN_SCLK, 1); esp_rom_delay_us(BB_DELAY_US);
        v = (uint8_t)((v << 1) | (gpio_get_level(EPD_PIN_MOSI) & 1));
        gpio_set_level(EPD_PIN_SCLK, 0); esp_rom_delay_us(BB_DELAY_US);
    }
    return v;
}

static void read_bitbang(uint8_t cmd, uint8_t *rx, size_t n)
{
    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_DC, 0);          // command phase
    bb_write_byte(cmd);
    gpio_set_level(EPD_PIN_DC, 1);          // read phase
    for (size_t i = 0; i < n; i++) rx[i] = bb_read_byte();
    gpio_set_level(EPD_PIN_CS, 1);
}

void epd_probe_run(void)
{
    ESP_LOGW(TAG, "=== EPD PROBE (read-only) START ===");

    // Honor the EPD(PYG0)/SD(PYG3) rail coupling: both rails on before any
    // shared-bus traffic. Then init the bus + panel ourselves (false = not yet
    // inited), leaving REG=0 so the OTP waveform path is the active one.
    (void)pm1_set_sd_power(true);
    epd_prepare_power_off();
    if (pm1_set_epd_power(true) != 0) {
        ESP_LOGE(TAG, "EPD rail enable failed");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    if (epd_init(false) != 0) {
        ESP_LOGE(TAG, "epd_init failed");
        return;
    }
    ESP_LOGI(TAG, "panel inited (REG=0); beginning read attempts");

    uint8_t rx[64];

    ESP_LOGW(TAG, "--- Attempt A: MISO (GPIO%d) half-duplex read ---", EPD_PIN_MISO);
    bool a_alive = false;
    for (size_t i = 0; i < sizeof k_cmds; i++) {
        size_t n = read_len_for(k_cmds[i]);
        esp_err_t err = read_miso(k_cmds[i], rx,n);
        if (err == ESP_OK) {
            hexdump("MISO", k_cmds[i], rx, n);
            if (looks_alive(rx, n)) a_alive = true;
        } else {
            ESP_LOGE(TAG, "MISO read cmd 0x%02x failed: %s",
                     k_cmds[i], esp_err_to_name(err));
        }
    }
    ESP_LOGW(TAG, "Attempt A verdict: %s",
             a_alive ? "DATA SEEN (inspect dump)" : "looks floating/unsupported");

    ESP_LOGW(TAG, "--- Attempt B: bit-bang SIO on MOSI (GPIO%d) ---", EPD_PIN_MOSI);
    bb_begin();   // NOTE: detaches MOSI/SCLK/CS/DC from the SPI peripheral; the
                  // SPI driver and SD are unusable after this point (probe is terminal).
    bool b_alive = false;
    for (size_t i = 0; i < sizeof k_cmds; i++) {
        size_t n = read_len_for(k_cmds[i]);
        read_bitbang(k_cmds[i], rx, n);
        hexdump("SIO", k_cmds[i], rx, n);
        if (looks_alive(rx, n)) b_alive = true;
    }
    ESP_LOGW(TAG, "Attempt B verdict: %s",
             b_alive ? "DATA SEEN (inspect dump)" : "looks floating/unsupported");

    ESP_LOGW(TAG, "=== EPD PROBE DONE ===");
}

#endif // CONFIG_EPD_PROBE

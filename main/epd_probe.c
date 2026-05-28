// epd_probe.c — Phase 1/2 bench reverse-engineering probe. Active only when
// CONFIG_EPD_PROBE=y; otherwise this file compiles to nothing.
//
// Fast-iteration knob: EPD_PROBE_EXP selects the experiment. Edit it, rebuild,
// flash, observe — repeat. Each experiment powers the rails, runs the stock
// init (REG=0), then does its thing.
//
//   0 = READ probe   : Attempt A (MISO half-duplex) + Attempt B (bit-bang SIO)
//                      register reads. (Finding: B works, status regs read,
//                      LUT regs 0x20-0x25 read back 0xFF = write-only.)
//   1 = WRITE baseline: push a 6-colour test-bar pattern via OUR own raw write
//                      path using the stock OTP waveform. Validates the write
//                      path + gives a reference image and refresh timing.
//   2 = REG-mode test : set PSR bit5 (REG=1) with zeroed LUTs, then refresh the
//                      same pattern. If the panel output CHANGES vs EXP=1, the
//                      chip honours register-LUT mode and we can iterate on LUT
//                      content. If identical, this chip ignores the REG bit.
#include "sdkconfig.h"

#if CONFIG_EPD_PROBE

#define EPD_PROBE_EXP 1

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
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "epd_probe";

// ===========================================================================
// EXP 0 — READ probe (register read-back)
// ===========================================================================
#if EPD_PROBE_EXP == 0

static const uint8_t k_cmds[] = { 0x71, 0x70, 0x2E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 };
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

static bool looks_alive(const uint8_t *b, size_t n)
{
    bool all_same = true;
    for (size_t i = 1; i < n; i++) if (b[i] != b[0]) { all_same = false; break; }
    if (!all_same) return true;
    return (b[0] != 0x00 && b[0] != 0xFF);
}

static esp_err_t read_miso(uint8_t cmd, uint8_t *rx, size_t n)
{
    spi_device_handle_t dev = (spi_device_handle_t)epd_spi_handle();
    if (!dev) return ESP_ERR_INVALID_STATE;
    esp_err_t err = spi_device_acquire_bus(dev, portMAX_DELAY);
    if (err != ESP_OK) return err;
    spi_transaction_t tc = {
        .length = 8, .tx_buffer = &cmd, .user = (void *)0,
        .flags = SPI_TRANS_CS_KEEP_ACTIVE,
    };
    err = spi_device_polling_transmit(dev, &tc);
    if (err == ESP_OK) {
        memset(rx, 0, n);
        spi_transaction_t tr = { .rxlength = n * 8, .rx_buffer = rx, .user = (void *)1 };
        err = spi_device_polling_transmit(dev, &tr);
    }
    spi_device_release_bus(dev);
    return err;
}

#define BB_DELAY_US 1
static void bb_begin(void)
{
    gpio_reset_pin(EPD_PIN_SCLK);
    gpio_reset_pin(EPD_PIN_MOSI);
    gpio_reset_pin(EPD_PIN_CS);
    gpio_reset_pin(EPD_PIN_DC);
    gpio_set_direction(EPD_PIN_SCLK, GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_PIN_CS,   GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_PIN_DC,   GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_PIN_MOSI, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(EPD_PIN_SCLK, 0);
    gpio_set_level(EPD_PIN_CS, 1);
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
    gpio_set_level(EPD_PIN_DC, 0);
    bb_write_byte(cmd);
    gpio_set_level(EPD_PIN_DC, 1);
    for (size_t i = 0; i < n; i++) rx[i] = bb_read_byte();
    gpio_set_level(EPD_PIN_CS, 1);
}

static void do_experiment(void)
{
    uint8_t rx[64];

    ESP_LOGW(TAG, "--- Attempt A: MISO (GPIO%d) half-duplex read ---", EPD_PIN_MISO);
    bool a_alive = false;
    for (size_t i = 0; i < sizeof k_cmds; i++) {
        size_t n = read_len_for(k_cmds[i]);
        esp_err_t err = read_miso(k_cmds[i], rx, n);
        if (err == ESP_OK) {
            hexdump("MISO", k_cmds[i], rx, n);
            if (looks_alive(rx, n)) a_alive = true;
        } else {
            ESP_LOGE(TAG, "MISO read cmd 0x%02x failed: %s", k_cmds[i], esp_err_to_name(err));
        }
    }
    ESP_LOGW(TAG, "Attempt A verdict: %s", a_alive ? "DATA SEEN" : "looks floating/unsupported");

    ESP_LOGW(TAG, "--- Attempt B: bit-bang SIO on MOSI (GPIO%d) ---", EPD_PIN_MOSI);
    bb_begin();   // detaches MOSI/SCLK/CS/DC from the SPI peripheral; terminal.
    bool b_alive = false;
    for (size_t i = 0; i < sizeof k_cmds; i++) {
        size_t n = read_len_for(k_cmds[i]);
        read_bitbang(k_cmds[i], rx, n);
        hexdump("SIO", k_cmds[i], rx, n);
        if (looks_alive(rx, n)) b_alive = true;
    }
    ESP_LOGW(TAG, "Attempt B verdict: %s", b_alive ? "DATA SEEN" : "looks floating/unsupported");
}

// ===========================================================================
// EXP 1/2 — WRITE experiments
// ===========================================================================
#else

// Raw command/data via the stock EPD SPI device handle (DC flips through the
// driver's pre_transfer_cb, keyed on the transaction `user` field).
static void probe_cmd(uint8_t c)
{
    spi_device_handle_t dev = (spi_device_handle_t)epd_spi_handle();
    spi_transaction_t t = { .length = 8, .tx_buffer = &c, .user = (void *)0 };
    spi_device_polling_transmit(dev, &t);
}
static void probe_data(const uint8_t *b, size_t n)
{
    if (!n) return;
    spi_device_handle_t dev = (spi_device_handle_t)epd_spi_handle();
    spi_transaction_t t = { .length = n * 8, .tx_buffer = b, .user = (void *)1 };
    spi_device_polling_transmit(dev, &t);
}
static void probe_data1(uint8_t v) { probe_data(&v, 1); }

// Bounded BUSY wait. A bad/empty LUT can leave BUSY asserted forever with the
// HV rails on — dangerous for the panel — so cap the wait and report a timeout
// rather than spinning under drive indefinitely. Baseline refresh is ~14.5 s.
#define PROBE_BUSY_TIMEOUT_MS 25000
static bool probe_wait_busy(void)
{
    int waited = 0;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        if (waited >= PROBE_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY stuck >%d ms — aborting wait", PROBE_BUSY_TIMEOUT_MS);
            return false;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return true;
}

// Replicates epd_turn_on() from epd_4in0e.c, with refresh timing logged.
static void probe_turn_on(void)
{
    int64_t t0 = esp_timer_get_time();
    probe_cmd(0x04);                 // POWER_ON
    probe_wait_busy();
    vTaskDelay(pdMS_TO_TICKS(200));

    probe_cmd(0x06);                 // booster (second setting, per reference)
    probe_data1(0x6F); probe_data1(0x1F); probe_data1(0x17); probe_data1(0x27);
    vTaskDelay(pdMS_TO_TICKS(200));

    int64_t t_drf = esp_timer_get_time();
    probe_cmd(0x12); probe_data1(0x00);   // DISPLAY_REFRESH
    probe_wait_busy();
    int64_t t_done = esp_timer_get_time();

    probe_cmd(0x02); probe_data1(0x00);   // POWER_OFF
    probe_wait_busy();
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGW(TAG, "refresh timing: DRF->BUSY %lld ms, total %lld ms",
             (t_done - t_drf) / 1000, (t_done - t0) / 1000);
}

// 6 vertical colour bars (black/white/yellow/red/blue/green), 4bpp packed.
static uint8_t *make_bars(void)
{
    const int W = EPD_4IN0E_WIDTH, H = EPD_4IN0E_HEIGHT, W2 = W / 2;
    uint8_t *fb = heap_caps_malloc((size_t)W2 * H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) { ESP_LOGE(TAG, "fb alloc failed"); return NULL; }
    static const uint8_t inks[6] = { 0x0, 0x1, 0x2, 0x3, 0x5, 0x6 };
    for (int y = 0; y < H; y++) {
        for (int xb = 0; xb < W2; xb++) {
            int x0 = xb * 2, x1 = x0 + 1;
            uint8_t c0 = inks[(x0 * 6) / W];
            uint8_t c1 = inks[(x1 * 6) / W];
            fb[(size_t)y * W2 + xb] = (uint8_t)((c0 << 4) | (c1 & 0x0F));
        }
    }
    return fb;
}

static void push_framebuffer(const uint8_t *fb)
{
    const size_t total = (size_t)(EPD_4IN0E_WIDTH / 2) * EPD_4IN0E_HEIGHT;
    probe_cmd(0x10);
    const size_t CHUNK = 4096;
    size_t sent = 0;
    while (sent < total) {
        size_t n = (total - sent) > CHUNK ? CHUNK : (total - sent);
        probe_data(fb + sent, n);
        sent += n;
    }
}

#if EPD_PROBE_EXP == 2
// Re-send PSR (0x00) with bit5 (REG) set → waveform from registers, not OTP.
// Init value is {0x5F,0x69}; 0x5F|0x20 = 0x7F.
static void set_reg_mode_on(void)
{
    uint8_t d[2] = { 0x7F, 0x69 };
    probe_cmd(0x00); probe_data(d, sizeof d);
    ESP_LOGW(TAG, "PSR REG=1 -> {0x7F,0x69}");
}
// Write all-zero LUTs to the UC8179-style addresses 0x20..0x24 (42B each).
// Deliberately wrong/empty: if the panel still renders normally the chip is
// ignoring REG mode; if it changes, REG mode is honoured.
static void write_zero_luts(void)
{
    uint8_t z[42] = { 0 };
    for (uint8_t cmd = 0x20; cmd <= 0x24; cmd++) { probe_cmd(cmd); probe_data(z, sizeof z); }
    ESP_LOGW(TAG, "wrote zeroed LUTs 0x20-0x24 (42B each)");
}
#endif

static void do_experiment(void)
{
    uint8_t *fb = make_bars();
    if (!fb) return;

#if EPD_PROBE_EXP == 2
    set_reg_mode_on();
    write_zero_luts();
    ESP_LOGW(TAG, "EXP 2: REG-mode test — expect CHANGED output if REG honoured");
#else
    ESP_LOGW(TAG, "EXP 1: baseline — 6-colour bars via stock OTP waveform");
#endif

    push_framebuffer(fb);
    probe_turn_on();
    heap_caps_free(fb);
    ESP_LOGW(TAG, "EXP %d refresh complete — LOOK AT THE PANEL", EPD_PROBE_EXP);
}

#endif // EPD_PROBE_EXP

// ===========================================================================
// Common entry
// ===========================================================================
void epd_probe_run(void)
{
    ESP_LOGW(TAG, "=== EPD PROBE START (EXP=%d) ===", EPD_PROBE_EXP);

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
    ESP_LOGI(TAG, "panel inited (REG=0)");

    do_experiment();

    ESP_LOGW(TAG, "=== EPD PROBE DONE ===");
}

#endif // CONFIG_EPD_PROBE

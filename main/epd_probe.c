// epd_probe.c — Phase 1/2 bench reverse-engineering probe. Active only when
// CONFIG_EPD_PROBE=y; otherwise this file compiles to nothing.
//
// Fast-iteration knob: EPD_PROBE_EXP selects the experiment. Edit it, rebuild,
// flash, observe — repeat. Each experiment powers the rails, runs the stock
// init (REG=0), then does its thing.
//
//   0 = READ probe   : Attempt A (MISO half-duplex) + Attempt B (bit-bang SIO)
//                      register reads. Finding: B works; status regs read;
//                      LUT regs 0x20-0x25 read 0xFF (write-only).
//   1 = WRITE baseline: push a 6-colour test-bar pattern via our own raw write
//                      path using the stock OTP waveform. Validates write path,
//                      gives a reference image + refresh timing (~14.5 s).
//   2 = REG-mode test : PSR bit5 (REG=1) + zeroed LUTs. CONFIRMED REG mode is
//                      honoured (output changed). [bad-waveform — do not rerun]
//   3 = minimal LUT   : guessed 5x42B LUT. Wedged the panel. [do not rerun]
//   4 = OTP DUMP      : read the built-in waveform out of OTP via ROTP (0x92)
//                      over the bit-bang SIO channel. READ-ONLY. This is the
//                      right way to get the real waveform; EXP 2/3 were wrong.
//
// Chip is a UC8159 / SPD1656-class ACeP controller. OTP layout (per SPD1656):
// per-temperature (10 ranges) VCOM LUT (20 states x 11 B) + 8 colour LUTs
// (20 states x 13 B); each state = 8 phases of [repeat, level-select, frames].
#include "sdkconfig.h"

#if CONFIG_EPD_PROBE

#define EPD_PROBE_EXP 4

// EXP 4 only: how many OTP bytes to read+dump. Start modest to validate capture
// integrity over USB-CDC; raise toward the full image (~96000 on the sibling
// ED2208 panel) once a clean partial dump is confirmed.
#define OTP_DUMP_BYTES 98304

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
// Bit-bang SIO read on the MOSI/SDA line (GPIO13) — used by EXP 0 and EXP 4.
// MISO has no SDO wired; the controller drives reads back on the data line.
// SPI mode 0: clock idles low, sample on the rising edge. Reclaims the pads
// from the SPI peripheral (terminal — SPI/SD unusable afterward).
// ===========================================================================
#if EPD_PROBE_EXP == 0 || EPD_PROBE_EXP == 4

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
// Send command, then read n bytes (no dummy). Suitable for REV/FLG/register reads.
static void read_bitbang(uint8_t cmd, uint8_t *rx, size_t n)
{
    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_DC, 0);
    bb_write_byte(cmd);
    gpio_set_level(EPD_PIN_DC, 1);
    for (size_t i = 0; i < n; i++) rx[i] = bb_read_byte();
    gpio_set_level(EPD_PIN_CS, 1);
}
#endif // bit-bang read helpers

// ===========================================================================
// EXP 0 — register read-back (also MISO attempt)
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

static void do_experiment(void)
{
    uint8_t rx[64];
    ESP_LOGW(TAG, "--- Attempt A: MISO (GPIO%d) half-duplex read ---", EPD_PIN_MISO);
    bool a_alive = false;
    for (size_t i = 0; i < sizeof k_cmds; i++) {
        size_t n = read_len_for(k_cmds[i]);
        esp_err_t err = read_miso(k_cmds[i], rx, n);
        if (err == ESP_OK) { hexdump("MISO", k_cmds[i], rx, n); if (looks_alive(rx, n)) a_alive = true; }
        else ESP_LOGE(TAG, "MISO read 0x%02x failed: %s", k_cmds[i], esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "Attempt A verdict: %s", a_alive ? "DATA SEEN" : "floating/unsupported");

    ESP_LOGW(TAG, "--- Attempt B: bit-bang SIO on MOSI (GPIO%d) ---", EPD_PIN_MOSI);
    bb_begin();
    bool b_alive = false;
    for (size_t i = 0; i < sizeof k_cmds; i++) {
        size_t n = read_len_for(k_cmds[i]);
        read_bitbang(k_cmds[i], rx, n);
        hexdump("SIO", k_cmds[i], rx, n);
        if (looks_alive(rx, n)) b_alive = true;
    }
    ESP_LOGW(TAG, "Attempt B verdict: %s", b_alive ? "DATA SEEN" : "floating/unsupported");
}

// ===========================================================================
// EXP 4 — OTP / waveform dump via ROTP (0x92), READ-ONLY
// ===========================================================================
#elif EPD_PROBE_EXP == 4

static void do_experiment(void)
{
    bb_begin();

    // Sanity: same commands that worked in EXP 0. REV(0x70) was 01 01 04 on
    // this panel (sibling ED2208 returns 01 01 03), confirming the command set.
    uint8_t rev[3] = { 0 };
    read_bitbang(0x70, rev, sizeof rev);
    ESP_LOGW(TAG, "REV(0x70) = %02x %02x %02x", rev[0], rev[1], rev[2]);
    uint8_t flg = 0;
    read_bitbang(0x71, &flg, 1);
    ESP_LOGW(TAG, "FLG(0x71) = %02x", flg);

    uint8_t *buf = heap_caps_malloc(OTP_DUMP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "OTP buffer alloc failed"); return; }

    // ROTP (0x92): after the command the 1st byte read is a DUMMY, then byte N
    // is OTP address N. One continuous CS-low frame.
    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_DC, 0);
    bb_write_byte(0x92);
    gpio_set_level(EPD_PIN_DC, 1);
    (void)bb_read_byte();                       // discard dummy byte
    for (size_t i = 0; i < OTP_DUMP_BYTES; i++) buf[i] = bb_read_byte();
    gpio_set_level(EPD_PIN_CS, 1);

    // Quick content check so we know the read wasn't floating/padding.
    size_t nonzero = 0, nonff = 0;
    for (size_t i = 0; i < OTP_DUMP_BYTES; i++) { if (buf[i] != 0x00) nonzero++; if (buf[i] != 0xFF) nonff++; }
    ESP_LOGW(TAG, "OTP read %d bytes: %u non-zero, %u non-FF", OTP_DUMP_BYTES, (unsigned)nonzero, (unsigned)nonff);

    ESP_LOGW(TAG, "=== OTP DUMP BEGIN (ROTP 0x92, %d bytes) ===", OTP_DUMP_BYTES);
    for (size_t i = 0; i < OTP_DUMP_BYTES; i += 32) {
        printf("%05x: ", (unsigned)i);
        for (size_t j = 0; j < 32 && i + j < OTP_DUMP_BYTES; j++) printf("%02x", buf[i + j]);
        printf("\n");
        if ((i & 0x7FF) == 0) { fflush(stdout); vTaskDelay(1); }   // let USB-CDC drain
    }
    fflush(stdout);
    ESP_LOGW(TAG, "=== OTP DUMP END ===");
    heap_caps_free(buf);
}

// ===========================================================================
// EXP 1/2/3 — WRITE experiments (OTP baseline / REG-mode tests)
// ===========================================================================
#else

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

static bool probe_turn_on(void)
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
    bool drf_ok = probe_wait_busy();
    int64_t t_done = esp_timer_get_time();
    probe_cmd(0x02); probe_data1(0x00);   // POWER_OFF
    probe_wait_busy();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "refresh timing: DRF->BUSY %lld ms, total %lld ms (%s)",
             (t_done - t_drf) / 1000, (t_done - t0) / 1000, drf_ok ? "completed" : "TIMED OUT");
    return drf_ok;
}

static uint8_t *make_bars(void)
{
    const int W = EPD_4IN0E_WIDTH, H = EPD_4IN0E_HEIGHT, W2 = W / 2;
    uint8_t *fb = heap_caps_malloc((size_t)W2 * H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) { ESP_LOGE(TAG, "fb alloc failed"); return NULL; }
    static const uint8_t inks[6] = { 0x0, 0x1, 0x2, 0x3, 0x5, 0x6 };
    for (int y = 0; y < H; y++) {
        for (int xb = 0; xb < W2; xb++) {
            int x0 = xb * 2, x1 = x0 + 1;
            fb[(size_t)y * W2 + xb] = (uint8_t)((inks[(x0 * 6) / W] << 4) | (inks[(x1 * 6) / W] & 0x0F));
        }
    }
    return fb;
}
static void push_framebuffer(const uint8_t *fb)
{
    const size_t total = (size_t)(EPD_4IN0E_WIDTH / 2) * EPD_4IN0E_HEIGHT;
    probe_cmd(0x10);
    const size_t CHUNK = 4096;
    for (size_t sent = 0; sent < total; ) {
        size_t n = (total - sent) > CHUNK ? CHUNK : (total - sent);
        probe_data(fb + sent, n);
        sent += n;
    }
}

#if EPD_PROBE_EXP >= 2
static void set_reg_mode_on(void)
{
    uint8_t d[2] = { 0x7F, 0x69 };
    probe_cmd(0x00); probe_data(d, sizeof d);
    ESP_LOGW(TAG, "PSR REG=1 -> {0x7F,0x69}");
}
static void recover_panel(const uint8_t *fb)
{
    ESP_LOGW(TAG, "recovery: re-init to OTP (REG=0) + clean refresh");
    epd_init(true);
    push_framebuffer(fb);
    probe_turn_on();
    ESP_LOGW(TAG, "recovery refresh done — panel restored to OTP bars");
}
#endif

#if EPD_PROBE_EXP == 2
static void write_luts(void)
{
    uint8_t z[42] = { 0 };
    for (uint8_t cmd = 0x20; cmd <= 0x24; cmd++) { probe_cmd(cmd); probe_data(z, sizeof z); }
    ESP_LOGW(TAG, "wrote zeroed LUTs 0x20-0x24 (42B each)");
}
#elif EPD_PROBE_EXP == 3
static void write_luts(void)
{
    static const uint8_t vcom[42]   = { 0x00, 0x04, 0x04, 0x00, 0x00, 0x01 };
    static const uint8_t lut_ww[42] = { 0x80, 0x04, 0x04, 0x00, 0x00, 0x01 };
    static const uint8_t lut_bw[42] = { 0x90, 0x04, 0x04, 0x00, 0x00, 0x01 };
    static const uint8_t lut_wb[42] = { 0x60, 0x04, 0x04, 0x00, 0x00, 0x01 };
    static const uint8_t lut_bb[42] = { 0x00, 0x04, 0x04, 0x00, 0x00, 0x01 };
    probe_cmd(0x20); probe_data(vcom,   sizeof vcom);
    probe_cmd(0x21); probe_data(lut_ww, sizeof lut_ww);
    probe_cmd(0x22); probe_data(lut_bw, sizeof lut_bw);
    probe_cmd(0x23); probe_data(lut_wb, sizeof lut_wb);
    probe_cmd(0x24); probe_data(lut_bb, sizeof lut_bb);
    ESP_LOGW(TAG, "wrote minimal 1-phase LUTs 0x20-0x24 (42B each)");
}
#endif

static void do_experiment(void)
{
    uint8_t *fb = make_bars();
    if (!fb) return;
#if EPD_PROBE_EXP == 1
    ESP_LOGW(TAG, "EXP 1: baseline — 6-colour bars via stock OTP waveform");
    push_framebuffer(fb);
    probe_turn_on();
#else
    set_reg_mode_on();
    write_luts();
    ESP_LOGW(TAG, "EXP %d: custom register-LUT refresh — observe panel", EPD_PROBE_EXP);
    push_framebuffer(fb);
    bool ok = probe_turn_on();
    ESP_LOGW(TAG, "EXP %d custom-LUT refresh %s", EPD_PROBE_EXP, ok ? "COMPLETED" : "TIMED OUT");
    recover_panel(fb);           // never leave the panel in a wrong state
#endif
    heap_caps_free(fb);
    ESP_LOGW(TAG, "EXP %d done", EPD_PROBE_EXP);
}

#endif // EXP dispatch

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

// pm1.c — minimal M5PM1 PMIC driver.  Talks to the chip at I2C 0x6E and
// configures the GPIO mode + output registers to bring the EPD and SD
// power rails online. SD card presence is not detected through the PMIC;
// the SD driver tries to initialize the card directly. All other PMIC
// features (LDOs, ADCs, RTC, NeoPixel
// driver, watchdog) are left untouched — the chip's reset defaults already
// have L1/L2/L3A on, so we only need to flip the few GPIOs the board uses.

#include "pm1.h"
#include "board_pins.h"

#include <string.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pm1";

#define PM1_REG_DEVICE_ID  0x00
#define PM1_REG_PWR_CFG    0x06
#define PM1_REG_HOLD_CFG   0x07
#define PM1_REG_GPIO_MODE  0x10
#define PM1_REG_GPIO_OUT   0x11
#define PM1_REG_GPIO_DRV   0x13
#define PM1_REG_GPIO_PUPD0 0x14
#define PM1_REG_GPIO_PUPD1 0x15
#define PM1_REG_GPIO_FUNC0 0x16
#define PM1_REG_GPIO_FUNC1 0x17

#define PM1_RAIL_BITS  ((1 << PM1_PYG0) | (1 << PM1_PYG3))

static bool s_i2c_installed = false;

static int i2c_install_once(void)
{
    if (s_i2c_installed) return 0;
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = BOARD_I2C_SDA,
        .scl_io_num       = BOARD_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BOARD_I2C_HZ,
    };
    if (i2c_param_config(BOARD_I2C_PORT, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed");
        return -1;
    }
    if (i2c_driver_install(BOARD_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed");
        return -1;
    }
    s_i2c_installed = true;
    return 0;
}

// PMIC wake: bare address probe (START + addr+W + STOP). The chip uses the
// resulting SDA edge to fire its EXTI wake interrupt — without this it stays
// asleep and NAKs every register access. Mirrors M5PM1_I2C_MASTER_SEND_WAKE
// in the Arduino library.
static void pm1_send_wake(void)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PM1_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    (void)i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(30));
    i2c_cmd_link_delete(cmd);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static int pm1_read_reg(uint8_t reg, uint8_t *val)
{
    // Try, and on failure send a wake pulse and retry — matches the Arduino
    // lib's retry-after-wake pattern.
    for (int attempt = 0; attempt < 3; attempt++) {
        esp_err_t e1 = i2c_master_write_to_device(
            BOARD_I2C_PORT, PM1_I2C_ADDR, &reg, 1, pdMS_TO_TICKS(200));
        if (e1 == ESP_OK) {
            esp_err_t e2 = i2c_master_read_from_device(
                BOARD_I2C_PORT, PM1_I2C_ADDR, val, 1, pdMS_TO_TICKS(200));
            if (e2 == ESP_OK) return 0;
        }
        pm1_send_wake();
    }
    ESP_LOGW(TAG, "read 0x%02x failed after retries", reg);
    return -1;
}

static int pm1_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    for (int attempt = 0; attempt < 3; attempt++) {
        esp_err_t e = i2c_master_write_to_device(
            BOARD_I2C_PORT, PM1_I2C_ADDR, buf, 2, pdMS_TO_TICKS(200));
        if (e == ESP_OK) return 0;
        pm1_send_wake();
    }
    ESP_LOGW(TAG, "write 0x%02x=0x%02x failed after retries", reg, val);
    return -1;
}

// Read-modify-write a single bit in `reg`.
static int pm1_rmw_bit(uint8_t reg, uint8_t bit, bool on)
{
    uint8_t v;
    if (pm1_read_reg(reg, &v) != 0) return -1;
    uint8_t nv = on ? (v | (1u << bit)) : (v & ~(1u << bit));
    if (nv == v) return 0;
    return pm1_write_reg(reg, nv);
}

// Walk the 7-bit address space and log anything that ACKs a zero-byte write.
// Useful when the PMIC isn't responding at the expected address — we can see
// whether ANY device is on the bus and at what address.
static void pm1_scan_bus(void)
{
    ESP_LOGI(TAG, "I2C scan on SDA=G%d SCL=G%d:", BOARD_I2C_SDA, BOARD_I2C_SCL);
    int found = 0;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (a << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t e = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(30));
        i2c_cmd_link_delete(cmd);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "  device ACK at 0x%02x", a);
            found++;
        }
    }
    if (!found) ESP_LOGW(TAG, "  no devices responded — check pull-ups / power / pins");
}

int pm1_init(void)
{
    if (i2c_install_once() != 0) return -1;

    // M5PM1 enters auto-sleep when there's no recent I2C traffic, and NAKs
    // register accesses until woken.  Send a bare-address pulse and wait
    // ~10ms before the first real transaction.
    pm1_send_wake();

    // The chip ACKs but reg 0x00 is not a reliable probe target.  Skip the
    // device-ID read and go straight to the writes — if those succeed, the
    // chip is on the bus and configured.

    // GPIO_MODE: set PYG0 / PYG3 as outputs.  Read-modify-write so
    // we don't disturb whatever other bits the bootloader / RTC backup
    // domain left in there.
    uint8_t mode = 0;
    (void)pm1_read_reg(PM1_REG_GPIO_MODE, &mode);    // best-effort
    mode |= PM1_RAIL_BITS;
    if (pm1_write_reg(PM1_REG_GPIO_MODE, mode) != 0) {
        ESP_LOGE(TAG, "M5PM1 GPIO_MODE write failed at I2C 0x%02x", PM1_I2C_ADDR);
        pm1_scan_bus();
        return -1;
    }

    // GPIO_DRV: switch our 2 rail bits from open-drain (default 0x1F) to
    // push-pull.  PYG0/3 drive load-switch MOSFET gates with no external
    // pull-up — open-drain won't pull them high.
    uint8_t drv = 0;
    (void)pm1_read_reg(PM1_REG_GPIO_DRV, &drv);
    drv &= ~PM1_RAIL_BITS;
    if (pm1_write_reg(PM1_REG_GPIO_DRV, drv) != 0) {
        ESP_LOGE(TAG, "M5PM1 GPIO_DRV write failed");
        return -1;
    }

    // GPIO_OUT: drive them high.
    uint8_t out = 0;
    (void)pm1_read_reg(PM1_REG_GPIO_OUT, &out);
    out |= PM1_RAIL_BITS;
    if (pm1_write_reg(PM1_REG_GPIO_OUT, out) != 0) {
        ESP_LOGE(TAG, "M5PM1 GPIO_OUT write failed at I2C 0x%02x", PM1_I2C_ADDR);
        return -1;
    }

    ESP_LOGI(TAG, "rails on: PYG0(EPD)=1 PYG3(SD)=1 "
                  "(mode=0x%02x out=0x%02x drv=0x%02x)", mode, out, drv);

    // SD spec allows up to 250 ms from power-good to ready for the first
    // CMD0. Give 300 ms to cover the slowest cards.
    vTaskDelay(pdMS_TO_TICKS(300));
    return 0;
}

int pm1_set_epd_power(bool on)
{
    return pm1_rmw_bit(PM1_REG_GPIO_OUT, PM1_PYG0, on);
}

int pm1_set_sd_power(bool on)
{
    return pm1_rmw_bit(PM1_REG_GPIO_OUT, PM1_PYG3, on);
}

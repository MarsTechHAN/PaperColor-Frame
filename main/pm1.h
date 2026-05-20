#ifndef PAPER_E6_PM1_H
#define PAPER_E6_PM1_H

// Minimum-viable M5PM1 PMIC driver for ESP-IDF.
//
// The M5PaperColor (C151) routes EPD power and SD power through the PMIC's
// GPIO ports (PYG0..PYG4) rather than ESP32
// GPIOs.  This module wraps the two registers we actually need (0x10
// GPIO_MODE + 0x11 GPIO_OUT) so main.c can bring up the rails before SD
// mount / EPD init. SD presence is not checked through the PMIC; SD mount
// attempts initialization once and reports the driver error on failure.
//
// Lifted from the M5PM1 Arduino library's gpioSetMode/gpioSetOutput
// implementations.  We never use that library directly — the user wants
// a clean ESP-IDF dependency tree (no m5unified).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM1_I2C_ADDR     0x6E

// PMIC GPIO ports (bit positions in REG 0x10 / 0x11). PYG1/PYG4 are card-
// detect related hardware lines, but this firmware intentionally does not use
// them to decide whether to mount SD.
enum {
    PM1_PYG0 = 0,   // PY_EPD_EN — gates EPD power
    PM1_PYG1 = 1,   // CARD_DEC — unused SD card insertion detect
    PM1_PYG2 = 2,   // RTC IRQ (input)
    PM1_PYG3 = 3,   // PY_SD_PWR_EN — gates microSD power
    PM1_PYG4 = 4,   // PY_SD_DET_EN — unused SD detect pull-up enable
};

// Bring up the I2C bus, probe the PMIC, then enable only the always-needed
// storage rail:
//   * PYG0 = output low  (EPD power is enabled only during refresh)
//   * PYG3 = output high (SD power)
// Returns 0 on success.  Leaves the I2C bus installed on BOARD_I2C_PORT.
int pm1_init(void);

// Toggle EPD rail without re-initialising I2C (e.g. for power-save).
int pm1_set_epd_power(bool on);

// Toggle SD rail.
int pm1_set_sd_power(bool on);

// Toggle the PMIC RGB_EN output that powers the top NeoPixels.
int pm1_set_rgb_power(bool on);

// Optional external boost rail.  Do not disable the 3.3 V buck/LDO rail in
// deep sleep because it can be the ESP32's own supply.
int pm1_set_dcdc_power(bool on);
int pm1_set_boost_power(bool on);

// Battery voltage from the PMIC ADC, in millivolts.
int pm1_read_battery_mv(uint16_t *mv);

// Approximate single-cell Li-ion state-of-charge from VBAT in millivolts.
uint8_t pm1_battery_percent_from_mv(uint16_t mv);

// Best-effort shutdown of external loads before ESP deep sleep.
void pm1_prepare_deep_sleep(void);

#ifdef __cplusplus
}
#endif
#endif

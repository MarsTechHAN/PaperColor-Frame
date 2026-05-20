#ifndef PAPER_E6_BOARD_PINS_H
#define PAPER_E6_BOARD_PINS_H

// Board: M5Stack PaperColor (C151).
// Panel: ED2208-DOA / EL040EF1, 400x600, E-Ink Spectra 6.
//
// Important: SD card power (PYG3) and EPD power (PYG0) are NOT driven by
// ESP32 GPIOs — they are gated through the on-board M5PM1 PMIC at I2C
// address 0x6E.  The PMIC must be configured before SD mount / EPD init,
// otherwise both peripherals stay dark.  SD presence is not checked through
// the PMIC card-detect pin; mount attempts initialization directly. See pm1.h.

#define EPD_SPI_HOST     SPI2_HOST

// SPI bus — shared between SD and EPD.
#define EPD_PIN_MOSI     13
#define EPD_PIN_MISO     14
#define EPD_PIN_SCLK     15

// EPD chip-select / control lines.
#define EPD_PIN_CS       44
#define EPD_PIN_DC       43
#define EPD_PIN_BUSY     11
#define EPD_PIN_RST      12

// microSD chip-select.
#ifndef SD_PIN_CS
#define SD_PIN_CS        47
#endif

// I2C bus (M5PM1 PMIC at 0x6E, RX8130CE RTC, SHT40, audio codec).
#define BOARD_I2C_PORT   0
#define BOARD_I2C_SDA    3
#define BOARD_I2C_SCL    2
#define BOARD_I2C_HZ     100000

// On-board buttons (active-low).
#define BOARD_BTN_TOP_GPIO   1
#define BOARD_BTN_UP_GPIO    9
#define BOARD_BTN_DOWN_GPIO  10

// Two top NeoPixels are powered by PMIC RGB_EN and driven by ESP32 G21.
#define BOARD_NEOPIXEL_GPIO  21
#define BOARD_NEOPIXEL_COUNT 2

#define EPD_SPI_CLOCK_HZ   (4 * 1000 * 1000)   // 4 MHz panel data rate
#define SD_SPI_CLOCK_KHZ   (20000)             // 20 MHz initial; sdspi negotiates

#endif

#ifndef PAPER_E6_EPD_4IN0E_H
#define PAPER_E6_EPD_4IN0E_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_4IN0E_WIDTH   400
#define EPD_4IN0E_HEIGHT  600

// Initialise the shared SPI bus (SPI2_HOST) and the panel.  Safe to call once
// at boot.  Returns 0 on success, negative ESP error otherwise.
//
// `bus_already_inited` skips spi_bus_initialize() — pass true when the SD
// card driver has already configured the bus.
int  epd_init(bool bus_already_inited);

// Push a full 4-bit framebuffer (size = EPD_4IN0E_WIDTH/2 * EPD_4IN0E_HEIGHT
// = 120000 bytes) to the panel and trigger the refresh.
// Returns 0 on success, -1 if the panel's BUSY line never released (a wedged
// controller) — the caller should attempt a power-cycle recovery.
int epd_display(const uint8_t *fb);

// Fill the entire panel with a single 4-bit ink code (0..6).
void epd_clear(uint8_t ink_code);

// Send the deep-sleep command.  After this, only a hardware reset wakes the
// controller.
void epd_sleep(void);

// Hold EPD-only control pins in a safe state before/after PMIC rail gating.
void epd_prepare_power_off(void);

// Returns the SPI device handle so the SD-card init can register on the same
// bus without re-initialising it.
struct spi_device_t;
struct spi_device_t *epd_spi_handle(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef PAPER_E6_EPD_PROBE_H
#define PAPER_E6_EPD_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

// Read-only Phase-1 probe. Self-contained: powers the PMIC rails, inits the
// SPI bus + panel, attempts controller read-back two ways, and dumps the
// results over UART. Never returns control to the normal app stack.
void epd_probe_run(void);

#ifdef __cplusplus
}
#endif

#endif

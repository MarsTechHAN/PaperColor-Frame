#ifndef PAPER_E6_BATTERY_OVERLAY_H
#define PAPER_E6_BATTERY_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Draws a small adaptive black/white battery icon in the panel's top-right
// corner over a packed 4bpp EPD framebuffer.
void battery_overlay_draw(uint8_t *fb, uint8_t percent);

#ifdef __cplusplus
}
#endif
#endif

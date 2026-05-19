#ifndef PAPER_E6_PALETTE_H
#define PAPER_E6_PALETTE_H

#include <stdint.h>

// E6 panel ink-code (low 4 bits) as written to the controller.
// See EPD_4IN0E datasheet: 4 bits per pixel, two pixels per byte.
// Note that ink codes 0x4 and 0x7 are reserved/unused — the panel uses
// 0,1,2,3,5,6 only.  This matches INK_CODES in converter.py.
#define EPD_INK_BLACK   0x0
#define EPD_INK_WHITE   0x1
#define EPD_INK_YELLOW  0x2
#define EPD_INK_RED     0x3
#define EPD_INK_BLUE    0x5
#define EPD_INK_GREEN   0x6

#define PALETTE_N 6

// Internal palette indices used by the dither code.
enum {
    PIDX_BLACK = 0,
    PIDX_WHITE,
    PIDX_YELLOW,
    PIDX_RED,
    PIDX_BLUE,
    PIDX_GREEN
};

// Ink-code lookup keyed by internal palette index.
extern const uint8_t PALETTE_INK_CODE[PALETTE_N];

// Nominal sRGB primaries for labels/tests.  The real panel appearance is the
// measured reflective palette below; dither matching uses measured Lab.
extern const uint8_t PALETTE_RGB[PALETTE_N][3];

// Measured colorimeter values (average of 3 readings under the user's
// PANTONE-LS C2019 setup).  Used by palette_init() for distance matching.
extern const uint8_t PALETTE_RGB_MEASURED[PALETTE_N][3];

// Pre-computed CIELAB (D65 sRGB interpretation of measured appearance RGB).
// Filled by palette_init() at boot from PALETTE_RGB_MEASURED.
extern float PALETTE_LAB[PALETTE_N][3];

void palette_init(void);

#endif

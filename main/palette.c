#include "palette.h"
#include "color_pipeline.h"

const uint8_t PALETTE_INK_CODE[PALETTE_N] = {
    [PIDX_BLACK]  = EPD_INK_BLACK,
    [PIDX_WHITE]  = EPD_INK_WHITE,
    [PIDX_YELLOW] = EPD_INK_YELLOW,
    [PIDX_RED]    = EPD_INK_RED,
    [PIDX_BLUE]   = EPD_INK_BLUE,
    [PIDX_GREEN]  = EPD_INK_GREEN,
};

// Nominal sRGB primaries.  Kept for labels/tests; distance matching uses the
// measured reflective palette below because the panel is not emissive sRGB.
const uint8_t PALETTE_RGB[PALETTE_N][3] = {
    [PIDX_BLACK]  = {  0,   0,   0},
    [PIDX_WHITE]  = {255, 255, 255},
    [PIDX_YELLOW] = {255, 221,   0},
    [PIDX_RED]    = {206,  38,  54},
    [PIDX_BLUE]   = {  0,  64, 255},
    [PIDX_GREEN]  = {  0, 128,   0},
};

// Averaged colorimeter measurements from the user's three test points
// (rounded). These are panel appearance values under the measured lighting.
const uint8_t PALETTE_RGB_MEASURED[PALETTE_N][3] = {
    [PIDX_BLACK]  = { 70,  71,  80},
    [PIDX_WHITE]  = {151, 161, 160},
    [PIDX_YELLOW] = {163, 154,  69},
    [PIDX_RED]    = {118,  69,  70},
    [PIDX_BLUE]   = { 60,  97, 134},
    [PIDX_GREEN]  = { 77, 100,  76},
};

float PALETTE_LAB[PALETTE_N][3];

void palette_init(void)
{
    for (int i = 0; i < PALETTE_N; i++) {
        rgb_to_lab_u8(PALETTE_RGB_MEASURED[i][0],
                      PALETTE_RGB_MEASURED[i][1],
                      PALETTE_RGB_MEASURED[i][2],
                      &PALETTE_LAB[i][0], &PALETTE_LAB[i][1], &PALETTE_LAB[i][2]);
    }
}

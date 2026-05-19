#ifndef PAPER_E6_COLOR_PIPELINE_H
#define PAPER_E6_COLOR_PIPELINE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 7-axis per-image config.  The first six values are colour adjustment
// (mirroring Lightroom's basic panel: temperature is the yellow-blue
// axis, tint is the magenta-green axis); the seventh (`smoothness`) is a
// dither hint — see dither.c for the snap-to-pure-ink behaviour.
typedef struct {
    int   brightness;   // -100 .. 100  (default 0)
    int   contrast;     //    0 .. 200  (default 100, percent)
    int   saturation;   //    0 .. 200  (default 100, percent)
    int   gamma;        //   20 .. 300  (default 100, percent → gamma = value/100)
    int   temperature;  //   50 .. 150  (default 100, percent → 1.0 = neutral; warm > 100, cool < 100)
    int   tint;         //   50 .. 150  (default 100, percent → 1.0 = neutral; magenta > 100, green < 100)
    int   smoothness;   //    0 .. 100  (default  30) — see dither.c
} adjust_cfg_t;

void color_pipeline_init(void);          // builds sRGB→linear LUT, etc.
void adjust_cfg_default(adjust_cfg_t *o);

// In-place colour adjustment over an RGB888 row-major buffer.
// rgb buffer size = w * h * 3.
void apply_adjust_rgb888(uint8_t *rgb, int w, int h, const adjust_cfg_t *cfg);

// EINK-aware local enhancement before palette quantisation.  This is a small
// deterministic analogue of a learned enhancement block: local contrast,
// edge-aware sharpening, and chroma separation tuned for a six-ink reflective
// display. Returns 0 on success, -1 if scratch allocation fails.
int enhance_eink_rgb888(uint8_t *rgb, int w, int h);

// One-shot sRGB-uint8 → CIELAB-float (D65).  Uses the linearisation LUT.
void rgb_to_lab_u8(uint8_t r, uint8_t g, uint8_t b,
                   float *L, float *a, float *b_out);

// Float sRGB ([0,1]) → CIELAB.  Same maths, slower path used by tests.
void rgb_to_lab_f(float r, float g, float b, float *L, float *a, float *b_out);

// CIEDE2000 distance between two LAB triples.
float ciede2000(float L1, float a1, float b1,
                float L2, float a2, float b2);

#ifdef __cplusplus
}
#endif

#endif

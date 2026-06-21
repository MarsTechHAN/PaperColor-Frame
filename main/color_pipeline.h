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

// Colour-rendering mode — selects the dither's colour model.
//   6COLOR   (default) — match only the six primary inks, as the pipeline did
//                        before the 28-patch halftone-mix calibration was
//                        added (commit 29e9b69). Cleaner, lighter, less
//                        oversaturated; the dither ignores the mix model.
//   MIXPATCH (experimental) — also match the 28 measured halftone-mix patches
//                        (richer, more saturated colour). This is the current
//                        behaviour, demoted to an opt-in Experimental choice.
//   REFLECTANCE (experimental, "v2") — the first-principles redesign. Error
//                        diffusion runs in pseudo-reflectance (p = XYZ^(1/n),
//                        n≈1.4) so a halftone neighbourhood converges to the
//                        correct *optical* (Yule–Nielsen) mean instead of the
//                        physically-wrong linear-Lab mean; the local refine
//                        scores against the same YN reflectance model; tone is
//                        a display-referred lift (no cusp-bend that throws away
//                        the bright yellow ink); and the pre-enhance stack is
//                        softened (no chroma boost, gentler clarity, lighter
//                        post-resample sharpen). Source-side chroma compression
//                        (0.44×) is unchanged, so out-of-gamut residual for the
//                        diffuser is preserved (see docs 2026-06-17 redesign).
// 6COLOR/MIXPATCH gate only the dither's USE of the mix model; REFLECTANCE
// additionally changes the error-diffusion colour space and the tone curve.
typedef enum {
    COLOR_RENDER_6COLOR      = 0,
    COLOR_RENDER_MIXPATCH    = 1,
    COLOR_RENDER_REFLECTANCE = 2,
} color_render_mode_t;

void color_pipeline_init(void);          // builds sRGB→linear LUT, etc.
void adjust_cfg_default(adjust_cfg_t *o);

void                color_render_set_mode(color_render_mode_t mode);
color_render_mode_t color_render_get_mode(void);

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

// CIELAB ↔ XYZ (D65) — reflectance-domain helpers for the v2 (REFLECTANCE)
// renderer.  XYZ here is the diffuse tristimulus that optical dot-mixing is
// (approximately) linear in; pseudo-reflectance p = XYZ^(1/n) is then linear
// under the Yule–Nielsen halftone model.
void lab_to_xyz(float L, float a, float b, float *X, float *Y, float *Z);
void xyz_to_lab(float X, float Y, float Z, float *L, float *a, float *b_out);

// sRGB ↔ linear-light helpers (for gamma-correct resampling). The forward path
// uses the same 256-entry LUT as rgb_to_lab; the inverse encodes [0,1]→0..255.
float   color_srgb_to_linear(uint8_t c);   // 0..255 → linear 0..1
uint8_t color_linear_to_srgb(float lin);   // linear 0..1 → 0..255

#ifdef __cplusplus
}
#endif

#endif

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

// Pre-computed CIELAB (D65) the dither picker compares against.  Filled by
// palette_init() at boot from the colorimeter's measured Lab readings with a
// constant XYZ specular-floor subtraction (SCI→SCE correction) so the
// in-memory ink anchors approximate what the eye sees through the AG glass,
// not what the colorimeter integrated under d/8 SCI.
extern float PALETTE_LAB[PALETTE_N][3];

// Per-ink pseudo-reflectance p = XYZ^(1/n) for the v2 (reflectance) renderer.
// Built by palette_init() from PALETTE_LAB.  Optical dot-mixing is linear in p
// under the Yule–Nielsen model: p_mix = Σ wᵢ·pᵢ, appearance XYZ = p_mix^n.
// Diffusing error in p-space makes a halftone converge to the correct *optical*
// mean rather than the physically-wrong linear-Lab mean.
#define PALETTE_YN_N 1.4f   // re-fit from the 28 measured mix patches (≈1.4–1.45)
extern float PALETTE_REFL_P[PALETTE_N][3];

// Yule–Nielsen reflectance mix → predicted Lab of a dot population.
// weights need not be normalised (they are normalised internally).
void palette_mix_model_yn(const float weights[PALETTE_N], float out_lab[3]);

// Panel chroma ceiling (cusp model) — max C* the panel can hold at target
// lightness L* for the hue of (a,b). Built from the measured 6 inks + mix
// patches. Drives the v2 chroma gamut-map so saturation rises to the cusp but
// never past it (no bleaching). See panel_build_cusp in palette.c.
float panel_chroma_ceiling(float L, float a, float b);

// Lightness of the chroma cusp for the hue of (a,b) — where this hue is most
// saturated (yellow ~L63, red ~L36, blue/green ~L41). The v2 map pulls
// saturated colours toward it so they render vivid instead of dull.
float panel_cusp_lightness(float a, float b);

void palette_init(void);

// Colorimetry pipeline mode.  LEGACY reproduces the pre-2026-05 behavior
// bit-exact: PALETTE_LAB comes from rgb_to_lab_u8(PALETTE_RGB_MEASURED), and
// mix-patch Lab comes from rgb_to_lab_u8(MIX_DEFAULTS.rgb).  SCE_* modes apply
// a constant XYZ specular-floor subtraction to the colorimeter's measured
// Lab to approximate the SCE (in-use) appearance through AG glass — see
// lab_apply_sce in palette.c.  Currently scaffolded as an experimental
// opt-in; default is LEGACY for both firmware and WASM.
typedef enum {
    PALETTE_MODE_LEGACY      = 0,  // rgb_to_lab(PALETTE_RGB_MEASURED) — pre-SCE behavior
    PALETTE_MODE_SCE_OFF     = 1,  // CSV-baked Lab directly (k=0, no specular subtraction)
    PALETTE_MODE_SCE_SUBTLE  = 2,  // SCE k ≈ 0.02
    PALETTE_MODE_SCE_DEFAULT = 3,  // SCE k ≈ 0.04
    PALETTE_MODE_SCE_STRONG  = 4,  // SCE k ≈ 0.06
} palette_mode_t;

void palette_set_mode(palette_mode_t mode);
palette_mode_t palette_get_mode(void);

// Low-level specular-floor knob — exposed mainly for console A/B tuning.
// Setting this only takes effect in SCE_* modes; LEGACY ignores k entirely.
// Range 0..0.08 (clamped).
void palette_set_specular_floor(float k);
float palette_get_specular_floor(void);

// Measured mixed-patch calibration. The order matches the non-solid entries
// in CALIB_TARGETS in the web UI so browser calibration can update the model
// without string IDs crossing the WASM ABI.
#define PALETTE_MIX_PATCH_N 28

typedef struct {
    uint8_t weights[PALETTE_N];  // Percent weights in PIDX_* order.
    float   lab[3];             // Measured appearance of this halftone patch.
    uint8_t enabled;            // 0 when the user has no reading for it.
} palette_mix_patch_t;

extern palette_mix_patch_t PALETTE_MIX_PATCHES[PALETTE_MIX_PATCH_N];

void palette_mix_reset_defaults(void);
void palette_mix_clear(void);
void palette_mix_set_patch_lab(int patch_idx, const float lab[3]);
void palette_mix_rebuild_model(void);
int  palette_mix_enabled_count(void);
void palette_mix_model_lab(const float weights[PALETTE_N], float out_lab[3]);

#endif

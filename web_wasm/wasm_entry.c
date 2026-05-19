// wasm_entry.c — single-export shim around color_pipeline + dither + palette.
//
// Memory layout the JS caller is expected to use:
//
//   inputs:
//     rgb        — w*h*3 bytes, RGB888 row-major  (typically 400*600*3 = 720000)
//     palette_lab — 18 floats  (6 colours × {L,a,b})  — calibrated panel LABs
//     cfg        — adjust_cfg_t struct (5 ints)
//
//   outputs:
//     packed     — (w*h+1)/2  bytes — 4-bit ink codes (top nibble = even pixel)
//
// The JS wrapper allocates these buffers in the WASM heap with malloc(), calls
// wasm_dither(...), then reads `packed` out.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "color_pipeline.h"
#include "dither.h"
#include "palette.h"

#define EXPORT __attribute__((used)) __attribute__((visibility("default")))

// Read the current dither row from JS.  Lets a debug client poll the WASM
// while a dither is running on a worker (or stuck) and see progress.
extern volatile int dither_progress_row;
EXPORT int wasm_dither_progress(void) { return dither_progress_row; }

// Single-pixel CIEDE2000 sanity exposure.  Returns the LAB distance from
// (Lp1, ap1, bp1) to (Lp2, ap2, bp2).  Lets the JS-side smoke test verify
// that the math reaches a terminating answer.
EXPORT float wasm_ciede2000(float L1, float a1, float b1,
                            float L2, float a2, float b2)
{
    return ciede2000(L1, a1, b1, L2, a2, b2);
}

// Process one pixel through the full sRGB→LAB pipeline.  Same purpose as
// wasm_ciede2000 — keeps the test reach-set small while still hitting
// rgb_to_lab_u8 + ciede2000.  Writes 3 floats to out_lab3.
EXPORT void wasm_rgb_to_lab(uint8_t r, uint8_t g, uint8_t b, float *out_lab3)
{
    rgb_to_lab_u8(r, g, b, &out_lab3[0], &out_lab3[1], &out_lab3[2]);
}

// Defined in palette.c — we mutate it from here.
extern float PALETTE_LAB[PALETTE_N][3];

// Called once after the module loads.
EXPORT void wasm_init(void)
{
    color_pipeline_init();
    palette_init();
}

// Overwrite PALETTE_LAB with calibrated values.  Pass 18 floats in
// PIDX_BLACK, WHITE, YELLOW, RED, BLUE, GREEN order.
EXPORT void wasm_set_palette_lab(const float *lab18)
{
    if (!lab18) return;
    memcpy(PALETTE_LAB, lab18, sizeof(float) * PALETTE_N * 3);
}

// Full pipeline: adjust → dither → packed 4bpp.
//
// Returns 0 on success, -1 on error.  Caller must size `packed` to
// (w*h+1)/2 bytes.  `indices_opt` (optional, may be 0) receives w*h raw
// palette indices (0..5), used by the browser-side live preview to colour
// the thumbnail with the calibrated panel RGB.
EXPORT int wasm_dither(uint8_t *rgb, int w, int h,
                       const adjust_cfg_t *cfg,
                       uint8_t *packed,
                       uint8_t *indices_opt)
{
    if (!rgb || !packed || w <= 0 || h <= 0) return -1;
    int smoothness = cfg ? cfg->smoothness : 30;
    if (cfg) apply_adjust_rgb888(rgb, w, h, cfg);
    enhance_eink_rgb888(rgb, w, h);
    return dither_ved_fs(rgb, w, h, packed, indices_opt, smoothness);
}

// Flat-fill poster pipeline used by the "Russian" preset.  Same adjustment and
// enhancement stages as the normal preview, but final quantisation is pure ink
// assignment with no error diffusion.
EXPORT int wasm_flat_fill(uint8_t *rgb, int w, int h,
                          const adjust_cfg_t *cfg,
                          uint8_t *packed,
                          uint8_t *indices_opt)
{
    if (!rgb || !packed || w <= 0 || h <= 0) return -1;
    if (cfg) apply_adjust_rgb888(rgb, w, h, cfg);
    enhance_eink_rgb888(rgb, w, h);
    return flat_fill_constructivist(rgb, w, h, packed, indices_opt);
}

// 1.5× supersampled pipeline.  Source is rgb_15x at (panel_w*3/2, panel_h*3/2);
// output panel-grid framebuffer is panel_w × panel_h packed 4bpp.  The adjust
// pass runs on the 1.5× buffer (same colour math, more pixels), then the
// dither does linear-light area averaging per panel pixel.  See
// dither.c::dither_ved_fs_15x for the rationale.
EXPORT int wasm_dither_15x(uint8_t *rgb_15x, int panel_w, int panel_h,
                           const adjust_cfg_t *cfg,
                           uint8_t *packed,
                           uint8_t *indices_opt)
{
    if (!rgb_15x || !packed || panel_w <= 0 || panel_h <= 0) return -1;
    int src_w = panel_w * 3 / 2;
    int src_h = panel_h * 3 / 2;
    int smoothness = cfg ? cfg->smoothness : 30;
    if (cfg) apply_adjust_rgb888(rgb_15x, src_w, src_h, cfg);
    enhance_eink_rgb888(rgb_15x, src_w, src_h);
    return dither_ved_fs_15x(rgb_15x, src_w, src_h, panel_w, panel_h,
                             packed, indices_opt, smoothness);
}

EXPORT int wasm_flat_fill_15x(uint8_t *rgb_15x, int panel_w, int panel_h,
                              const adjust_cfg_t *cfg,
                              uint8_t *packed,
                              uint8_t *indices_opt)
{
    if (!rgb_15x || !packed || panel_w <= 0 || panel_h <= 0) return -1;
    int src_w = panel_w * 3 / 2;
    int src_h = panel_h * 3 / 2;
    if (cfg) apply_adjust_rgb888(rgb_15x, src_w, src_h, cfg);
    enhance_eink_rgb888(rgb_15x, src_w, src_h);
    return flat_fill_constructivist_15x(rgb_15x, src_w, src_h, panel_w, panel_h,
                                        packed, indices_opt);
}

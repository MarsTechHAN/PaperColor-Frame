#ifndef PAPER_E6_DITHER_H
#define PAPER_E6_DITHER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Vector-error Floyd-Steinberg dither in CIELAB with CIEDE2000 distance.
//
// in_rgb888    : w*h*3 bytes (row-major)
// out_packed   : (w*h+1)/2  bytes — output 4-bit ink codes (high nibble = even
//                pixel, low nibble = odd pixel) in PALETTE_INK_CODE ordering.
// out_idx_opt  : if non-NULL, w*h bytes — receives raw palette indices 0..5
//                (useful for preview / debug).  May be NULL.
//
// Both output buffers may safely live in PSRAM.  The function allocates
// 2*w*3*4 bytes of internal SRAM for the rolling error buffer.
//
// Returns 0 on success, -1 on allocation failure.
// `smoothness` (0..100) controls solid-fill snapping in the dither.  When a
// pixel's residual ΔE2000 (after the palette pick) falls below the threshold
// derived from this value, error diffusion is attenuated to zero — the dither
// "snaps" to the chosen ink without propagating speckle into neighbouring
// pixels.  The mapping is:
//
//   threshold_ΔE = smoothness * 0.2   (so 30 → ΔE=6, 100 → ΔE=20)
//
// A linear ramp from 0.5*threshold to threshold softens the transition so
// near-snap regions don't form hard boundaries against neighbouring dithered
// regions.  `smoothness=0` disables the feature (pure FS).  Default is 30.
int dither_ved_fs(const uint8_t *in_rgb888, int w, int h,
                  uint8_t *out_packed,
                  uint8_t *out_idx_opt,
                  int smoothness);

// Supersampled variant: source is exactly 1.5× the panel size in each axis
// (e.g. 600×900 in, 400×600 out).  Each panel pixel decision averages a
// 1.5×1.5 block of source pixels (4 cells with weights summing to 2.25) in
// linear sRGB before quantising, so the dither sees finer chroma transitions
// than the canvas-resize-to-panel pipeline can deliver.  Floyd-Steinberg
// error is still propagated on the panel grid; that's where the visible
// artefacts live, so a finer error grid wouldn't help.
//
// `smoothness` carries the same meaning as in dither_ved_fs above.
// src_w / src_h must equal panel_w * 3 / 2 and panel_h * 3 / 2 respectively.
// Returns 0 on success.
int dither_ved_fs_15x(const uint8_t *src_rgb, int src_w, int src_h,
                      int panel_w, int panel_h,
                      uint8_t *out_packed,
                      uint8_t *out_idx_opt,
                      int smoothness);

// Pure flat-colour quantiser for the "Russian" preset.  This path deliberately
// disables error diffusion: every panel pixel is one physical ink, then a small
// edge-aware mode cleanup removes isolated speckles so the result reads like
// constructivist poster blocks rather than a halftone.
int flat_fill_constructivist(const uint8_t *in_rgb888, int w, int h,
                             uint8_t *out_packed,
                             uint8_t *out_idx_opt);

// 1.5× supersampled variant used by the browser Send path.
int flat_fill_constructivist_15x(const uint8_t *src_rgb, int src_w, int src_h,
                                 int panel_w, int panel_h,
                                 uint8_t *out_packed,
                                 uint8_t *out_idx_opt);

#ifdef __cplusplus
}
#endif

#endif

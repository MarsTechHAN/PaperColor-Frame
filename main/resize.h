#ifndef PAPER_E6_RESIZE_H
#define PAPER_E6_RESIZE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// In-place rotation by 90° counter-clockwise (matches PIL Image.rotate(90, expand=True)).
// Allocates a new buffer in PSRAM and returns it via *out_rgb; the caller is
// responsible for freeing the OLD buffer (kept on input) AND the new buffer.
//   *in_rgb   : pointer to input pixels (read-only).  Not freed.
//   in_w/in_h : input dimensions.
//   out_rgb   : receives newly-allocated pixel buffer; out_w/out_h updated.
// Returns 0 on success.
int rotate_90_ccw(const uint8_t *in_rgb, int in_w, int in_h,
                  uint8_t **out_rgb, int *out_w, int *out_h);

// Lanczos-2 separable resampling.  Output buffer is allocated in PSRAM.
//   in_rgb/in_w/in_h     : input (read-only)
//   out_rgb/out_w/out_h  : caller specifies the target dimensions, function
//                          allocates the buffer in PSRAM and writes pixels.
// Returns 0 on success.
int resize_lanczos2(const uint8_t *in_rgb,  int in_w,  int in_h,
                    uint8_t      **out_rgb, int out_w, int out_h);

// Full Python resize_crop port:
//   * Auto-rotate to portrait orientation
//   * Lanczos2 fill (max scale) so target rect is fully covered
//   * Centre crop to TARGET_W x TARGET_H (400 x 600)
// Allocates output in PSRAM.  Caller frees input.
//
// Returns 0 on success; *was_rotated reports whether portrait rotation was
// applied.
int resize_crop_to_400x600(const uint8_t *in_rgb, int in_w, int in_h,
                           uint8_t **out_rgb,
                           bool *was_rotated);

#define TARGET_W 400
#define TARGET_H 600

#ifdef __cplusplus
}
#endif

#endif

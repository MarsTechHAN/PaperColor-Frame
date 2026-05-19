#ifndef PAPER_E6_IMAGE_LOADER_H
#define PAPER_E6_IMAGE_LOADER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Caller-owned RGB888 image buffer, row-major, no padding.
typedef struct {
    uint8_t *pixels;  // heap_caps_aligned in PSRAM
    int      width;
    int      height;
} rgb888_t;

// Decode any supported file (JPG/JPEG/BMP).  Detection is by filename
// extension.  Returns 0 on success.  On success caller must free out->pixels
// with heap_caps_free / free.
int image_load_from_file(const char *path, rgb888_t *out);

// Decoders, exposed so dither.c tests can stub if needed.
int decode_bmp_file (const char *path, rgb888_t *out);
int decode_jpeg_file(const char *path, rgb888_t *out);

void rgb888_free(rgb888_t *img);

#ifdef __cplusplus
}
#endif

#endif

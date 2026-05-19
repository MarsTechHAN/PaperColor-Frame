// jpeg_decoder.c — JPEG decoding via the ROM TJpgDec.
//
// Wraps the ROM-resident tjpgd library exposed at known symbol addresses on
// ESP32-S3.  We do a two-call dance:
//
//   1. jd_prepare(): read SOF0 to learn (w,h).  Pick the smallest power-of-two
//      down-scale (1, 1/2, 1/4, 1/8) that keeps both dimensions ≤ MAX_DIM.
//      This prevents 12MP camera JPEGs from blowing PSRAM.
//
//   2. jd_decomp(): stream MCU blocks into our callback, which copies RGB888
//      into the PSRAM-resident output image at the rect's offset.
//
// TJpgDec needs a working pool of about 3 KB; we put it on the calling task's
// stack (size_t pool aligned).  We also keep a 512-byte stream buffer (the
// JD_SZBUF default).
//
// The library produces RGB888 in scan order, top-to-bottom — no flipping
// required.

#include "image_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp32s3/rom/tjpgd.h"

static const char *TAG = "jpeg";

#define JPEG_POOL_SIZE   3100
#define MAX_DIM          1200

typedef struct {
    FILE     *fp;
    uint8_t  *out_rgb;
    int       out_w;
    int       out_h;
} jpeg_ctx_t;

static UINT in_func(JDEC *jd, BYTE *buff, UINT nbyte)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    if (buff) {
        return (UINT)fread(buff, 1, nbyte, c->fp);
    } else {
        // Skip nbyte bytes.
        return fseek(c->fp, nbyte, SEEK_CUR) == 0 ? nbyte : 0;
    }
}

static UINT out_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    const BYTE *src = (const BYTE *)bitmap;
    int rect_w = rect->right - rect->left + 1;
    int rect_h = rect->bottom - rect->top + 1;
    for (int y = 0; y < rect_h; y++) {
        int dst_y = rect->top + y;
        if (dst_y >= c->out_h) break;
        uint8_t *dst = c->out_rgb + (dst_y * c->out_w + rect->left) * 3;
        int copy_w = rect_w;
        if (rect->left + copy_w > c->out_w) copy_w = c->out_w - rect->left;
        memcpy(dst, src + y * rect_w * 3, (size_t)copy_w * 3);
    }
    return 1;
}

int decode_jpeg_file(const char *path, rgb888_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen %s failed", path);
        return -1;
    }

    void *pool = malloc(JPEG_POOL_SIZE);
    if (!pool) {
        fclose(fp);
        return -1;
    }

    JDEC jdec;
    jpeg_ctx_t ctx = { .fp = fp };
    JRESULT jr = jd_prepare(&jdec, in_func, pool, JPEG_POOL_SIZE, &ctx);
    if (jr != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare: %d", (int)jr);
        free(pool);
        fclose(fp);
        return -1;
    }

    // Pick smallest scale that fits.
    int scale = 0;
    int sw = jdec.width;
    int sh = jdec.height;
    while ((sw > MAX_DIM || sh > MAX_DIM) && scale < 3) {
        scale++;
        sw = jdec.width  >> scale;
        sh = jdec.height >> scale;
    }
    ESP_LOGI(TAG, "JPEG %dx%d → scale 1/%d → %dx%d", jdec.width, jdec.height,
             1 << scale, sw, sh);

    uint8_t *rgb = heap_caps_aligned_alloc(16, (size_t)sw * sh * 3,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb) {
        ESP_LOGE(TAG, "PSRAM alloc failed for %dx%d", sw, sh);
        free(pool);
        fclose(fp);
        return -1;
    }
    ctx.out_rgb = rgb;
    ctx.out_w   = sw;
    ctx.out_h   = sh;

    jr = jd_decomp(&jdec, out_func, (BYTE)scale);
    free(pool);
    fclose(fp);

    if (jr != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp: %d", (int)jr);
        heap_caps_free(rgb);
        return -1;
    }

    out->pixels = rgb;
    out->width  = sw;
    out->height = sh;
    return 0;
}

// bmp_decoder.c — minimal 24-bit BI_RGB / 32-bit BI_RGB (BGRA) BMP reader.
//
// We only target the simplest BMPs likely to land on an SD card — typical
// camera/screenshot output is 24-bit uncompressed.  Compressed BMPs, indexed
// palettes, 16-bit RGB565, and OS/2 v1 headers are not supported.
//
// Output rgb888_t->pixels is allocated in PSRAM and rows are written top-to-
// bottom regardless of BMP storage order (BMP rows are bottom-up by default).

#include "image_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "bmp";

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;          // 'BM'
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;       // offset to pixel data
} bmp_file_header_t;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} bmp_info_header_t;
#pragma pack(pop)

int decode_bmp_file(const char *path, rgb888_t *out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen %s failed", path);
        return -1;
    }

    bmp_file_header_t bfh;
    bmp_info_header_t bih;
    if (fread(&bfh, 1, sizeof bfh, fp) != sizeof bfh ||
        fread(&bih, 1, sizeof bih, fp) != sizeof bih) {
        ESP_LOGE(TAG, "header read failed");
        fclose(fp);
        return -1;
    }
    if (bfh.bfType != 0x4D42) {
        ESP_LOGE(TAG, "not a BMP (magic=0x%04x)", bfh.bfType);
        fclose(fp);
        return -1;
    }
    if (bih.biCompression != 0) {
        ESP_LOGE(TAG, "compressed BMP not supported (compr=%lu)", (unsigned long)bih.biCompression);
        fclose(fp);
        return -1;
    }
    if (bih.biBitCount != 24 && bih.biBitCount != 32) {
        ESP_LOGE(TAG, "only 24/32-bit BMP supported (bpp=%u)", bih.biBitCount);
        fclose(fp);
        return -1;
    }

    int w = bih.biWidth;
    int h_signed = bih.biHeight;
    bool flip = h_signed > 0;        // positive height = bottom-up rows
    int h = h_signed < 0 ? -h_signed : h_signed;
    if (w <= 0 || h <= 0) {
        ESP_LOGE(TAG, "bad dimensions %dx%d", w, h_signed);
        fclose(fp);
        return -1;
    }

    int bpp = bih.biBitCount / 8;
    int row_stride = (w * bpp + 3) & ~3; // each row padded to 4-byte boundary

    if (fseek(fp, bfh.bfOffBits, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "seek to pixels failed");
        fclose(fp);
        return -1;
    }

    uint8_t *rgb = heap_caps_aligned_alloc(16, (size_t)w * h * 3,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb) {
        ESP_LOGE(TAG, "PSRAM alloc failed for %dx%d", w, h);
        fclose(fp);
        return -1;
    }

    uint8_t *row = malloc(row_stride);
    if (!row) {
        heap_caps_free(rgb);
        fclose(fp);
        return -1;
    }

    for (int y = 0; y < h; y++) {
        if (fread(row, 1, row_stride, fp) != (size_t)row_stride) {
            ESP_LOGE(TAG, "pixel read failed at row %d", y);
            free(row);
            heap_caps_free(rgb);
            fclose(fp);
            return -1;
        }
        int dst_y = flip ? (h - 1 - y) : y;
        uint8_t *dst = rgb + dst_y * w * 3;
        // BMP is BGR(A); convert to RGB.
        for (int x = 0; x < w; x++) {
            dst[x * 3 + 0] = row[x * bpp + 2]; // R
            dst[x * 3 + 1] = row[x * bpp + 1]; // G
            dst[x * 3 + 2] = row[x * bpp + 0]; // B
        }
    }
    free(row);
    fclose(fp);

    out->pixels = rgb;
    out->width  = w;
    out->height = h;
    ESP_LOGI(TAG, "decoded %s : %dx%d", path, w, h);
    return 0;
}

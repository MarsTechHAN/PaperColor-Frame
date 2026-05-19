// image_loader.c — file-extension dispatch for JPEG/BMP decoding.

#include "image_loader.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "imgload";

static bool ends_with_ci(const char *s, const char *suf)
{
    size_t ls = strlen(s), lsu = strlen(suf);
    if (ls < lsu) return false;
    return strcasecmp(s + ls - lsu, suf) == 0;
}

void rgb888_free(rgb888_t *img)
{
    if (img && img->pixels) {
        heap_caps_free(img->pixels);
        img->pixels = NULL;
    }
}

int image_load_from_file(const char *path, rgb888_t *out)
{
    memset(out, 0, sizeof(*out));
    if (ends_with_ci(path, ".bmp")) {
        return decode_bmp_file(path, out);
    }
    if (ends_with_ci(path, ".jpg") || ends_with_ci(path, ".jpeg")) {
        return decode_jpeg_file(path, out);
    }
    ESP_LOGE(TAG, "unsupported file extension: %s", path);
    return -1;
}

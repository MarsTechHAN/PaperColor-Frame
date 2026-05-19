// Tiny shims so we can compile color_pipeline.c / dither.c / palette.c on the
// host with stock clang/gcc — without dragging in the ESP-IDF.

#ifndef HOST_TEST_STUBS_H
#define HOST_TEST_STUBS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESP_LOGI(tag, fmt, ...) do { fprintf(stderr, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_8BIT     0

static inline void *heap_caps_calloc(size_t n, size_t sz, int caps) { (void)caps; return calloc(n, sz); }
static inline void *heap_caps_malloc(size_t sz, int caps)           { (void)caps; return malloc(sz); }
static inline void *heap_caps_aligned_alloc(size_t align, size_t sz, int caps)
{
    (void)caps; (void)align;
    return malloc(sz);
}
static inline void  heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(int caps) { (void)caps; return 0; }

#endif

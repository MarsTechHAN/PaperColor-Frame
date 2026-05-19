// loop_display.c — Single-consumer EPD task.  All display work is serialised
// here so concurrent HTTP requests can't double-trigger a panel refresh (the
// E6 refresh is ~30 s and not reentrant).
//
// Behaviour:
//   * A queue of length 4 holds pending display commands.
//   * If no command arrives within `config_get_loop_interval_s()` the task
//     synthesises a CMD_NEXT (auto-advance).
//   * On every refresh we update photo_store's "active" marker so the UI
//     highlights the currently-displayed thumbnail.

#include "loop_display.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "photo_store.h"
#include "config_store.h"
#include "image_loader.h"
#include "resize.h"
#include "color_pipeline.h"
#include "dither.h"
#include "epd_4in0e.h"
#include "palette.h"
#include "board_pins.h"

static const char *TAG = "loop_display";

typedef enum {
    CMD_NEXT     = 1,
    CMD_SHOW_OPT = 2,
    CMD_FILL_INK = 3,
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    char       name[PHOTO_NAME_MAX];
    uint8_t    ink_code;     // valid when kind == CMD_FILL_INK
} display_cmd_t;

static QueueHandle_t      s_q          = NULL;
static SemaphoreHandle_t  s_idle_sem   = NULL;
static int                s_auto_index = 0;

static const adjust_cfg_t k_default_adjust = {
    .brightness  = 0,
    .contrast    = 104,
    .saturation  = 112,
    .gamma       = 98,
    .temperature = 108,
    .tint        = 98,
    .smoothness  = 12,
};

// Fast path — read a pre-dithered 4bpp framebuffer straight from disk and
// push it to the panel.  Used when the browser-side WASM pipeline has already
// produced /sdcard/photos/<base>.bin.
static int run_bin_pipeline(const char *bin_path)
{
    int64_t t0 = esp_timer_get_time();
    FILE *fp = fopen(bin_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "open bin %s: errno=%d", bin_path, errno);
        return -1;
    }
    size_t fb_bytes = (size_t)(EPD_4IN0E_WIDTH / 2) * EPD_4IN0E_HEIGHT;
    uint8_t *fb = heap_caps_aligned_alloc(16, fb_bytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        ESP_LOGE(TAG, "no PSRAM for bin FB");
        fclose(fp);
        return -1;
    }
    size_t got = fread(fb, 1, fb_bytes, fp);
    fclose(fp);
    if (got != fb_bytes) {
        ESP_LOGE(TAG, "bin %s: short read %u/%u", bin_path,
                 (unsigned)got, (unsigned)fb_bytes);
        heap_caps_free(fb);
        return -1;
    }
    ESP_LOGI(TAG, "bin read %lldms", (esp_timer_get_time() - t0) / 1000);
    t0 = esp_timer_get_time();
    epd_display(fb);
    ESP_LOGI(TAG, "epd_disp %lldms", (esp_timer_get_time() - t0) / 1000);
    heap_caps_free(fb);
    return 0;
}

static int run_pipeline(const char *path)
{
    rgb888_t img = {0};
    int64_t t0 = esp_timer_get_time();
    if (image_load_from_file(path, &img) != 0) {
        ESP_LOGE(TAG, "decode failed: %s", path);
        return -1;
    }
    ESP_LOGI(TAG, "decode  %lldms (%dx%d)",
             (esp_timer_get_time()-t0)/1000, img.width, img.height);

    uint8_t *resized = NULL;
    bool rotated = false;
    t0 = esp_timer_get_time();
    if (resize_crop_to_400x600(img.pixels, img.width, img.height,
                               &resized, &rotated) != 0) {
        rgb888_free(&img);
        return -1;
    }
    rgb888_free(&img);
    ESP_LOGI(TAG, "resize  %lldms (rotated=%d)",
             (esp_timer_get_time()-t0)/1000, rotated);

    apply_adjust_rgb888(resized, TARGET_W, TARGET_H, &k_default_adjust);
    enhance_eink_rgb888(resized, TARGET_W, TARGET_H);

    size_t fb_bytes = (size_t)(TARGET_W / 2) * TARGET_H;
    uint8_t *fb = heap_caps_aligned_alloc(16, fb_bytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        ESP_LOGE(TAG, "no PSRAM for FB");
        heap_caps_free(resized);
        return -1;
    }
    t0 = esp_timer_get_time();
    int dr = dither_ved_fs(resized, TARGET_W, TARGET_H, fb, NULL,
                            k_default_adjust.smoothness);
    heap_caps_free(resized);
    if (dr != 0) {
        heap_caps_free(fb);
        return -1;
    }
    ESP_LOGI(TAG, "dither  %lldms", (esp_timer_get_time()-t0)/1000);

    t0 = esp_timer_get_time();
    epd_display(fb);
    ESP_LOGI(TAG, "epd_disp %lldms", (esp_timer_get_time()-t0)/1000);
    heap_caps_free(fb);
    return 0;
}

static int resolve_next(char *out_name, size_t out_sz)
{
    photo_meta_t list[64];
    int n = photo_store_list(list, 64);
    if (n <= 0) return -1;
    if (s_auto_index >= n) s_auto_index = 0;
    // Skip the current active photo so "Next" actually moves forward when
    // we boot mid-cycle.
    const char *cur = photo_store_get_active();
    if (cur && *cur && n > 1 && strcmp(list[s_auto_index].name, cur) == 0) {
        s_auto_index = (s_auto_index + 1) % n;
    }
    strncpy(out_name, list[s_auto_index].name, out_sz - 1);
    out_name[out_sz - 1] = 0;
    s_auto_index = (s_auto_index + 1) % n;
    return 0;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        display_cmd_t cmd = {0};
        uint32_t int_s = config_get_loop_interval_s();
        // Take the semaphore while we wait for work — `wait_idle` reads it.
        xSemaphoreGive(s_idle_sem);
        BaseType_t got = xQueueReceive(s_q, &cmd, pdMS_TO_TICKS((uint32_t)int_s * 1000U));
        xSemaphoreTake(s_idle_sem, 0);

        if (got != pdTRUE) {
            cmd.kind = CMD_NEXT;
            cmd.name[0] = 0;
        }

        if (cmd.kind == CMD_FILL_INK) {
            ESP_LOGI(TAG, "fill ink 0x%x", cmd.ink_code);
            epd_clear(cmd.ink_code);
            // Calibration fills are not "active photos"; clear the marker
            // so the gallery UI doesn't keep one highlighted.
            photo_store_set_active(NULL);
            continue;
        }

        char name[PHOTO_NAME_MAX];
        if (cmd.kind == CMD_SHOW_OPT && cmd.name[0]) {
            strncpy(name, cmd.name, sizeof name - 1);
            name[sizeof name - 1] = 0;
        } else {
            if (resolve_next(name, sizeof name) != 0) {
                ESP_LOGW(TAG, "no photos on SD — sleeping");
                continue;
            }
        }

        ESP_LOGI(TAG, "displaying %s", name);

        // Fast path: if the browser-side WASM pipeline produced a pre-
        // dithered .bin sibling, push it straight to the panel.  Falls back
        // to decode → resize → dither for legacy .jpg uploads.
        int rc = -1;
        if (photo_store_has_bin(name)) {
            char bin_path[PHOTO_PATH_MAX + 32];
            if (photo_store_bin_path_for(name, bin_path, sizeof bin_path) == 0) {
                rc = run_bin_pipeline(bin_path);
            }
        }
        if (rc != 0) {
            char path[PHOTO_PATH_MAX + 32];
            if (photo_store_path_for(name, path, sizeof path) != 0) {
                ESP_LOGE(TAG, "bad name: %s", name);
                continue;
            }
            rc = run_pipeline(path);
        }
        if (rc == 0) {
            photo_store_set_active(name);
        }
    }
}

int loop_display_start(void)
{
    if (s_q) return 0;
    s_q = xQueueCreate(4, sizeof(display_cmd_t));
    s_idle_sem = xSemaphoreCreateBinary();
    if (!s_q || !s_idle_sem) return -1;
    // 8 KB overflows on the JPEG-decode fallback path (jpeg + resize +
    // adjust + dither stack frames stack up). 24 KB gives comfortable
    // headroom; the .bin fast path uses far less but the stack is shared.
    BaseType_t ok = xTaskCreate(worker, "epd_worker", 24576, NULL,
                                tskIDLE_PRIORITY + 3, NULL);
    return ok == pdPASS ? 0 : -1;
}

int loop_display_request_show(const char *name)
{
    if (!s_q || !name) return -1;
    display_cmd_t c = { .kind = CMD_SHOW_OPT };
    strncpy(c.name, name, sizeof c.name - 1);
    return xQueueSend(s_q, &c, 0) == pdTRUE ? 0 : -1;
}

int loop_display_request_next(void)
{
    if (!s_q) return -1;
    display_cmd_t c = { .kind = CMD_NEXT };
    return xQueueSend(s_q, &c, 0) == pdTRUE ? 0 : -1;
}

int loop_display_request_fill(uint8_t ink_code)
{
    if (!s_q) return -1;
    display_cmd_t c = { .kind = CMD_FILL_INK, .ink_code = ink_code };
    return xQueueSend(s_q, &c, 0) == pdTRUE ? 0 : -1;
}

void loop_display_wait_idle(void)
{
    if (!s_idle_sem) return;
    xSemaphoreTake(s_idle_sem, portMAX_DELAY);
    xSemaphoreGive(s_idle_sem);
}

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
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "photo_store.h"
#include "sd_storage.h"
#include "config_store.h"
#include "image_loader.h"
#include "resize.h"
#include "color_pipeline.h"
#include "dither.h"
#include "epd_4in0e.h"
#include "palette.h"
#include "board_pins.h"
#include "pm1.h"
#include "battery_overlay.h"
#include "welcome_overlay.h"
#include "wifi_ap.h"

static const char *TAG = "loop_display";

// Embedded welcome panels: pre-dithered 400x600 4bpp framebuffers that the
// device shows when the user's photo library is empty. Each side-button press
// in that state toggles between zh and en.
extern const uint8_t _binary_welcome_zh_bin_start[] asm("_binary_welcome_zh_bin_start");
extern const uint8_t _binary_welcome_zh_bin_end[]   asm("_binary_welcome_zh_bin_end");
extern const uint8_t _binary_welcome_en_bin_start[] asm("_binary_welcome_en_bin_start");
extern const uint8_t _binary_welcome_en_bin_end[]   asm("_binary_welcome_en_bin_end");

static int s_welcome_lang = 0;  // 0 = zh, 1 = en

typedef enum {
    CMD_NEXT     = 1,
    CMD_SHOW_OPT = 2,
    CMD_FILL_INK = 3,
    CMD_MIX_PATCH = 4,
    CMD_WELCOME  = 5,
    CMD_BOOT_SPLASH = 6,
} cmd_kind_t;

// Power-on first-screen splash: paint the welcome card, dwell this long, then
// advance to the first stored photo (if any). Long enough to read the on-screen
// Wi-Fi credentials before the panel switches to a photo.
#define BOOT_SPLASH_HOLD_MS 15000

typedef struct {
    cmd_kind_t kind;
    char       name[PHOTO_NAME_MAX];
    uint8_t    ink_code;     // valid when kind == CMD_FILL_INK
    uint8_t    mix_n;        // valid when kind == CMD_MIX_PATCH
    uint8_t    mix_ink[6];
    uint8_t    mix_weight[6];
} display_cmd_t;

static QueueHandle_t      s_q          = NULL;
static int                s_auto_index = 0;
static volatile bool      s_worker_idle = false;

static const adjust_cfg_t k_default_adjust = {
    .brightness  = 0,
    .contrast    = 104,
    .saturation  = 142,
    .gamma       = 98,
    .temperature = 108,
    .tint        = 98,
    .smoothness  = 12,
};

static int panel_power_on(void)
{
    // The EPD and SD rails share one SPI bus and are kept powered together for
    // the whole awake session (see project_spi_power_coupling). Assert both
    // rails on — idempotent, a no-op write when already up — then re-init the
    // panel controller, which the previous refresh left in POWER_OFF (0x02)
    // idle (NOT deep sleep — that wedged the soft-reset wake).
    //
    // epd_init(false): let the EPD driver *ensure* the bus is up rather than
    // assume sd_storage_mount(true) already did it. spi_bus_initialize() is
    // idempotent (returns ESP_ERR_INVALID_STATE when already inited, which
    // epd_init tolerates), so this is a no-op on the normal SD path but
    // correctly brings the bus up in the LittleFS-fallback case where the SD
    // mount — and thus its bus init — never succeeded. A failed init returns
    // -1 so display_fb can power-cycle.
    (void)pm1_set_epd_power(true);
    (void)pm1_set_sd_power(true);
    if (epd_init(false) != 0) {
        ESP_LOGE(TAG, "EPD init failed");
        return -1;
    }
    return 0;
}

static void panel_power_off(void)
{
    // Do NOT issue the controller deep-sleep (0x07/0xA5) here. The rail stays
    // powered for the whole awake session (it is shared with SD), and a
    // controller parked in deep sleep does NOT reliably wake from the next
    // refresh's *soft* reset — BUSY then stays low and the panel wedges
    // (observed: refresh #1 OK, refresh #2 "BUSY stuck low 30000 ms"). After
    // epd_turn_on()'s POWER_OFF (0x02) the controller is already in a low-power
    // idle state that the next epd_init() reset reliably revives. True low-power
    // shutdown happens on ESP deep sleep, which cuts both rails entirely
    // (pm1_prepare_deep_sleep) for a clean cold boot on wake — epd_sleep() is
    // issued there if needed, not per refresh.
}

// ---------------------------------------------------------- panel recovery
//
// Fallback when the EPD controller wedges (BUSY never releases). A soft reset
// has already proven insufficient in that state, so we hard power-cycle the
// shared EPD+SD rail and cold-init. The store is unmounted first and remounted
// after (mirroring the boot order) so a card-backed filesystem survives the
// rail drop; the internal LittleFS store lives in flash and is unaffected by
// the rail but is cycled too for uniformity. Rare path — logged loudly.
static int panel_remount_store(void)
{
    // Bus stays initialised across the rail cycle (we only gate power, never
    // spi_bus_free), so remount/re-init pass "bus already inited".
    //
    // Re-mount the SAME backend the session booted with — do NOT silently
    // re-home the store. Switching an SD-backed session to LittleFS here would
    // (a) hide the user's SD library for the rest of the session, (b) let
    // sd_storage_mount_internal() format the flash partition, and (c) skip the
    // mkdir of "<mount>/photos" — so uploads/list would land in a nonexistent
    // dir. We detect the boot backend from the current mount point.
    bool was_sd = strncmp(photo_store_dir(), "/sdcard", 7) == 0;
    if (was_sd) {
        // The rail just dropped; a slow card may need a couple of tries to
        // come back. Retry rather than fall through to flash.
        for (int attempt = 0; attempt < 3; attempt++) {
            if (sd_storage_mount(false) == 0) {
                photo_store_set_mount_point("/sdcard");
                photo_store_init();        // ensure /sdcard/photos exists
                return 0;
            }
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        ESP_LOGE(TAG, "recovery: SD remount failed after retries; "
                      "keeping /sdcard (store degraded until reboot)");
        return -1;   // stay SD; do not orphan the library onto flash
    }
    // LittleFS-backed session: remount flash.
    if (sd_storage_mount_internal() == 0 &&
        photo_store_set_mount_point("/littlefs") == 0) {
        photo_store_init();                // ensure /littlefs/photos exists
        return 0;
    }
    return -1;
}

static int panel_hard_recover(void)
{
    ESP_LOGW(TAG, "EPD wedged — hard power-cycle recovery");

    // Close the gate on new httpd file I/O and wait for any in-flight read /
    // write to finish before we unmount and cut the shared rail. The httpd task
    // can be mid-fopen/fread/fwrite on this very filesystem; unmounting under it
    // would free the VFS/littlefs state it is using (use-after-free). If the
    // in-flight ops don't drain in time, ABORT recovery — drop this frame and
    // let the next command retry — rather than tear the FS down unsafely.
    if (!sd_storage_recover_begin(8000)) {
        ESP_LOGE(TAG, "recovery: FS busy, deferring power-cycle to next attempt");
        sd_storage_recover_end();
        return -1;
    }

    epd_prepare_power_off();          // park DC/RST low before the rail drops
    sd_storage_unmount();             // release the FS before cutting SD power

    // Rails must move together (shared bus); drop both, then restore both.
    (void)pm1_set_epd_power(false);
    (void)pm1_set_sd_power(false);
    vTaskDelay(pdMS_TO_TICKS(250));   // let the panel DC-DC fully discharge
    (void)pm1_set_epd_power(true);
    (void)pm1_set_sd_power(true);
    vTaskDelay(pdMS_TO_TICKS(300));   // SD power-good + rail settle

    if (panel_remount_store() != 0) {
        ESP_LOGE(TAG, "recovery: store remount failed (FS may be degraded)");
    }
    sd_storage_recover_end();         // re-open the gate for httpd FS ops

    if (epd_init(false) != 0) {       // cold init from a truly powered-off state
        ESP_LOGE(TAG, "recovery: EPD re-init still failing");
        return -1;
    }
    ESP_LOGW(TAG, "EPD recovery: re-init OK");
    return 0;
}

static uint8_t read_battery_percent(void)
{
    static uint8_t last_percent = 100;
    uint16_t mv = 0;
    if (pm1_read_battery_mv(&mv) == 0 && mv > 0) {
        last_percent = pm1_battery_percent_from_mv(mv);
        ESP_LOGI(TAG, "battery %umV => %u%%", (unsigned)mv, (unsigned)last_percent);
    }
    return last_percent;
}

static int display_fb(uint8_t *fb, bool draw_battery)
{
    if (draw_battery && config_get_battery_icon_enabled()) {
        battery_overlay_draw(fb, read_battery_percent());
    }
    int64_t t0 = esp_timer_get_time();

    // Normal path: power on (re-inits the controller) then push + refresh.
    int rc = panel_power_on();
    if (rc == 0) rc = epd_display(fb);

    // Fallback (兜底): a wedged controller (BUSY never released, at init or
    // mid-refresh) — hard power-cycle the rail and retry once. If it still
    // fails, drop this frame but keep the worker alive so HTTP keeps serving
    // and the next image can try again, instead of stalling 30 s per command.
    if (rc != 0) {
        if (panel_hard_recover() != 0 || epd_display(fb) != 0) {
            ESP_LOGE(TAG, "epd refresh failed after recovery — skipping frame");
            return -1;
        }
    }

    panel_power_off();
    ESP_LOGI(TAG, "epd_disp %lldms", (esp_timer_get_time() - t0) / 1000);
    return 0;
}

static inline void fb_pack(uint8_t *fb, int x, int y, uint8_t ink_code)
{
    size_t byte_idx = ((size_t)y * EPD_4IN0E_WIDTH + x) >> 1;
    if ((x & 1) == 0) {
        fb[byte_idx] = (fb[byte_idx] & 0x0F) | ((ink_code & 0x0F) << 4);
    } else {
        fb[byte_idx] = (fb[byte_idx] & 0xF0) | (ink_code & 0x0F);
    }
}

static uint32_t calib_hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

#define CALIB_TILE 16

typedef struct {
    uint32_t h;
    uint8_t cell;
} calib_rank_item_t;

static int calib_rank_cmp(const void *a, const void *b)
{
    const calib_rank_item_t *ia = (const calib_rank_item_t *)a;
    const calib_rank_item_t *ib = (const calib_rank_item_t *)b;
    if (ia->h < ib->h) return -1;
    if (ia->h > ib->h) return 1;
    return (int)ia->cell - (int)ib->cell;
}

static uint8_t choose_mix_ink(uint16_t rank, uint16_t cells,
                              const uint8_t *ink, const uint8_t *weight,
                              uint8_t n, uint16_t total)
{
    uint16_t t = (uint32_t)rank * total / cells;
    uint16_t acc = 0;
    for (uint8_t i = 0; i < n; i++) {
        acc += weight[i];
        if (t < acc) return ink[i];
    }
    return ink[n - 1];
}

static int run_mix_patch(const uint8_t *ink, const uint8_t *weight, uint8_t n)
{
    if (!ink || !weight || n == 0 || n > 6) return -1;
    uint16_t total = 0;
    for (uint8_t i = 0; i < n; i++) total += weight[i];
    if (total == 0) return -1;

    size_t fb_bytes = (size_t)(EPD_4IN0E_WIDTH / 2) * EPD_4IN0E_HEIGHT;
    uint8_t *fb = heap_caps_aligned_alloc(16, fb_bytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        fb = heap_caps_aligned_alloc(16, fb_bytes,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!fb) return -1;
    memset(fb, 0, fb_bytes);

    for (int ty = 0; ty < EPD_4IN0E_HEIGHT; ty += CALIB_TILE) {
        for (int tx = 0; tx < EPD_4IN0E_WIDTH; tx += CALIB_TILE) {
            const int tw = (tx + CALIB_TILE <= EPD_4IN0E_WIDTH)
                         ? CALIB_TILE : (EPD_4IN0E_WIDTH - tx);
            const int th = (ty + CALIB_TILE <= EPD_4IN0E_HEIGHT)
                         ? CALIB_TILE : (EPD_4IN0E_HEIGHT - ty);
            const uint16_t cells = (uint16_t)(tw * th);
            calib_rank_item_t order[CALIB_TILE * CALIB_TILE];
            uint8_t rank_by_cell[CALIB_TILE * CALIB_TILE];

            // Calibration patches are measured by a colorimeter, so low-
            // frequency periodic screens are more harmful than fine grain.
            // Build a unique hashed permutation per 16x16 tile: every tile has
            // nearly exact ink ratios, but no Bayer rows/columns to read as
            // bands in the aperture.
            uint32_t salt = calib_hash32((uint32_t)(tx / CALIB_TILE) * 0x9e3779b9u
                                       ^ (uint32_t)(ty / CALIB_TILE) * 0x85ebca6bu
                                       ^ (uint32_t)n * 0xc2b2ae35u
                                       ^ (uint32_t)total);
            for (uint16_t cy = 0, cell = 0; cy < th; cy++) {
                for (uint16_t cx = 0; cx < tw; cx++, cell++) {
                    uint32_t key = salt
                                 ^ ((uint32_t)(tx + cx) * 0x27d4eb2du)
                                 ^ ((uint32_t)(ty + cy) * 0x165667b1u)
                                 ^ ((uint32_t)cell * 0xd3a2646cu);
                    order[cell].h = calib_hash32(key);
                    order[cell].cell = (uint8_t)cell;
                }
            }
            qsort(order, cells, sizeof(order[0]), calib_rank_cmp);
            for (uint16_t r = 0; r < cells; r++) {
                rank_by_cell[order[r].cell] = (uint8_t)r;
            }

            for (uint16_t cy = 0, cell = 0; cy < th; cy++) {
                for (uint16_t cx = 0; cx < tw; cx++, cell++) {
                    uint8_t chosen = choose_mix_ink(rank_by_cell[cell], cells,
                                                    ink, weight, n, total);
                    fb_pack(fb, tx + cx, ty + cy, chosen);
                }
            }
        }
    }

    int rc = display_fb(fb, false);
    heap_caps_free(fb);
    return rc;
}

// First-run welcome — paint the embedded zh/en welcome panel and overlay the
// per-device SSID and password. Called from the worker when photo_store has
// nothing to show; lang flips on every invocation so consecutive side-button
// presses cycle the language without requiring a separate command type.
static int run_welcome(int lang)
{
    const uint8_t *src;
    size_t src_len;
    if (lang == 1) {
        src = _binary_welcome_en_bin_start;
        src_len = (size_t)(_binary_welcome_en_bin_end - _binary_welcome_en_bin_start);
    } else {
        src = _binary_welcome_zh_bin_start;
        src_len = (size_t)(_binary_welcome_zh_bin_end - _binary_welcome_zh_bin_start);
    }
    size_t fb_bytes = (size_t)(EPD_4IN0E_WIDTH / 2) * EPD_4IN0E_HEIGHT;
    if (src_len != fb_bytes) {
        ESP_LOGE(TAG, "welcome bin size %u != %u",
                 (unsigned)src_len, (unsigned)fb_bytes);
        return -1;
    }
    uint8_t *fb = heap_caps_aligned_alloc(16, fb_bytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        fb = heap_caps_aligned_alloc(16, fb_bytes,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!fb) return -1;
    memcpy(fb, src, fb_bytes);
    welcome_overlay_draw(fb, lang,
                         config_get_wifi_ap_ssid(),
                         config_get_wifi_ap_password());
    int rc = display_fb(fb, false);
    heap_caps_free(fb);
    return rc;
}

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
    int rc = display_fb(fb, true);
    heap_caps_free(fb);
    return rc;
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
    int dr = dither_e6_mix_fs(resized, TARGET_W, TARGET_H, fb, NULL,
                              k_default_adjust.smoothness);
    heap_caps_free(resized);
    if (dr != 0) {
        heap_caps_free(fb);
        return -1;
    }
    ESP_LOGI(TAG, "dither  %lldms", (esp_timer_get_time()-t0)/1000);

    int disp_rc = display_fb(fb, true);
    heap_caps_free(fb);
    return disp_rc;
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
        TickType_t wait_ticks = int_s == CONFIG_LOOP_INTERVAL_NEVER_S
                                    ? portMAX_DELAY
                                    : pdMS_TO_TICKS(int_s * 1000U);
        s_worker_idle = true;
        BaseType_t got = xQueueReceive(s_q, &cmd, wait_ticks);
        s_worker_idle = false;

        if (got != pdTRUE) {
            // With nobody connected, periodic refreshes are driven by deep
            // sleep timer wakeups instead of burning power while awake.
            if (wifi_ap_station_count() <= 0) {
                continue;
            }
            cmd.kind = CMD_NEXT;
            cmd.name[0] = 0;
        }

        if (cmd.kind == CMD_FILL_INK) {
            ESP_LOGI(TAG, "fill ink 0x%x", cmd.ink_code);
            if (panel_power_on() == 0) {
                epd_clear(cmd.ink_code);
                panel_power_off();
            }
            // Calibration fills are not "active photos"; clear the marker
            // so the gallery UI doesn't keep one highlighted.
            photo_store_set_active(NULL);
            continue;
        }

        if (cmd.kind == CMD_MIX_PATCH) {
            ESP_LOGI(TAG, "calib mix patch n=%u", (unsigned)cmd.mix_n);
            (void)run_mix_patch(cmd.mix_ink, cmd.mix_weight, cmd.mix_n);
            photo_store_set_active(NULL);
            continue;
        }

        if (cmd.kind == CMD_WELCOME) {
            // Long-press recovery: show credentials even when the library is
            // non-empty. Don't flip s_welcome_lang — the user is holding the
            // button to read the AP info, not to cycle languages.
            ESP_LOGI(TAG, "forced welcome (lang=%d)", s_welcome_lang);
            (void)run_welcome(s_welcome_lang);
            photo_store_set_active(NULL);
            continue;
        }

        if (cmd.kind == CMD_BOOT_SPLASH) {
            // Power-on first screen: paint the welcome card (shows the per-device
            // Wi-Fi credentials), dwell ~15 s, then advance to the first stored
            // photo. Don't flip s_welcome_lang — this is a splash, not a toggle.
            ESP_LOGI(TAG, "boot splash (welcome lang=%d)", s_welcome_lang);
            (void)run_welcome(s_welcome_lang);
            photo_store_set_active(NULL);

            photo_meta_t one;
            bool have_photos = photo_store_list(&one, 1) > 0;
            if (!have_photos) {
                // Empty library: leave the welcome card up, same as before.
                continue;
            }
            // Hold the first screen, but bail early if a web/side-button command
            // shows up so the user's choice wins instead of waiting out the dwell
            // and then doing a redundant auto-advance.
            bool preempted = false;
            for (int waited = 0; waited < BOOT_SPLASH_HOLD_MS; waited += 200) {
                if (uxQueueMessagesWaiting(s_q) > 0) { preempted = true; break; }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            // No command queued during the dwell: advance to the first photo via
            // the normal NEXT path (active was just cleared, so it shows list[0]).
            if (!preempted && uxQueueMessagesWaiting(s_q) == 0) {
                (void)loop_display_request_next();
            }
            continue;
        }

        char name[PHOTO_NAME_MAX];
        if (cmd.kind == CMD_SHOW_OPT && cmd.name[0]) {
            strncpy(name, cmd.name, sizeof name - 1);
            name[sizeof name - 1] = 0;
        } else {
            if (resolve_next(name, sizeof name) != 0) {
                // Empty library: paint the bundled welcome card and flip the
                // language so the next side-button press lands on the other
                // localisation. This is the only state where ▲▼ doesn't
                // advance through photos.
                int lang = s_welcome_lang;
                s_welcome_lang = (s_welcome_lang + 1) % 2;
                ESP_LOGI(TAG, "no photos — showing welcome (lang=%d)", lang);
                (void)run_welcome(lang);
                photo_store_set_active(NULL);
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
        if (rc == 0 && uxQueueMessagesWaiting(s_q) == 0) {
            photo_store_set_active(name);
        }
    }
}

int loop_display_start(void)
{
    if (s_q) return 0;
    s_q = xQueueCreate(4, sizeof(display_cmd_t));
    if (!s_q) return -1;
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
    // Explicit "Show Now" should override stale queued Next/Show commands.
    // The current refresh is not interrupted; this only clears pending work.
    xQueueReset(s_q);
    if (xQueueSend(s_q, &c, 0) != pdTRUE) return -1;
    photo_store_set_active(name);
    return 0;
}

int loop_display_request_next(void)
{
    if (!s_q) return -1;
    display_cmd_t c = { .kind = CMD_NEXT };
    return xQueueSend(s_q, &c, 0) == pdTRUE ? 0 : -1;
}

int loop_display_request_welcome(void)
{
    if (!s_q) return -1;
    display_cmd_t c = { .kind = CMD_WELCOME };
    return xQueueSend(s_q, &c, 0) == pdTRUE ? 0 : -1;
}

int loop_display_request_boot_splash(void)
{
    if (!s_q) return -1;
    display_cmd_t c = { .kind = CMD_BOOT_SPLASH };
    return xQueueSend(s_q, &c, 0) == pdTRUE ? 0 : -1;
}

int loop_display_request_fill(uint8_t ink_code)
{
    if (!s_q) return -1;
    display_cmd_t c = { .kind = CMD_FILL_INK, .ink_code = ink_code };
    return xQueueSend(s_q, &c, 0) == pdTRUE ? 0 : -1;
}

int loop_display_request_mix(const uint8_t *ink_codes,
                             const uint8_t *weights,
                             uint8_t n)
{
    if (!s_q || !ink_codes || !weights || n == 0 || n > 6) return -1;
    display_cmd_t c = { .kind = CMD_MIX_PATCH, .mix_n = n };
    memcpy(c.mix_ink, ink_codes, n);
    memcpy(c.mix_weight, weights, n);
    return xQueueSend(s_q, &c, 0) == pdTRUE ? 0 : -1;
}

void loop_display_wait_idle(void)
{
    while (!loop_display_is_idle()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool loop_display_wait_idle_timeout(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!loop_display_is_idle()) {
        if (waited >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    return true;
}

bool loop_display_is_idle(void)
{
    if (!s_q) return true;
    if (uxQueueMessagesWaiting(s_q) > 0) return false;
    return s_worker_idle;
}

// resize.c — Lanczos-2 separable resampling + 90° rotate + Python-equivalent
// resize_crop pipeline.
//
// Why Lanczos-2 specifically:
//   * Python uses PIL Image.LANCZOS, which is a Lanczos kernel with a=3 by
//     default in Pillow.  a=3 is sharper but ~50% slower per pixel and the
//     subjective difference after we throw the result at a 6-colour dither is
//     negligible.  Lanczos-2 keeps texture detail without the perceptible
//     overshoot artefacts of bicubic, at ~half the kernel taps.
//
//   * Separable horizontal-then-vertical pass means O(W_out * H_in * 4) +
//     O(W_out * H_out * 4) instead of O(W_out * H_out * 16).
//
//   * The horizontal pass writes into a float32 intermediate (PSRAM) so the
//     vertical pass doesn't clamp twice.
//
// Memory: for 400×600 output and ≤1200×1200 input, intermediate is
// 400 × 1200 × 3 × 4 = 5.4 MB.  That'd blow PSRAM together with the source
// (≤4.3 MB) + destination (720 KB).  So the intermediate is stored as
// int16_t fixed-point Q8, halving to ~2.7 MB.  Q8 (1/256 LSB) is well below
// the visual noise floor of a 6-colour panel.

#include "resize.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "resize";

// ----------------------------------------------------------------- rotation

int rotate_90_ccw(const uint8_t *in_rgb, int in_w, int in_h,
                  uint8_t **out_rgb, int *out_w, int *out_h)
{
    int W = in_h, H = in_w;
    uint8_t *dst = heap_caps_aligned_alloc(16, (size_t)W * H * 3,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) {
        ESP_LOGE(TAG, "no PSRAM for rotation %dx%d", W, H);
        return -1;
    }
    // PIL Image.rotate(90, expand=True): CCW.  (x, y) in input maps to
    // (y, W-1-x) in output.
    for (int y = 0; y < in_h; y++) {
        const uint8_t *src_row = in_rgb + y * in_w * 3;
        for (int x = 0; x < in_w; x++) {
            int dx = y;
            int dy = (in_w - 1 - x);
            uint8_t *d = dst + (dy * W + dx) * 3;
            d[0] = src_row[x * 3 + 0];
            d[1] = src_row[x * 3 + 1];
            d[2] = src_row[x * 3 + 2];
        }
    }
    *out_rgb = dst;
    *out_w   = W;
    *out_h   = H;
    return 0;
}

// --------------------------------------------------------------- lanczos kernel

static inline float sinc_pi(float x)
{
    if (x == 0.0f) return 1.0f;
    float xpi = (float)M_PI * x;
    return sinf(xpi) / xpi;
}

static inline float lanczos2(float x)
{
    if (x < 0.f) x = -x;
    if (x >= 2.0f) return 0.0f;
    return sinc_pi(x) * sinc_pi(x * 0.5f);
}

// For a target axis size out_n and source axis size in_n, build a contribution
// table: for each output sample, the list of (src_index, weight) pairs.
//
// We use the standard "shift by half a pixel and divide by scale" sampling.
// support_radius is in *source* pixels when upscaling, and grows when down-
// scaling so the low-pass band is wide enough.
typedef struct {
    int16_t  *idx;   // out_n * taps_per_out
    float    *w;     // out_n * taps_per_out  (sums to ~1 per output sample)
    int       taps;  // taps per output
} resample_plan_t;

static int build_plan(int in_n, int out_n, resample_plan_t *plan)
{
    const float scale = (float)out_n / (float)in_n;
    const float inv_scale = 1.0f / scale;
    // When downsampling, expand the support so we don't alias.
    const float support = (scale < 1.0f) ? (2.0f * inv_scale) : 2.0f;
    const int   taps    = (int)(2.0f * support + 0.5f) + 1;

    plan->taps = taps;
    plan->idx = malloc((size_t)out_n * taps * sizeof(int16_t));
    plan->w   = malloc((size_t)out_n * taps * sizeof(float));
    if (!plan->idx || !plan->w) {
        free(plan->idx);
        free(plan->w);
        return -1;
    }

    const float filter_scale = (scale < 1.0f) ? scale : 1.0f;

    for (int o = 0; o < out_n; o++) {
        // Centre of output sample mapped to source space.
        float c = (o + 0.5f) * inv_scale - 0.5f;
        int   left = (int)floorf(c - support + 0.5f);

        int16_t *p_idx = plan->idx + o * taps;
        float   *p_w   = plan->w   + o * taps;
        float    wsum  = 0.0f;

        for (int k = 0; k < taps; k++) {
            int s = left + k;
            int cs = s < 0 ? 0 : (s >= in_n ? in_n - 1 : s);  // clamp-to-edge
            float t = ((s + 0.5f) - (c + 0.5f)) * filter_scale;
            float w = lanczos2(t);
            p_idx[k] = (int16_t)cs;
            p_w[k]   = w;
            wsum    += w;
        }
        if (wsum != 0.0f) {
            float inv = 1.0f / wsum;
            for (int k = 0; k < taps; k++) p_w[k] *= inv;
        }
    }
    return 0;
}

static void free_plan(resample_plan_t *p)
{
    free(p->idx);
    free(p->w);
    p->idx = NULL;
    p->w = NULL;
}

// --------------------------------------------------------------- main resize

int resize_lanczos2(const uint8_t *in_rgb, int in_w, int in_h,
                    uint8_t **out_rgb, int out_w, int out_h)
{
    if (in_w == out_w && in_h == out_h) {
        uint8_t *dst = heap_caps_aligned_alloc(16, (size_t)out_w * out_h * 3,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!dst) return -1;
        memcpy(dst, in_rgb, (size_t)out_w * out_h * 3);
        *out_rgb = dst;
        return 0;
    }

    resample_plan_t hp = {0}, vp = {0};
    if (build_plan(in_w, out_w, &hp) != 0) return -1;
    if (build_plan(in_h, out_h, &vp) != 0) { free_plan(&hp); return -1; }

    // Intermediate buffer: width=out_w, height=in_h, 3 channels, int16 Q8.
    size_t mid_bytes = (size_t)out_w * in_h * 3 * sizeof(int16_t);
    int16_t *mid = heap_caps_aligned_alloc(16, mid_bytes,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mid) {
        ESP_LOGE(TAG, "no PSRAM for intermediate %u bytes", (unsigned)mid_bytes);
        free_plan(&hp); free_plan(&vp);
        return -1;
    }

    // Horizontal pass: for each input row, for each output column, sum taps.
    for (int y = 0; y < in_h; y++) {
        const uint8_t *src_row = in_rgb + y * in_w * 3;
        int16_t       *mid_row = mid + y * out_w * 3;
        for (int x = 0; x < out_w; x++) {
            const int16_t *idx = hp.idx + x * hp.taps;
            const float   *w   = hp.w   + x * hp.taps;
            float r = 0, g = 0, b = 0;
            for (int k = 0; k < hp.taps; k++) {
                const uint8_t *s = src_row + idx[k] * 3;
                r += w[k] * s[0];
                g += w[k] * s[1];
                b += w[k] * s[2];
            }
            int16_t *m = mid_row + x * 3;
            // Q8: multiply by 256, clamp to int16 range.
            int ir = (int)(r * 256.0f + 0.5f);
            int ig = (int)(g * 256.0f + 0.5f);
            int ib = (int)(b * 256.0f + 0.5f);
            if (ir < -32768) ir = -32768; else if (ir > 32767) ir = 32767;
            if (ig < -32768) ig = -32768; else if (ig > 32767) ig = 32767;
            if (ib < -32768) ib = -32768; else if (ib > 32767) ib = 32767;
            m[0] = (int16_t)ir;
            m[1] = (int16_t)ig;
            m[2] = (int16_t)ib;
        }
    }

    uint8_t *dst = heap_caps_aligned_alloc(16, (size_t)out_w * out_h * 3,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) {
        ESP_LOGE(TAG, "no PSRAM for output %dx%d", out_w, out_h);
        heap_caps_free(mid);
        free_plan(&hp); free_plan(&vp);
        return -1;
    }

    // Vertical pass.
    for (int y = 0; y < out_h; y++) {
        const int16_t *idx = vp.idx + y * vp.taps;
        const float   *w   = vp.w   + y * vp.taps;
        uint8_t *dst_row = dst + y * out_w * 3;
        for (int x = 0; x < out_w; x++) {
            float r = 0, g = 0, b = 0;
            for (int k = 0; k < vp.taps; k++) {
                const int16_t *m = mid + (idx[k] * out_w + x) * 3;
                r += w[k] * m[0];
                g += w[k] * m[1];
                b += w[k] * m[2];
            }
            r *= (1.0f / 256.0f);
            g *= (1.0f / 256.0f);
            b *= (1.0f / 256.0f);
            int ir = (int)(r + 0.5f);
            int ig = (int)(g + 0.5f);
            int ib = (int)(b + 0.5f);
            if (ir < 0) ir = 0; else if (ir > 255) ir = 255;
            if (ig < 0) ig = 0; else if (ig > 255) ig = 255;
            if (ib < 0) ib = 0; else if (ib > 255) ib = 255;
            dst_row[x * 3 + 0] = (uint8_t)ir;
            dst_row[x * 3 + 1] = (uint8_t)ig;
            dst_row[x * 3 + 2] = (uint8_t)ib;
        }
    }

    heap_caps_free(mid);
    free_plan(&hp); free_plan(&vp);
    *out_rgb = dst;
    return 0;
}

// --------------------------------------------------------------- resize_crop

// Crop helper: copy a sub-rectangle of `src` (size sw×sh) starting at
// (left, top) of size (cw, ch) into a freshly-allocated PSRAM buffer.
static uint8_t *crop_rect(const uint8_t *src, int sw, int sh,
                          int left, int top, int cw, int ch)
{
    (void)sh;
    uint8_t *dst = heap_caps_aligned_alloc(16, (size_t)cw * ch * 3,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) return NULL;
    for (int y = 0; y < ch; y++) {
        memcpy(dst + y * cw * 3,
               src + ((top + y) * sw + left) * 3,
               (size_t)cw * 3);
    }
    return dst;
}

int resize_crop_to_400x600(const uint8_t *in_rgb, int in_w, int in_h,
                           uint8_t **out_rgb, bool *was_rotated)
{
    uint8_t *rotated = NULL;
    int rw = in_w, rh = in_h;
    const uint8_t *src = in_rgb;

    *was_rotated = false;
    if (in_w > in_h) {
        if (rotate_90_ccw(in_rgb, in_w, in_h, &rotated, &rw, &rh) != 0) {
            return -1;
        }
        src = rotated;
        *was_rotated = true;
    }

    float scale_w = (float)TARGET_W / rw;
    float scale_h = (float)TARGET_H / rh;
    float scale   = scale_w > scale_h ? scale_w : scale_h;

    int new_w = (int)(rw * scale + 0.5f);
    int new_h = (int)(rh * scale + 0.5f);
    if (new_w < TARGET_W) new_w = TARGET_W;
    if (new_h < TARGET_H) new_h = TARGET_H;

    uint8_t *scaled = NULL;
    int rc = resize_lanczos2(src, rw, rh, &scaled, new_w, new_h);
    if (rotated) heap_caps_free(rotated);
    if (rc != 0) return rc;

    int left = (new_w - TARGET_W) / 2;
    int top  = (new_h - TARGET_H) / 2;
    uint8_t *cropped = crop_rect(scaled, new_w, new_h, left, top, TARGET_W, TARGET_H);
    heap_caps_free(scaled);
    if (!cropped) return -1;

    *out_rgb = cropped;
    return 0;
}

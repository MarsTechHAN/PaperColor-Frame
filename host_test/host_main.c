// host_main.c — drive color_pipeline.c + dither.c on a desktop, compare with
// the Python reference output.
//
// Modes:
//   ./host_test path/to/demo_rgb888.h            # uses the header's embedded array
//   ./host_test --raw path/to/file.raw           # raw 400x600 RGB888 bytes
//
// Always writes:
//   out_preview.bmp      — 6-colour preview using the measured palette RGB
//   out_indices.bin      — 400x600 palette indices (0..5), uint8 per pixel
//
// The indices file can be diffed against a Python-side `code_idx.tobytes()`
// for a per-pixel comparison.

#include "stubs.h"
#include "../main/color_pipeline.h"
#include "../main/dither.h"
#include "../main/palette.h"

#include <ctype.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#define TW 400
#define TH 600

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// Parser for ../demo_rgb888.h:  picks every "0xHH," byte after the array name.
static uint8_t *load_rgb888_header(const char *path, size_t *out_n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *txt = malloc(sz + 1);
    fread(txt, 1, sz, fp);
    txt[sz] = 0;
    fclose(fp);

    size_t cap = TW * TH * 3;
    uint8_t *buf = malloc(cap);
    size_t n = 0;

    const char *p = strstr(txt, "demoRGB888");
    if (!p) p = strstr(txt, "{");
    if (!p) { free(txt); free(buf); return NULL; }
    while ((p = strstr(p, "0x")) != NULL && n < cap) {
        p += 2;
        unsigned v = 0;
        int got = 0;
        while (isxdigit((unsigned char)*p) && got < 2) {
            v = (v << 4) | (isdigit((unsigned char)*p)
                              ? (*p - '0')
                              : ((tolower((unsigned char)*p) - 'a') + 10));
            p++;
            got++;
        }
        if (got > 0) buf[n++] = (uint8_t)v;
    }
    free(txt);
    *out_n = n;
    return buf;
}

static uint8_t *load_raw(const char *path, size_t *out_n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    fread(buf, 1, sz, fp);
    fclose(fp);
    *out_n = (size_t)sz;
    return buf;
}

// Write a 24-bit BI_RGB BMP (top-down via negative height) so we can open it
// in any viewer.
static void write_bmp(const char *path, const uint8_t *rgb, int w, int h)
{
    int row_stride = (w * 3 + 3) & ~3;
    int pad        = row_stride - w * 3;
    long pixels    = (long)row_stride * h;
    long file_sz   = 54 + pixels;

    FILE *fp = fopen(path, "wb");
    if (!fp) { perror(path); return; }

    uint8_t bf[14] = {0};
    bf[0] = 'B'; bf[1] = 'M';
    bf[2] = file_sz & 0xff; bf[3] = (file_sz>>8)&0xff;
    bf[4] = (file_sz>>16)&0xff; bf[5] = (file_sz>>24)&0xff;
    bf[10] = 54;
    fwrite(bf, 1, 14, fp);

    int32_t W = w, H = h;
    uint8_t bi[40] = {0};
    bi[0] = 40;
    memcpy(bi + 4, &W, 4);
    int32_t Hneg = -H;
    memcpy(bi + 8, &Hneg, 4);
    bi[12] = 1;
    bi[14] = 24;
    fwrite(bi, 1, 40, fp);

    uint8_t zero[4] = {0};
    for (int y = 0; y < h; y++) {
        const uint8_t *src = rgb + y * w * 3;
        for (int x = 0; x < w; x++) {
            uint8_t bgr[3] = { src[x*3 + 2], src[x*3 + 1], src[x*3 + 0] };
            fwrite(bgr, 1, 3, fp);
        }
        if (pad) fwrite(zero, 1, pad, fp);
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s [--raw] <input>\n", argv[0]);
        return 1;
    }
    int raw_mode = 0;
    int no_adjust = 0;
    int russian = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0)        raw_mode = 1;
        else if (strcmp(argv[i], "--no-adjust") == 0) no_adjust = 1;
        else if (strcmp(argv[i], "--russian") == 0) russian = 1;
        else                                       path = argv[i];
    }
    if (!path) { fprintf(stderr, "no input path given\n"); return 1; }

    size_t n = 0;
    uint8_t *rgb = raw_mode ? load_raw(path, &n) : load_rgb888_header(path, &n);
    if (!rgb) return 1;
    if (n < (size_t)TW * TH * 3) {
        fprintf(stderr, "input too small: %zu bytes (need %d)\n", n, TW*TH*3);
        free(rgb);
        return 1;
    }

    color_pipeline_init();
    palette_init();

    fprintf(stderr, "palette LAB:\n");
    static const char *NM[] = {"black","white","yellow","red","blue","green"};
    for (int i = 0; i < PALETTE_N; i++) {
        fprintf(stderr, "  %-7s rgb=(%3d,%3d,%3d) lab=(%6.2f,%6.2f,%6.2f)\n",
                NM[i],
                PALETTE_RGB_MEASURED[i][0],
                PALETTE_RGB_MEASURED[i][1],
                PALETTE_RGB_MEASURED[i][2],
                PALETTE_LAB[i][0], PALETTE_LAB[i][1], PALETTE_LAB[i][2]);
    }

    adjust_cfg_t cfg = {0};
    adjust_cfg_default(&cfg);
    cfg.contrast    = 104;
    cfg.saturation  = 112;
    cfg.gamma       = 98;
    cfg.temperature = 108;
    cfg.tint        = 98;
    cfg.smoothness  = 12;
    if (russian) {
        cfg.brightness  = 1;
        cfg.contrast    = 123;
        cfg.saturation  = 88;
        cfg.gamma       = 91;
        cfg.temperature = 104;
        cfg.tint        = 98;
        cfg.smoothness  = 100;
    }

    double t0 = now_ms();
    if (!no_adjust) {
        apply_adjust_rgb888(rgb, TW, TH, &cfg);
        enhance_eink_rgb888(rgb, TW, TH);
    }
    double t1 = now_ms();
    fprintf(stderr, "[time] adjust : %.1f ms%s\n", t1 - t0, no_adjust ? " (skipped)" : "");

    uint8_t *packed   = calloc(1, (size_t)TW * TH / 2);
    uint8_t *idx_buf  = calloc(1, (size_t)TW * TH);
    int rc = russian ? flat_fill_constructivist(rgb, TW, TH, packed, idx_buf)
                     : dither_ved_fs(rgb, TW, TH, packed, idx_buf, 30);
    double t2 = now_ms();
    fprintf(stderr, "[time] %s : %.1f ms\n", russian ? "flat fill" : "dither", t2 - t1);
    if (rc != 0) {
        fprintf(stderr, "dither failed\n");
        return 1;
    }

    // Build BMP preview by looking up each palette index.
    uint8_t *preview = malloc((size_t)TW * TH * 3);
    for (int i = 0; i < TW * TH; i++) {
        uint8_t p = idx_buf[i];
        preview[i*3 + 0] = PALETTE_RGB_MEASURED[p][0];
        preview[i*3 + 1] = PALETTE_RGB_MEASURED[p][1];
        preview[i*3 + 2] = PALETTE_RGB_MEASURED[p][2];
    }
    write_bmp("out_preview.bmp", preview, TW, TH);
    fprintf(stderr, "[time] preview write done\n");

    FILE *f = fopen("out_indices.bin", "wb");
    fwrite(idx_buf, 1, (size_t)TW * TH, f);
    fclose(f);

    // Index histogram (so we can sanity-check the colour distribution).
    int hist[PALETTE_N] = {0};
    for (int i = 0; i < TW * TH; i++) hist[idx_buf[i]]++;
    static const char *NAMES[] = {"black","white","yellow","red","blue","green"};
    fprintf(stderr, "histogram (palette index → pixel count):\n");
    for (int i = 0; i < PALETTE_N; i++) {
        fprintf(stderr, "  %s : %d  (%.1f%%)\n",
                NAMES[i], hist[i], hist[i] * 100.0 / (TW*TH));
    }

    free(preview); free(idx_buf); free(packed); free(rgb);
    return 0;
}

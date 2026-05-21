#include "battery_overlay.h"

#include "epd_4in0e.h"
#include "palette.h"

#include <stddef.h>
#include <stdint.h>

#define BAT_W  32
#define BAT_H  14
#define BAT_X  (EPD_4IN0E_WIDTH - BAT_W - 10)
#define BAT_Y  10

static uint8_t get_px(const uint8_t *fb, int x, int y)
{
    size_t byte_idx = ((size_t)y * EPD_4IN0E_WIDTH + x) >> 1;
    uint8_t v = fb[byte_idx];
    return (x & 1) ? (v & 0x0F) : (v >> 4);
}

static void set_px(uint8_t *fb, int x, int y, uint8_t ink)
{
    if (x < 0 || x >= EPD_4IN0E_WIDTH || y < 0 || y >= EPD_4IN0E_HEIGHT) return;
    size_t byte_idx = ((size_t)y * EPD_4IN0E_WIDTH + x) >> 1;
    if ((x & 1) == 0) {
        fb[byte_idx] = (fb[byte_idx] & 0x0F) | (ink << 4);
    } else {
        fb[byte_idx] = (fb[byte_idx] & 0xF0) | (ink & 0x0F);
    }
}

static uint8_t ink_luma(uint8_t ink)
{
    switch (ink & 0x0F) {
    case EPD_INK_BLACK:  return 20;
    case EPD_INK_WHITE:  return 235;
    case EPD_INK_YELLOW: return 190;
    case EPD_INK_RED:    return 85;
    case EPD_INK_BLUE:   return 75;
    case EPD_INK_GREEN:  return 95;
    default:             return 128;
    }
}

static void draw_hline(uint8_t *fb, int x0, int x1, int y, uint8_t ink)
{
    for (int x = x0; x <= x1; x++) set_px(fb, x, y, ink);
}

static void draw_vline(uint8_t *fb, int x, int y0, int y1, uint8_t ink)
{
    for (int y = y0; y <= y1; y++) set_px(fb, x, y, ink);
}

static void fill_rect(uint8_t *fb, int x, int y, int w, int h, uint8_t ink)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) set_px(fb, xx, yy, ink);
    }
}

void battery_overlay_draw(uint8_t *fb, uint8_t percent)
{
    if (!fb) return;
    if (percent > 100) percent = 100;

    uint32_t luma_sum = 0;
    uint32_t n = 0;
    for (int y = BAT_Y - 2; y < BAT_Y + BAT_H + 2; y++) {
        for (int x = BAT_X - 2; x < BAT_X + BAT_W + 4; x++) {
            if (x < 0 || x >= EPD_4IN0E_WIDTH || y < 0 || y >= EPD_4IN0E_HEIGHT) continue;
            luma_sum += ink_luma(get_px(fb, x, y));
            n++;
        }
    }
    uint8_t ink = percent < 10
                    ? EPD_INK_RED
                    : ((n && (luma_sum / n) > 145) ? EPD_INK_BLACK : EPD_INK_WHITE);

    const int body_w = BAT_W - 4;
    const int nub_x = BAT_X + body_w + 1;
    const int nub_y = BAT_Y + 4;

    draw_hline(fb, BAT_X, BAT_X + body_w, BAT_Y, ink);
    draw_hline(fb, BAT_X, BAT_X + body_w, BAT_Y + BAT_H - 1, ink);
    draw_vline(fb, BAT_X, BAT_Y, BAT_Y + BAT_H - 1, ink);
    draw_vline(fb, BAT_X + body_w, BAT_Y, BAT_Y + BAT_H - 1, ink);
    fill_rect(fb, nub_x, nub_y, 2, BAT_H - 8, ink);

    int inner_w = body_w - 4;
    int fill_w = (inner_w * percent + 50) / 100;
    if (percent > 0 && fill_w < 1) fill_w = 1;
    fill_rect(fb, BAT_X + 2, BAT_Y + 2, fill_w, BAT_H - 4, ink);
}

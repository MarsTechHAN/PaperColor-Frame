// welcome_overlay.c — paint per-device SSID and password into the welcome
// screen's reserved slots.
//
// Font is a compact 5x7 bitmap rendered at 2x scale (10x14 panel pixels per
// glyph). The welcome card's value slots are short, single-line strips just
// above each underline, so a smaller font keeps the value inside the strip
// without spilling into the big "Wi-Fi" / "Password" headings. Only ASCII
// SSID/password characters are required; everything outside the supported set
// renders blank.

#include "welcome_overlay.h"

#include <stddef.h>
#include <string.h>

#include "epd_4in0e.h"
#include "palette.h"

// Slot rectangles in panel pixels, matched to the underline strips drawn in
// the new welcome card. Each slot exactly covers an underline; clear_slot
// wipes it to white before we draw the value text. The SSID underline lives
// at the same x range in both bins (the "Wi-Fi" label is the same width); the
// password underline differs because "Password" is much wider than "密码".
#define SSID_SLOT_X  154
#define SSID_SLOT_W  182  // both languages
#define SSID_SLOT_Y  207

#define PWD_SLOT_X_ZH 135
#define PWD_SLOT_W_ZH 201
#define PWD_SLOT_X_EN 230
#define PWD_SLOT_W_EN 106
#define PWD_SLOT_Y    266

#define SLOT_H       14

#define GLYPH_W      5
#define GLYPH_H      7
#define GLYPH_SCALE  2
#define GLYPH_GAP    1  // source-pixel gap; becomes GLYPH_GAP * GLYPH_SCALE on panel

// 5x7 glyph table. Each row of a glyph is one byte; the low 5 bits are pixels,
// bit 4 = leftmost. Seven rows top-to-bottom. Coverage is intentionally narrow:
// every character the welcome card ever needs to print (SSID prefix, MAC hex,
// password alphabet, hyphen) is here. Look-ups outside the table render blank.
typedef struct {
    char     c;
    uint8_t  rows[GLYPH_H];
} glyph_t;

static const glyph_t GLYPHS[] = {
    {' ',  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-',  {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'(',  {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')',  {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},

    {'0',  {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1',  {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2',  {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3',  {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4',  {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5',  {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6',  {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7',  {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8',  {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9',  {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},

    {'A',  {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B',  {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C',  {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D',  {0x1E, 0x09, 0x09, 0x09, 0x09, 0x09, 0x1E}},
    {'E',  {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F',  {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'P',  {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},

    {'a',  {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}},
    {'b',  {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E}},
    {'c',  {0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E}},
    {'d',  {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F}},
    {'e',  {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},
    {'f',  {0x06, 0x09, 0x08, 0x1E, 0x08, 0x08, 0x08}},
    {'g',  {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E}},
    {'h',  {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'i',  {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
    {'j',  {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}},
    {'k',  {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}},
    {'l',  {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'m',  {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15}},
    {'n',  {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'o',  {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}},
    {'p',  {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10}},
    {'q',  {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01}},
    {'r',  {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
    {'s',  {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}},
    {'t',  {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06}},
    {'u',  {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}},
    {'v',  {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'w',  {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}},
    {'x',  {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}},
    {'y',  {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E}},
    {'z',  {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}},
};

static const uint8_t *glyph_for(char c)
{
    for (size_t i = 0; i < sizeof GLYPHS / sizeof GLYPHS[0]; i++) {
        if (GLYPHS[i].c == c) return GLYPHS[i].rows;
    }
    return NULL;
}

static void fb_set_px(uint8_t *fb, int x, int y, uint8_t ink)
{
    if (x < 0 || x >= EPD_4IN0E_WIDTH || y < 0 || y >= EPD_4IN0E_HEIGHT) return;
    size_t byte_idx = ((size_t)y * EPD_4IN0E_WIDTH + x) >> 1;
    if ((x & 1) == 0) {
        fb[byte_idx] = (fb[byte_idx] & 0x0F) | ((ink & 0x0F) << 4);
    } else {
        fb[byte_idx] = (fb[byte_idx] & 0xF0) | (ink & 0x0F);
    }
}

static int draw_char(uint8_t *fb, int x, int y, char c, uint8_t ink)
{
    const uint8_t *g = glyph_for(c);
    if (!g) {
        // Unknown character — leave a same-width gap so alignment stays stable.
        return (GLYPH_W + GLYPH_GAP) * GLYPH_SCALE;
    }
    for (int row = 0; row < GLYPH_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < GLYPH_W; col++) {
            if (bits & (1 << (GLYPH_W - 1 - col))) {
                int px0 = x + col * GLYPH_SCALE;
                int py0 = y + row * GLYPH_SCALE;
                for (int dy = 0; dy < GLYPH_SCALE; dy++) {
                    for (int dx = 0; dx < GLYPH_SCALE; dx++) {
                        fb_set_px(fb, px0 + dx, py0 + dy, ink);
                    }
                }
            }
        }
    }
    return (GLYPH_W + GLYPH_GAP) * GLYPH_SCALE;
}

static void clear_slot(uint8_t *fb, int x, int y, int w, int h)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            fb_set_px(fb, xx, yy, EPD_INK_WHITE);
        }
    }
}

static int measure_text(const char *s)
{
    if (!s) return 0;
    int n = (int)strlen(s);
    if (n <= 0) return 0;
    int per = (GLYPH_W + GLYPH_GAP) * GLYPH_SCALE;
    // Last glyph doesn't need trailing gap.
    return n * per - GLYPH_GAP * GLYPH_SCALE;
}

static void draw_text_centered(uint8_t *fb, const char *s, int slot_x, int slot_y,
                               int slot_w, int slot_h, uint8_t ink)
{
    if (!s || !*s) return;
    int text_w = measure_text(s);
    int text_h = GLYPH_H * GLYPH_SCALE;
    int x = slot_x + (slot_w - text_w) / 2;
    int y = slot_y + (slot_h - text_h) / 2;
    if (x < slot_x) x = slot_x;
    for (const char *p = s; *p; p++) {
        x += draw_char(fb, x, y, *p, ink);
    }
}

void welcome_overlay_draw(uint8_t *fb, int lang,
                          const char *ssid, const char *password)
{
    if (!fb) return;

    int pwd_x = (lang == 1) ? PWD_SLOT_X_EN : PWD_SLOT_X_ZH;
    int pwd_w = (lang == 1) ? PWD_SLOT_W_EN : PWD_SLOT_W_ZH;

    clear_slot(fb, SSID_SLOT_X, SSID_SLOT_Y, SSID_SLOT_W, SLOT_H);
    clear_slot(fb, pwd_x,       PWD_SLOT_Y,  pwd_w,       SLOT_H);

    if (ssid && *ssid) {
        draw_text_centered(fb, ssid,
                           SSID_SLOT_X, SSID_SLOT_Y, SSID_SLOT_W, SLOT_H,
                           EPD_INK_BLACK);
    }
    // Empty password is the factory default — show "(open)" so the user
    // doesn't stare at a blank slot wondering whether the field is broken.
    const char *pwd_label = (password && *password) ? password : "(open)";
    draw_text_centered(fb, pwd_label,
                       pwd_x, PWD_SLOT_Y, pwd_w, SLOT_H,
                       EPD_INK_BLACK);
}

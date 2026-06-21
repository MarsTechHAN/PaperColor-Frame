// color_pipeline.c — sRGB↔LAB, CIEDE2000, and apply_adjust port.
//
// Port of the Python reference:
//   * apply_adjust()        — brightness/contrast/temperature/saturation/gamma
//   * rgb_to_lab via colormath sRGBColor → LabColor (D65, standard sRGB matrix)
//   * custom_delta_e_2000()
//
// Performance tricks (vs. the Python reference):
//   * 256-entry sRGB→linear LUT.  Burns 1 KiB, eliminates 3× powf() per pixel.
//   * Single pass over the buffer for brightness/contrast/temperature.
//   * Tabular gamma after-the-fact (also 256 entries).
//   * Saturation done in HSV via the same fast formulas OpenCV uses (no
//     LUT — saturation factor changes per call).

#include "color_pipeline.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

// --------------------------------------------------------------- sRGB ↔ XYZ
static float s_srgb_to_linear[256];   // 0..1, sRGB inverse companding
static const float D65_Xn = 0.95047f;
static const float D65_Yn = 1.00000f;
static const float D65_Zn = 1.08883f;

void color_pipeline_init(void)
{
    for (int i = 0; i < 256; i++) {
        float c = (float)i / 255.0f;
        s_srgb_to_linear[i] =
            (c <= 0.04045f) ? (c / 12.92f)
                            : powf((c + 0.055f) / 1.055f, 2.4f);
    }
}

void adjust_cfg_default(adjust_cfg_t *o)
{
    o->brightness  = 0;
    o->contrast    = 100;
    o->saturation  = 100;
    o->gamma       = 100;
    o->temperature = 100;
    o->tint        = 100;
    o->smoothness  = 30;
}

// ----------------------------------------------------- colour-rendering mode
static color_render_mode_t s_color_render_mode = COLOR_RENDER_6COLOR;

void color_render_set_mode(color_render_mode_t mode)
{
    if (mode < COLOR_RENDER_6COLOR || mode > COLOR_RENDER_REFLECTANCE) {
        mode = COLOR_RENDER_6COLOR;
    }
    s_color_render_mode = mode;
}

color_render_mode_t color_render_get_mode(void)
{
    return s_color_render_mode;
}

// --------------------------------------------------------------- apply_adjust
//
// One pass over the buffer.  Python order:
//   1. contrast & brightness:    out = clip(rgb*contrast + brightness)
//   2. temperature:              warm/cool tilt of R,B (G untouched)
//   3. saturation (in HSV):      S' = clip(S * sat)
//   4. gamma:                    out = (out/255)^(1/gamma) * 255
//
// Saturation: we use the same model OpenCV does (max-min-based S).

static inline uint8_t clamp_u8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

static inline float fclamp01(float x)
{
    return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
}

void apply_adjust_rgb888(uint8_t *rgb, int w, int h, const adjust_cfg_t *cfg)
{
    const float contrast = cfg->contrast / 100.0f;
    const float bright   = (float)cfg->brightness;
    const float sat      = cfg->saturation / 100.0f;
    const float gamma    = cfg->gamma / 100.0f;
    const float temp     = cfg->temperature / 100.0f;
    const float tint     = cfg->tint / 100.0f;

    const int N = w * h;

    // Pre-build gamma LUT (256 entries).
    uint8_t gamma_lut[256];
    {
        const float inv_g = 1.0f / (gamma < 0.01f ? 0.01f : gamma);
        for (int i = 0; i < 256; i++) {
            float v = powf((float)i / 255.0f, inv_g) * 255.0f;
            gamma_lut[i] = clamp_u8((int)(v + 0.5f));
        }
    }

    for (int i = 0; i < N; i++) {
        uint8_t *p = rgb + i * 3;
        float r = p[0];
        float g = p[1];
        float b = p[2];

        // 1) brightness + contrast (linear over uint8 range, like Python)
        r = r * contrast + bright;
        g = g * contrast + bright;
        b = b * contrast + bright;

        // 2) temperature — yellow ↔ blue axis (R-B tilt)
        if (temp > 1.0f) {           // warm
            r *= temp;
            b /= temp;
        } else if (temp < 1.0f) {    // cool
            r *= temp;               // matches Python: r_channel *= temp
            b /= (2.0f - temp);
        }

        // 2b) tint — magenta ↔ green axis (G tilt).  >1.0 suppresses G
        // (image reads more magenta), <1.0 boosts G (image reads more green).
        // Symmetric divide keeps the maths terse and gives roughly equal
        // perceptual movement on either side of neutral.
        if (tint != 1.0f) {
            g /= tint;
        }

        // clamp pre-HSV
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;

        // 3) saturation in HSV (the same model OpenCV cvtColor uses)
        if (sat != 1.0f) {
            float maxc = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minc = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float v = maxc;
            float s = (maxc > 0.f) ? (maxc - minc) / maxc : 0.f;
            float s_new = s * sat;
            if (s_new > 1.f) s_new = 1.f;
            if (s_new < 0.f) s_new = 0.f;
            if (maxc > 0.f) {
                float k = (1.0f - s_new) * v;     // new min
                float scale = (maxc - minc) > 0.f
                                ? (v - k) / (maxc - minc)
                                : 0.f;
                r = k + (r - minc) * scale;
                g = k + (g - minc) * scale;
                b = k + (b - minc) * scale;
            }
        }

        // 4) gamma via LUT
        p[0] = gamma_lut[clamp_u8((int)(r + 0.5f))];
        p[1] = gamma_lut[clamp_u8((int)(g + 0.5f))];
        p[2] = gamma_lut[clamp_u8((int)(b + 0.5f))];
    }
}

// ------------------------------------------------------------ sRGB → CIELAB
//
// Matches colormath sRGBColor → LabColor (D65, standard matrix, no chromatic
// adaptation).  The XYZ matrix is the canonical sRGB→XYZ D65 transform.

static inline float lab_f(float t)
{
    // f(t) used by the L*a*b* definition.
    if (t > 0.008856f) return cbrtf(t);
    return 7.787f * t + (16.0f / 116.0f);
}

static inline void linrgb_to_lab(float R, float G, float B,
                                 float *L, float *a, float *b_out)
{
    float X = R * 0.4124564f + G * 0.3575761f + B * 0.1804375f;
    float Y = R * 0.2126729f + G * 0.7151522f + B * 0.0721750f;
    float Z = R * 0.0193339f + G * 0.1191920f + B * 0.9503041f;

    float fx = lab_f(X / D65_Xn);
    float fy = lab_f(Y / D65_Yn);
    float fz = lab_f(Z / D65_Zn);

    *L     = 116.0f * fy - 16.0f;
    *a     = 500.0f * (fx - fy);
    *b_out = 200.0f * (fy - fz);
}

void rgb_to_lab_u8(uint8_t r, uint8_t g, uint8_t b,
                   float *L, float *a, float *b_out)
{
    float R = s_srgb_to_linear[r];
    float G = s_srgb_to_linear[g];
    float B = s_srgb_to_linear[b];
    linrgb_to_lab(R, G, B, L, a, b_out);
}

static float srgb_inv_companding(float c)
{
    c = fclamp01(c);
    return (c <= 0.04045f) ? (c / 12.92f)
                           : powf((c + 0.055f) / 1.055f, 2.4f);
}

void rgb_to_lab_f(float r, float g, float b, float *L, float *a, float *b_out)
{
    linrgb_to_lab(srgb_inv_companding(r),
                  srgb_inv_companding(g),
                  srgb_inv_companding(b),
                  L, a, b_out);
}

// ---------------------------------------------------------- CIELAB ↔ XYZ (D65)
//
// Used by the v2 (reflectance) renderer.  These are the diffuse XYZ tristimulus
// values — optical dot-mixing of reflective ink is (approximately) linear in
// XYZ, unlike CIELAB which is a cube-root/opponent space.  The forward f() and
// D65 white point match linrgb_to_lab above, so lab_to_xyz∘xyz_to_lab is an
// identity within float precision.

static inline float lab_finv(float t)
{
    // Inverse of lab_f().
    float t3 = t * t * t;
    if (t3 > 0.008856f) return t3;
    return (t - 16.0f / 116.0f) / 7.787f;
}

void lab_to_xyz(float L, float a, float b, float *X, float *Y, float *Z)
{
    float fy = (L + 16.0f) / 116.0f;
    float fx = a / 500.0f + fy;
    float fz = fy - b / 200.0f;
    *X = D65_Xn * lab_finv(fx);
    *Y = D65_Yn * lab_finv(fy);
    *Z = D65_Zn * lab_finv(fz);
}

void xyz_to_lab(float X, float Y, float Z, float *L, float *a, float *b_out)
{
    float fx = lab_f(X / D65_Xn);
    float fy = lab_f(Y / D65_Yn);
    float fz = lab_f(Z / D65_Zn);
    *L     = 116.0f * fy - 16.0f;
    *a     = 500.0f * (fx - fy);
    *b_out = 200.0f * (fy - fz);
}

float color_srgb_to_linear(uint8_t c)
{
    return s_srgb_to_linear[c];
}

uint8_t color_linear_to_srgb(float lin)
{
    if (lin <= 0.0f) return 0;
    if (lin >= 1.0f) return 255;
    float v = (lin <= 0.0031308f) ? (12.92f * lin)
                                  : (1.055f * powf(lin, 1.0f / 2.4f) - 0.055f);
    int i = (int)(v * 255.0f + 0.5f);
    return i < 0 ? 0 : (i > 255 ? 255 : (uint8_t)i);
}

// ------------------------------------------------------------------ CIEDE2000
//
// Faithful port of the Python custom_delta_e_2000().  Hot path of dithering —
// expect ~6 × W × H calls per image.  All math single-precision, FPU-friendly.

static inline float deg_rad(float deg) { return deg * (float)M_PI / 180.0f; }
static inline float rad_deg(float rad) { return rad * 180.0f / (float)M_PI; }

float ciede2000(float L1, float a1, float b1,
                float L2, float a2, float b2)
{
    float C1 = sqrtf(a1 * a1 + b1 * b1);
    float C2 = sqrtf(a2 * a2 + b2 * b2);
    float C_bar = 0.5f * (C1 + C2);

    float C_bar7 = C_bar; for (int i = 0; i < 6; i++) C_bar7 *= C_bar;
    const float k25_7 = 6103515625.0f; // 25^7
    float G = 0.5f * (1.0f - sqrtf(C_bar7 / (C_bar7 + k25_7)));

    float a1p = (1.0f + G) * a1;
    float a2p = (1.0f + G) * a2;

    float C1p = sqrtf(a1p * a1p + b1 * b1);
    float C2p = sqrtf(a2p * a2p + b2 * b2);

    float h1p = (a1p == 0.0f && b1 == 0.0f) ? 0.0f : atan2f(b1, a1p);
    float h2p = (a2p == 0.0f && b2 == 0.0f) ? 0.0f : atan2f(b2, a2p);
    if (h1p < 0.0f) h1p += 2.0f * (float)M_PI;
    if (h2p < 0.0f) h2p += 2.0f * (float)M_PI;
    h1p = rad_deg(h1p);
    h2p = rad_deg(h2p);

    float dLp = L2 - L1;
    float dCp = C2p - C1p;

    float dhp = 0.0f;
    if (C1p * C2p != 0.0f) {
        float diff = h2p - h1p;
        if (fabsf(diff) <= 180.0f)      dhp = diff;
        else if (diff > 180.0f)         dhp = diff - 360.0f;
        else                            dhp = diff + 360.0f;
    }
    float dHp = 2.0f * sqrtf(C1p * C2p) * sinf(deg_rad(dhp) * 0.5f);

    float Lbp = 0.5f * (L1 + L2);
    float Cbp = 0.5f * (C1p + C2p);

    float hbp = h1p + h2p;
    if (C1p * C2p != 0.0f) {
        if (fabsf(h1p - h2p) <= 180.0f) hbp = 0.5f * (h1p + h2p);
        else                            hbp = (h1p + h2p < 360.0f)
                                                ? 0.5f * (h1p + h2p + 360.0f)
                                                : 0.5f * (h1p + h2p - 360.0f);
    }

    float T = 1.0f
            - 0.17f * cosf(deg_rad(hbp -  30.0f))
            + 0.24f * cosf(deg_rad(2.0f * hbp))
            + 0.32f * cosf(deg_rad(3.0f * hbp + 6.0f))
            - 0.20f * cosf(deg_rad(4.0f * hbp - 63.0f));

    float dth = 30.0f * expf(-powf((hbp - 275.0f) / 25.0f, 2.0f));

    float Cbp7 = Cbp; for (int i = 0; i < 6; i++) Cbp7 *= Cbp;
    float Rc = 2.0f * sqrtf(Cbp7 / (Cbp7 + k25_7));

    float Sl_den = sqrtf(20.0f + (Lbp - 50.0f) * (Lbp - 50.0f));
    float Sl = 1.0f + (0.015f * (Lbp - 50.0f) * (Lbp - 50.0f)) / Sl_den;
    float Sc = 1.0f + 0.045f * Cbp;
    float Sh = 1.0f + 0.015f * Cbp * T;

    float Rt = -sinf(deg_rad(2.0f * dth)) * Rc;

    float t1 = dLp / Sl;
    float t2 = dCp / Sc;
    float t3 = dHp / Sh;

    return sqrtf(t1 * t1 + t2 * t2 + t3 * t3 + Rt * t2 * t3);
}

// ------------------------------------------------------- EINK pre-enhancement
//
// A tiny deterministic analogue of a WASM-friendly enhancement model.  It uses
// a 5-tap separable luma blur as the local base layer, then boosts detail and
// chroma in proportion to local contrast.  Doing this before six-colour
// quantisation gives the halftoner clearer edges and more separable transition
// colours without needing a neural runtime or model weights.

static inline int clamp_i32(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int enhance_eink_rgb888(uint8_t *rgb, int w, int h)
{
    if (!rgb || w <= 0 || h <= 0) return -1;
    const size_t n = (size_t)w * h;
    uint8_t *lum = (uint8_t *)malloc(n);
    uint8_t *tmp = (uint8_t *)malloc(n);
    uint8_t *base = (uint8_t *)malloc(n);
    if (!lum || !tmp || !base) {
        free(lum); free(tmp); free(base);
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = rgb + i * 3;
        lum[i] = (uint8_t)((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8);
    }

    // 5-tap binomial blur [1 4 6 4 1] / 16, separable.
    for (int y = 0; y < h; y++) {
        const uint8_t *row = lum + (size_t)y * w;
        uint8_t *dst = tmp + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int x0 = x > 1 ? x - 2 : 0;
            int x1 = x > 0 ? x - 1 : 0;
            int x3 = x < w - 1 ? x + 1 : w - 1;
            int x4 = x < w - 2 ? x + 2 : w - 1;
            int v = row[x0] + 4 * row[x1] + 6 * row[x] + 4 * row[x3] + row[x4];
            dst[x] = (uint8_t)((v + 8) >> 4);
        }
    }
    for (int y = 0; y < h; y++) {
        int y0 = y > 1 ? y - 2 : 0;
        int y1 = y > 0 ? y - 1 : 0;
        int y3 = y < h - 1 ? y + 1 : h - 1;
        int y4 = y < h - 2 ? y + 2 : h - 1;
        uint8_t *dst = base + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int v = tmp[(size_t)y0 * w + x]
                  + 4 * tmp[(size_t)y1 * w + x]
                  + 6 * tmp[(size_t)y  * w + x]
                  + 4 * tmp[(size_t)y3 * w + x]
                  +     tmp[(size_t)y4 * w + x];
            dst[x] = (uint8_t)((v + 8) >> 4);
        }
    }

    // v2 (REFLECTANCE) softens this stage: tone shaping moves to the
    // display-referred curve in map_source_to_panel_lab, and chroma is left to
    // the gamut map, so here we only do a gentle edge-clarity pass with no
    // S-curve and no chroma boost — avoiding the contrast/chroma EXPANSION that
    // the narrow panel window then clips into halos and out-of-gamut speckle.
    const int v2 = (color_render_get_mode() == COLOR_RENDER_REFLECTANCE);

    for (size_t i = 0; i < n; i++) {
        uint8_t *p = rgb + i * 3;
        int yv = lum[i];
        int bv = base[i];
        int detail = yv - bv;
        int absd = detail < 0 ? -detail : detail;

        // Stronger detail boost in textured/edge pixels, gentler in flats.
        float edge = absd > 28 ? 1.0f : (float)absd / 28.0f;
        float clarity = v2 ? (0.12f + 0.15f * edge) : (0.26f + 0.30f * edge);
        int y2 = yv + (int)(detail * clarity + (detail >= 0 ? 0.5f : -0.5f));

        // Reflective paper loses deep shadows and bright highlights; a mild
        // S-curve increases mid-tone separation without clipping endpoints hard.
        // v2 skips it (the display-referred tone curve handles luminance).
        if (!v2) {
            float t = (float)y2 / 255.0f;
            float s = t + 0.10f * (t - 0.5f) * (1.0f - fabsf(2.0f * t - 1.0f));
            y2 = clamp_i32((int)(s * 255.0f + 0.5f), 0, 255);
        }

        // Chroma separation: keep hue, expand distance from luma a little so
        // the six-ink dither has clearer colour decisions in transition zones.
        // v2 keeps chroma at 1.0 (no boost) — the tiny panel gamut turns an
        // unconditional boost into colored speckle.
        float sat = v2 ? 1.0f : (1.06f + 0.08f * edge);
        int r = y2 + (int)((p[0] - yv) * sat + ((p[0] >= yv) ? 0.5f : -0.5f));
        int g = y2 + (int)((p[1] - yv) * sat + ((p[1] >= yv) ? 0.5f : -0.5f));
        int b = y2 + (int)((p[2] - yv) * sat + ((p[2] >= yv) ? 0.5f : -0.5f));
        p[0] = (uint8_t)clamp_i32(r, 0, 255);
        p[1] = (uint8_t)clamp_i32(g, 0, 255);
        p[2] = (uint8_t)clamp_i32(b, 0, 255);
    }

    free(lum); free(tmp); free(base);
    return 0;
}

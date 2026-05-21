#include "palette.h"
#include "color_pipeline.h"

#include <math.h>
#include <string.h>

const uint8_t PALETTE_INK_CODE[PALETTE_N] = {
    [PIDX_BLACK]  = EPD_INK_BLACK,
    [PIDX_WHITE]  = EPD_INK_WHITE,
    [PIDX_YELLOW] = EPD_INK_YELLOW,
    [PIDX_RED]    = EPD_INK_RED,
    [PIDX_BLUE]   = EPD_INK_BLUE,
    [PIDX_GREEN]  = EPD_INK_GREEN,
};

// Nominal sRGB primaries.  Kept for labels/tests; distance matching uses the
// measured reflective palette below because the panel is not emissive sRGB.
const uint8_t PALETTE_RGB[PALETTE_N][3] = {
    [PIDX_BLACK]  = {  0,   0,   0},
    [PIDX_WHITE]  = {255, 255, 255},
    [PIDX_YELLOW] = {255, 221,   0},
    [PIDX_RED]    = {206,  38,  54},
    [PIDX_BLUE]   = {  0,  64, 255},
    [PIDX_GREEN]  = {  0, 128,   0},
};

// Colorimeter sRGB readings — retained for labels, the calibration wizard UI,
// and tests. Distance matching uses PALETTE_LAB below, which is now derived
// from the colorimeter's measured CIE Lab (SCI) with a specular-floor
// correction applied at init, not from these sRGB triplets.
const uint8_t PALETTE_RGB_MEASURED[PALETTE_N][3] = {
    [PIDX_BLACK]  = { 72,  71,  82},
    [PIDX_WHITE]  = {151, 161, 161},
    [PIDX_YELLOW] = {164, 154,  69},
    [PIDX_RED]    = {120,  70,  71},
    [PIDX_BLUE]   = { 60,  99, 136},
    [PIDX_GREEN]  = { 79, 103,  77},
};

// Measured CIE L*a*b* from doc/papercolor_gamut_samples_20260521.csv columns
// L,a,b — D65/2°, d/8 geometry, SCI mode, through AG glass. SCI bakes a ~4%
// first-surface Fresnel specular reflection into every patch, which under
// d/8 raises L* uniformly and depresses C* proportionally. palette_init()
// subtracts a constant specular floor in XYZ to estimate the SCE (in-use)
// appearance — that's what the eye actually sees through AG glass — and
// stores the result in PALETTE_LAB for the dither picker.
static const float PALETTE_LAB_SCI[PALETTE_N][3] = {
    [PIDX_BLACK]  = { 30.6245f,   2.9592f,  -6.3928f},
    [PIDX_WHITE]  = { 65.4689f,  -3.5446f,  -1.2256f},
    [PIDX_YELLOW] = { 62.8726f,  -7.5112f,  45.0810f},
    [PIDX_RED]    = { 35.6534f,  21.5614f,   8.6294f},
    [PIDX_BLUE]   = { 40.6722f,  -2.3424f, -24.7103f},
    [PIDX_GREEN]  = { 41.0541f, -14.5846f,  11.8314f},
};

float PALETTE_LAB[PALETTE_N][3];

// --------------------------------------------------------- SCI → SCE correction
//
// AG glass + d/8 SCI: the colorimeter integrates ~4% first-surface Fresnel
// reflection of the illuminant into every measurement, an achromatic floor
// that uniformly inflates L* and shrinks C*. Subtracting k·{Xn,Yn,Zn} in XYZ
// undoes this, recovering the diffuse (SCE) reflectance — closer to what the
// eye sees with the AG haze removed.
//
// k≈0.04 ≈ D65 Fresnel reflection at crown glass. AG etching scatters part
// of that back into the diffuse channel so the effective k for a specific
// panel may run 0.025..0.05; the wasm_set_specular_floor() bridge exposes
// it for A/B tuning.
//
// IMPORTANT: this only moves panel-side ink anchors. The source-side Lab in
// the dither (rgb_to_lab + map_source_to_panel_lab) is untouched, so error-
// diffusion residual is preserved — see the architectural note at the top
// of dither.c.

#define D65_Xn 0.95047f
#define D65_Yn 1.00000f
#define D65_Zn 1.08883f

#define SPECULAR_FLOOR_K_DEFAULT 0.04f
static float s_specular_floor_k = SPECULAR_FLOOR_K_DEFAULT;

// Default to LEGACY: the SCI→SCE pipeline is currently scaffolded as an
// experimental opt-in surfaced in the Adjust panel.  Once a/b proves it's
// universally better the default can flip and the LEGACY branch can be
// retired.
static palette_mode_t s_palette_mode = PALETTE_MODE_LEGACY;

static float specular_floor_for_mode(palette_mode_t mode)
{
    switch (mode) {
        case PALETTE_MODE_SCE_OFF:     return 0.00f;
        case PALETTE_MODE_SCE_SUBTLE:  return 0.02f;
        case PALETTE_MODE_SCE_DEFAULT: return 0.04f;
        case PALETTE_MODE_SCE_STRONG:  return 0.06f;
        default:                       return 0.00f;  // LEGACY: k irrelevant
    }
}

static inline float lab_finv(float t)
{
    // Inverse of the CIELAB f(t) used by linrgb_to_lab.
    float t3 = t * t * t;
    if (t3 > 0.008856f) return t3;
    return (t - 16.0f / 116.0f) / 7.787f;
}

static inline float lab_f_fwd(float t)
{
    if (t > 0.008856f) return cbrtf(t);
    return 7.787f * t + (16.0f / 116.0f);
}

static void lab_apply_sce(const float lab_sci[3], float lab_sce[3], float k)
{
    // Lab → XYZ
    float fy = (lab_sci[0] + 16.0f) / 116.0f;
    float fx = lab_sci[1] / 500.0f + fy;
    float fz = fy - lab_sci[2] / 200.0f;
    float X = D65_Xn * lab_finv(fx);
    float Y = D65_Yn * lab_finv(fy);
    float Z = D65_Zn * lab_finv(fz);

    // Subtract achromatic specular floor in XYZ.
    X -= k * D65_Xn;
    Y -= k * D65_Yn;
    Z -= k * D65_Zn;

    // Clamp to a small positive floor so f_inv is well-defined even when
    // k overshoots the actual specular component on dark patches.
    const float floor_x = 0.0005f * D65_Xn;
    const float floor_y = 0.0005f * D65_Yn;
    const float floor_z = 0.0005f * D65_Zn;
    if (X < floor_x) X = floor_x;
    if (Y < floor_y) Y = floor_y;
    if (Z < floor_z) Z = floor_z;

    // XYZ → Lab
    float fx2 = lab_f_fwd(X / D65_Xn);
    float fy2 = lab_f_fwd(Y / D65_Yn);
    float fz2 = lab_f_fwd(Z / D65_Zn);
    lab_sce[0] = 116.0f * fy2 - 16.0f;
    lab_sce[1] = 500.0f * (fx2 - fy2);
    lab_sce[2] = 200.0f * (fy2 - fz2);
}

void palette_set_specular_floor(float k)
{
    if (k < 0.0f) k = 0.0f;
    if (k > 0.08f) k = 0.08f;
    s_specular_floor_k = k;
    palette_init();
}

float palette_get_specular_floor(void)
{
    return s_specular_floor_k;
}

void palette_set_mode(palette_mode_t mode)
{
    if (mode < PALETTE_MODE_LEGACY || mode > PALETTE_MODE_SCE_STRONG) {
        mode = PALETTE_MODE_LEGACY;
    }
    s_palette_mode = mode;
    s_specular_floor_k = specular_floor_for_mode(mode);
    palette_init();
}

palette_mode_t palette_get_mode(void)
{
    return s_palette_mode;
}

// Mixed-patch readings from PaperColor-Cali-Data-20260521.xlsx.  Each row is
// the same halftone target order as the web calibration wizard after the six
// solid inks.  The dither uses these as a low-order optical model for what a
// local cluster of differently-coloured E6 particles actually measures as.
typedef struct {
    uint8_t weights[PALETTE_N];
    uint8_t rgb[3];
} palette_mix_default_t;

static const palette_mix_default_t MIX_DEFAULTS[PALETTE_MIX_PATCH_N] = {
    {{25, 75,  0,  0,  0,  0}, {130, 138, 142}}, // white_black_25
    {{50, 50,  0,  0,  0,  0}, {112, 118, 123}}, // white_black_50
    {{75, 25,  0,  0,  0,  0}, { 93,  96, 104}}, // white_black_75
    {{ 0, 75, 25,  0,  0,  0}, {155, 159, 141}}, // white_yellow_25
    {{ 0, 50, 50,  0,  0,  0}, {158, 157, 121}}, // white_yellow_50
    {{ 0, 25, 75,  0,  0,  0}, {161, 156,  97}}, // white_yellow_75
    {{ 0, 75,  0, 25,  0,  0}, {143, 142, 141}}, // white_red_25
    {{ 0, 50,  0, 50,  0,  0}, {136, 122, 120}}, // white_red_50
    {{ 0, 25,  0, 75,  0,  0}, {129,  99,  97}}, // white_red_75
    {{ 0, 75,  0,  0, 25,  0}, {133, 148, 155}}, // white_blue_25
    {{ 0, 50,  0,  0, 50,  0}, {113, 134, 149}}, // white_blue_50
    {{ 0, 25,  0,  0, 75,  0}, { 90, 118, 143}}, // white_blue_75
    {{ 0, 75,  0,  0,  0, 25}, {141, 151, 140}}, // white_green_25
    {{ 0, 50,  0,  0,  0, 50}, {127, 140, 121}}, // white_green_50
    {{ 0, 25,  0,  0,  0, 75}, {128, 140, 121}}, // white_green_75
    {{50,  0, 50,  0,  0,  0}, {126, 118,  81}}, // black_yellow_50
    {{50,  0,  0, 50,  0,  0}, { 96,  72,  78}}, // black_red_50
    {{50,  0,  0,  0, 50,  0}, { 70,  81, 104}}, // black_blue_50
    {{50,  0,  0,  0,  0, 50}, { 80,  93,  95}}, // black_green_50
    {{ 0,  0, 50, 50,  0,  0}, {146, 122,  72}}, // yellow_red_50
    {{ 0,  0, 50,  0, 50,  0}, {125, 132, 111}}, // yellow_blue_50
    {{ 0,  0, 50,  0,  0, 50}, {132, 137,  79}}, // yellow_green_50
    {{ 0,  0,  0, 50, 50,  0}, { 95,  84, 106}}, // red_blue_50
    {{ 0,  0,  0, 50,  0, 50}, {103,  96,  81}}, // red_green_50
    {{ 0,  0,  0,  0, 50, 50}, { 73, 107, 118}}, // blue_green_50
    {{ 0, 68, 24,  8,  0,  0}, {151, 153, 136}}, // skin_wyr
    {{ 0, 78,  0,  0, 22,  0}, {132, 147, 156}}, // sky_wb
    {{18,  0, 24,  0,  0, 58}, {108, 119,  87}}, // foliage_gyk
};

// Measured CIE Lab of the same 28 halftone patches, same order as MIX_DEFAULTS.
// Source: doc/papercolor_gamut_samples_20260521.csv rows 7-34, columns L,a,b.
// SCI through AG glass — palette_mix_reset_defaults() applies the same SCE
// correction as palette_init() so the dither's mix model sees the eye's view
// of the halftone, not the colorimeter's.
static const float MIX_PATCH_LAB_SCI[PALETTE_MIX_PATCH_N][3] = {
    { 56.9539f,  -2.0868f,  -3.1425f}, // white_black_25
    { 49.2911f,  -1.1860f,  -3.5216f}, // white_black_50
    { 40.7228f,   0.6005f,  -4.9443f}, // white_black_75
    { 64.7142f,  -4.7611f,   8.9077f}, // white_yellow_25
    { 63.9693f,  -5.7496f,  19.0268f}, // white_yellow_50
    { 63.5225f,  -7.1535f,  31.5428f}, // white_yellow_75
    { 59.0749f,   0.1740f,   0.6608f}, // white_red_25
    { 52.4087f,   5.0014f,   3.0486f}, // white_red_50
    { 44.8779f,  11.7784f,   5.7991f}, // white_red_75
    { 60.3667f,  -3.8540f,  -5.5092f}, // white_blue_25
    { 54.7933f,  -4.0356f, -10.5278f}, // white_blue_50
    { 48.4020f,  -3.5382f, -16.8375f}, // white_blue_75
    { 61.3851f,  -5.7520f,   4.5580f}, // white_green_25
    { 56.7313f,  -8.4632f,   8.5644f}, // white_green_50
    { 56.8072f,  -8.0864f,   8.6826f}, // white_green_75
    { 49.4804f,  -2.9364f,  21.5270f}, // black_yellow_50
    { 33.2901f,  11.2822f,   0.3916f}, // black_red_50
    { 34.3676f,   1.7119f, -14.7123f}, // black_blue_50
    { 38.4812f,  -4.5337f,  -2.8393f}, // black_green_50
    { 52.3967f,   2.5027f,  30.6955f}, // yellow_red_50
    { 54.0378f,  -6.6008f,  10.4774f}, // yellow_blue_50
    { 55.4429f, -11.1799f,  30.4319f}, // yellow_green_50
    { 37.5069f,   9.2089f, -10.9820f}, // red_blue_50
    { 40.9665f,   0.0332f,   9.5879f}, // red_green_50
    { 43.1215f,  -9.1633f, -10.0191f}, // blue_green_50
    { 62.6252f,  -3.8607f,   8.6937f}, // skin_wyr
    { 60.0445f,  -3.4165f,  -6.5637f}, // sky_wb
    { 48.3480f, -10.1889f,  16.2807f}, // foliage_gyk
};

palette_mix_patch_t PALETTE_MIX_PATCHES[PALETTE_MIX_PATCH_N];

#define MIX_PAIR_MAX_POINTS 4
#define MIX_ANCHOR_MAX 4

typedef struct {
    uint8_t n;
    float t[MIX_PAIR_MAX_POINTS];
    float q[MIX_PAIR_MAX_POINTS][3];
} mix_pair_model_t;

typedef struct {
    float weights[PALETTE_N];
    float residual[3];
    uint8_t enabled;
} mix_anchor_model_t;

static mix_pair_model_t s_pair_model[PALETTE_N][PALETTE_N];
static mix_anchor_model_t s_anchor_model[MIX_ANCHOR_MAX];
static int s_anchor_count = 0;
static int s_mix_enabled_count = 0;

static int normalise_patch_weights(const uint8_t weights[PALETTE_N], float out[PALETTE_N],
                                   int *first, int *second)
{
    int total = 0;
    int nonzero = 0;
    *first = -1;
    *second = -1;
    for (int i = 0; i < PALETTE_N; i++) total += weights[i];
    if (total <= 0) total = 1;
    for (int i = 0; i < PALETTE_N; i++) {
        out[i] = (float)weights[i] / (float)total;
        if (weights[i]) {
            if (*first < 0) *first = i;
            else if (*second < 0) *second = i;
            nonzero++;
        }
    }
    return nonzero;
}

static void linear_mix_lab(const float weights[PALETTE_N], float out[3])
{
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    for (int i = 0; i < PALETTE_N; i++) {
        out[0] += weights[i] * PALETTE_LAB[i][0];
        out[1] += weights[i] * PALETTE_LAB[i][1];
        out[2] += weights[i] * PALETTE_LAB[i][2];
    }
}

static void sort_pair_points(mix_pair_model_t *m)
{
    for (int i = 1; i < m->n; i++) {
        float t = m->t[i];
        float q[3] = {m->q[i][0], m->q[i][1], m->q[i][2]};
        int j = i - 1;
        while (j >= 0 && m->t[j] > t) {
            m->t[j + 1] = m->t[j];
            memcpy(m->q[j + 1], m->q[j], sizeof(q));
            j--;
        }
        m->t[j + 1] = t;
        memcpy(m->q[j + 1], q, sizeof(q));
    }
}

static void pair_eval_q(const mix_pair_model_t *m, float t, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!m || m->n == 0) return;
    if (m->n == 1 || t <= m->t[0]) {
        memcpy(out, m->q[0], sizeof(float) * 3);
        return;
    }
    if (t >= m->t[m->n - 1]) {
        memcpy(out, m->q[m->n - 1], sizeof(float) * 3);
        return;
    }
    for (int i = 0; i < m->n - 1; i++) {
        float t0 = m->t[i];
        float t1 = m->t[i + 1];
        if (t >= t0 && t <= t1) {
            float u = (t - t0) / (t1 - t0);
            for (int ch = 0; ch < 3; ch++) {
                out[ch] = m->q[i][ch] + (m->q[i + 1][ch] - m->q[i][ch]) * u;
            }
            return;
        }
    }
}

static void palette_mix_model_lab_pair_only(const float weights[PALETTE_N], float out[3])
{
    linear_mix_lab(weights, out);
    if (s_mix_enabled_count <= 0) return;

    float corr[3] = {0.0f, 0.0f, 0.0f};
    float phi_sum = 0.0f;
    for (int i = 0; i < PALETTE_N; i++) {
        if (weights[i] <= 0.0001f) continue;
        for (int j = i + 1; j < PALETTE_N; j++) {
            if (weights[j] <= 0.0001f) continue;
            const mix_pair_model_t *m = &s_pair_model[i][j];
            if (m->n == 0) continue;
            float pair_sum = weights[i] + weights[j];
            float t = pair_sum > 0.0f ? weights[j] / pair_sum : 0.5f;
            float q[3];
            pair_eval_q(m, t, q);
            float phi = 4.0f * weights[i] * weights[j];
            corr[0] += phi * q[0];
            corr[1] += phi * q[1];
            corr[2] += phi * q[2];
            phi_sum += phi;
        }
    }

    float scale = phi_sum > 1.0f ? 1.0f / phi_sum : 1.0f;
    out[0] += corr[0] * scale;
    out[1] += corr[1] * scale;
    out[2] += corr[2] * scale;
}

void palette_mix_model_lab(const float weights[PALETTE_N], float out[3])
{
    palette_mix_model_lab_pair_only(weights, out);
    if (s_mix_enabled_count <= 0) return;

    for (int k = 0; k < s_anchor_count; k++) {
        const mix_anchor_model_t *a = &s_anchor_model[k];
        if (!a->enabled) continue;
        float dist = 0.0f;
        for (int i = 0; i < PALETTE_N; i++) dist += fabsf(weights[i] - a->weights[i]);
        float strength = 1.0f - dist / 0.55f;
        if (strength <= 0.0f) continue;
        strength *= strength;
        out[0] += a->residual[0] * strength;
        out[1] += a->residual[1] * strength;
        out[2] += a->residual[2] * strength;
    }
}

void palette_mix_rebuild_model(void)
{
    memset(s_pair_model, 0, sizeof(s_pair_model));
    memset(s_anchor_model, 0, sizeof(s_anchor_model));
    s_anchor_count = 0;
    s_mix_enabled_count = 0;

    for (int p = 0; p < PALETTE_MIX_PATCH_N; p++) {
        palette_mix_patch_t *patch = &PALETTE_MIX_PATCHES[p];
        if (!patch->enabled) continue;
        s_mix_enabled_count++;

        float w[PALETTE_N];
        int first, second;
        int nonzero = normalise_patch_weights(patch->weights, w, &first, &second);
        if (nonzero == 2 && first >= 0 && second >= 0) {
            int i = first;
            int j = second;
            if (i > j) { int tmp = i; i = j; j = tmp; }
            mix_pair_model_t *m = &s_pair_model[i][j];
            if (m->n >= MIX_PAIR_MAX_POINTS) continue;

            float lin[3];
            linear_mix_lab(w, lin);
            float t = w[j] / (w[i] + w[j]);
            float phi = 4.0f * t * (1.0f - t);
            if (phi < 0.001f) phi = 0.001f;
            int n = m->n++;
            m->t[n] = t;
            for (int ch = 0; ch < 3; ch++) {
                m->q[n][ch] = (patch->lab[ch] - lin[ch]) / phi;
            }
        } else if (nonzero >= 3 && s_anchor_count < MIX_ANCHOR_MAX) {
            mix_anchor_model_t *a = &s_anchor_model[s_anchor_count++];
            memcpy(a->weights, w, sizeof(w));
            a->enabled = 1;
        }
    }

    for (int i = 0; i < PALETTE_N; i++) {
        for (int j = i + 1; j < PALETTE_N; j++) sort_pair_points(&s_pair_model[i][j]);
    }

    for (int k = 0; k < s_anchor_count; k++) {
        mix_anchor_model_t *a = &s_anchor_model[k];
        float pred[3];
        palette_mix_model_lab_pair_only(a->weights, pred);
        for (int p = 0; p < PALETTE_MIX_PATCH_N; p++) {
            if (!PALETTE_MIX_PATCHES[p].enabled) continue;
            float w[PALETTE_N];
            int first, second;
            int nonzero = normalise_patch_weights(PALETTE_MIX_PATCHES[p].weights, w, &first, &second);
            if (nonzero < 3) continue;
            int same = 1;
            for (int i = 0; i < PALETTE_N; i++) {
                if (fabsf(w[i] - a->weights[i]) > 0.0001f) { same = 0; break; }
            }
            if (!same) continue;
            for (int ch = 0; ch < 3; ch++) a->residual[ch] = PALETTE_MIX_PATCHES[p].lab[ch] - pred[ch];
            break;
        }
    }
}

void palette_mix_reset_defaults(void)
{
    // LEGACY: bit-exact pre-2026-05 path — sRGB roundtrip from MIX_DEFAULTS.rgb.
    // SCE_*: read the colorimeter's measured Lab directly and apply the
    // specular-floor correction.  Both stage the result in
    // PALETTE_MIX_PATCHES[].lab; downstream model code never branches.
    for (int i = 0; i < PALETTE_MIX_PATCH_N; i++) {
        memcpy(PALETTE_MIX_PATCHES[i].weights, MIX_DEFAULTS[i].weights, PALETTE_N);
        if (s_palette_mode == PALETTE_MODE_LEGACY) {
            rgb_to_lab_u8(MIX_DEFAULTS[i].rgb[0], MIX_DEFAULTS[i].rgb[1], MIX_DEFAULTS[i].rgb[2],
                          &PALETTE_MIX_PATCHES[i].lab[0],
                          &PALETTE_MIX_PATCHES[i].lab[1],
                          &PALETTE_MIX_PATCHES[i].lab[2]);
        } else {
            lab_apply_sce(MIX_PATCH_LAB_SCI[i], PALETTE_MIX_PATCHES[i].lab, s_specular_floor_k);
        }
        PALETTE_MIX_PATCHES[i].enabled = 1;
    }
    palette_mix_rebuild_model();
}

void palette_mix_clear(void)
{
    for (int i = 0; i < PALETTE_MIX_PATCH_N; i++) PALETTE_MIX_PATCHES[i].enabled = 0;
    palette_mix_rebuild_model();
}

void palette_mix_set_patch_lab(int patch_idx, const float lab[3])
{
    if (patch_idx < 0 || patch_idx >= PALETTE_MIX_PATCH_N || !lab) return;
    memcpy(PALETTE_MIX_PATCHES[patch_idx].lab, lab, sizeof(float) * 3);
    PALETTE_MIX_PATCHES[patch_idx].enabled = 1;
    palette_mix_rebuild_model();
}

int palette_mix_enabled_count(void)
{
    return s_mix_enabled_count;
}

void palette_init(void)
{
    if (s_palette_mode == PALETTE_MODE_LEGACY) {
        // Bit-exact pre-2026-05 behavior: sRGB roundtrip from
        // PALETTE_RGB_MEASURED.  The 8-bit projection loses ~0.5 ΔE versus
        // the CSV-baked Lab, but matches the previously calibrated output.
        for (int i = 0; i < PALETTE_N; i++) {
            rgb_to_lab_u8(PALETTE_RGB_MEASURED[i][0],
                          PALETTE_RGB_MEASURED[i][1],
                          PALETTE_RGB_MEASURED[i][2],
                          &PALETTE_LAB[i][0], &PALETTE_LAB[i][1], &PALETTE_LAB[i][2]);
        }
    } else {
        for (int i = 0; i < PALETTE_N; i++) {
            lab_apply_sce(PALETTE_LAB_SCI[i], PALETTE_LAB[i], s_specular_floor_k);
        }
    }
    palette_mix_reset_defaults();
}

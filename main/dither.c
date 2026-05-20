// dither.c — Vector-error diffusion in CIELAB with CIEDE2000 matching.
//
// Design notes (revision 9 — back-to-basics after rev-8 regression):
//
//   1. **Fixed-offset FS**.  Reverted from rev-8 mirrored serpentine back to
//      the rev-7 Python-compatible behaviour.  On backward rows the kernel
//      offsets are NOT mirrored, so the (+1,0)=7/16 weight diffuses
//      backwards into already-quantised pixels and is effectively lost.  In
//      the abstract this is "wrong"; in practice it is what the user has
//      been calibrating their eye against for weeks, and the mirrored
//      variant produced patterns they read as worse.  Visible artefact
//      character > textbook correctness for this user.
//
//   2. **Unweighted CIEDE2000** in the picker.  Rev-8's k_L=0.85 scaling
//      divided BOTH L1 and L2 by the same factor, which inflated ΔL
//      uniformly across all candidate palette entries — net effect on
//      the picker's ranking was negligible.  Removed for clarity.
//
//   3. **sRGB-domain averaging** in the 1.5× supersample path.  Browsers
//      downscale source images in display-sRGB space (no gamma-correct
//      resize), so the user's preview already shows the sRGB-averaged
//      version.  When the dither averaged in linear-light it produced
//      noticeably lighter mid-tones than the preview, which the user
//      read as "效果特别差" — the panel didn't look like the source they
//      saw on their phone.  We now average in sRGB to match the browser
//      preview's behaviour.  Reflective-ink physics says linear-light is
//      more correct; perception says match the preview.  Going with
//      perception.
//
//   4. **Solid-fill snap** (smoothness).  When the residual ΔE2000 between
//      the chosen palette entry and the target colour is small, we
//      attenuate the FS error diffusion toward zero so flat near-palette
//      regions don't accumulate enough error to dump a single contrasting
//      ink dot — eliminating sparse-speckle artefacts.  Mapping is
//      threshold_ΔE = smoothness * 0.4 (so default 30 → ΔE=12, where the
//      snap is actually visible on natural photos).
//
//   5. **1.5× supersample** with sRGB averaging.  Source at 600×900,
//      output at 400×600.  Each panel-pixel decision averages four
//      source pixels (weights summing to 2.25) before quantising — gives
//      the picker finer chroma/luma transitions to land on than direct
//      resize-to-panel.
//
//   6. Rolling 2-row error buffer in SRAM (~10 KiB at w=400) instead of
//      a full-image buffer in PSRAM (~5.7 MiB).  Memory drops 600× and
//      reads/writes hit cached SRAM.

#include "dither.h"
#include "palette.h"
#include "color_pipeline.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "dither";

// JS-visible progress counter — host code reads it via wasm_dither_progress.
volatile int dither_progress_row = 0;

// Stucki error diffusion weights.  This is a wider, smoother variant of
// Burkes: it preserves total error (sum=42/42) but sends part of it two rows
// down, producing a more even halftone texture on slow reflective media.
#define S8 (8.0f / 42.0f)
#define S4 (4.0f / 42.0f)
#define S2 (2.0f / 42.0f)
#define S1 (1.0f / 42.0f)

static inline float clamp_err_component(float v)
{
    if (v < -24.0f) return -24.0f;
    if (v >  24.0f) return  24.0f;
    return v;
}

static inline void add_err(float *row, int x, int w, float wt,
                           float dL, float da, float db)
{
    if (x < 0 || x >= w) return;
    float *t = row + x * 3;
    t[0] = clamp_err_component(t[0] + dL * wt);
    t[1] = clamp_err_component(t[1] + da * wt);
    t[2] = clamp_err_component(t[2] + db * wt);
}

// Lightweight region labels used as a WASM-friendly substitute for semantic
// segmentation.  They are deliberately low-dimensional and deterministic: the
// goal is not object recognition, but selecting better halftone behaviour for
// edges, flat gradients, skin/warm areas, sky/cool areas, shadows, and paper
// highlights.
#define REG_EDGE      0x01
#define REG_FLAT      0x02
#define REG_WARM_SKIN 0x04
#define REG_SKY_COOL  0x08
#define REG_DARK      0x10
#define REG_LIGHT     0x20

static inline uint8_t classify_region_rgb(uint8_t r, uint8_t g, uint8_t b, float edge)
{
    int maxc = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int minc = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int chroma = maxc - minc;
    int y = (77 * r + 150 * g + 29 * b + 128) >> 8;
    uint8_t reg = 0;

    if (edge > 46.0f) reg |= REG_EDGE;
    if (edge < 12.0f && chroma < 42) reg |= REG_FLAT;
    if (y < 58) reg |= REG_DARK;
    if (y > 196) reg |= REG_LIGHT;

    // Broad warm/skin detector: includes faces, hands, warm wood, food.  On
    // this panel those areas should be made from white/yellow/red, not blue or
    // green dots, unless the local colour evidence is overwhelming.
    if (r > 78 && g > 44 && r >= g && g >= b - 8 && r - b > 18 && chroma > 20) {
        reg |= REG_WARM_SKIN;
    }

    // Broad sky/cool detector: favours white/blue mixtures and suppresses
    // isolated green/yellow pollution in smooth cool gradients.
    if (b > 72 && b >= r + 10 && b >= g - 4 && y > 72 && chroma > 16) {
        reg |= REG_SKY_COOL;
    }

    return reg;
}

static inline float region_palette_bias(uint8_t reg, float L, int p)
{
    float b = 0.0f;
    if (reg & REG_WARM_SKIN) {
        if (p == PIDX_BLUE)  b += 2.20f;
        if (p == PIDX_GREEN) b += 1.55f;
        if (p == PIDX_RED)   b -= 0.20f;
        if (p == PIDX_YELLOW)b -= 0.14f;
        if (p == PIDX_WHITE) b -= 0.08f;
    }
    if (reg & REG_SKY_COOL) {
        if (p == PIDX_GREEN) b += 1.25f;
        if (p == PIDX_YELLOW)b += 0.75f;
        if (p == PIDX_BLUE)  b -= 0.16f;
        if (p == PIDX_WHITE) b -= 0.10f;
    }
    if (reg & REG_LIGHT) {
        if (p == PIDX_BLACK) b += 0.90f;
    }
    if (reg & REG_DARK) {
        if (p == PIDX_WHITE) b += 0.35f;
    }

    // Ink visibility is asymmetric on reflective paper.  On light paper,
    // yellow dots are low-contrast, while black/red/blue dots are conspicuous.
    // This term biases ambiguous choices toward less visible inks in highlights
    // and flat regions, without overriding clear colour evidence.
    if (L > 55.0f) {
        float k = (L - 55.0f) / 18.0f;
        if (k > 1.0f) k = 1.0f;
        if (reg & REG_FLAT) k *= 1.35f;
        if (p == PIDX_BLACK)  b += 0.95f * k;
        if (p == PIDX_RED)    b += 0.62f * k;
        if (p == PIDX_BLUE)   b += 0.55f * k;
        if (p == PIDX_GREEN)  b += 0.24f * k;
        if (p == PIDX_YELLOW) b -= 0.10f * k;
    } else if (L < 38.0f) {
        float k = (38.0f - L) / 8.0f;
        if (k > 1.0f) k = 1.0f;
        if (p == PIDX_WHITE)  b += 0.42f * k;
        if (p == PIDX_YELLOW) b += 0.20f * k;
    }
    return b;
}

// -------------------------------------------------------- source appearance
//
// The panel is reflective and very low dynamic range: measured white is only
// L*~65 and measured black is L*~30 under the user's light source.  Comparing
// camera/display sRGB Lab directly against the measured ink Lab therefore
// makes every highlight carry a huge, impossible "make this whiter" error.
// Before quantisation we compress the source appearance into the measured
// paper gamut, then run CIEDE2000/error diffusion in that same display space.
//
// Chroma is deliberately compressed rather than copied.  The six inks form a
// small, anisotropic gamut; preserving source hue at roughly 45% chroma gives
// the picker enough hue signal for red/yellow/blue/green decisions without
// letting unreachable phone-screen saturation dominate luma and texture.
static inline void map_source_to_panel_lab(float *L, float *a, float *b)
{
    const float Lb = PALETTE_LAB[PIDX_BLACK][0];
    const float ab = PALETTE_LAB[PIDX_BLACK][1];
    const float bb = PALETTE_LAB[PIDX_BLACK][2];
    const float Lw = PALETTE_LAB[PIDX_WHITE][0];
    const float aw = PALETTE_LAB[PIDX_WHITE][1];
    const float bw = PALETTE_LAB[PIDX_WHITE][2];

    float t = *L / 100.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    // Slight toe lift keeps shadow detail from collapsing into black ink.
    t = powf(t, 0.92f);

    float neutral_a = ab + (aw - ab) * t;
    float neutral_b = bb + (bw - bb) * t;

    *L = Lb + (Lw - Lb) * t;
    *a = neutral_a + (*a * 0.44f);
    *b = neutral_b + (*b * 0.44f);
}

// ----------------------------------------------------------------- picker
static inline int pick_palette_spatial(float L, float a, float b, uint8_t reg,
                                       int left, int up, int up_left, int up_right)
{
    float raw[PALETTE_N];
    int raw_best = 0;
    float raw_best_d = FLT_MAX;

    for (int p = 0; p < PALETTE_N; p++) {
        float d = ciede2000(L, a, b,
                            PALETTE_LAB[p][0],
                            PALETTE_LAB[p][1],
                            PALETTE_LAB[p][2]);
        raw[p] = d;
        if (d < raw_best_d) { raw_best_d = d; raw_best = p; }
    }

    // Do not disturb true solids; otherwise add a tiny same-neighbour cost so
    // close calls prefer a balanced local mix instead of clumping identical dots.
    if (raw_best_d < 2.0f) return raw_best;

    int best_idx = raw_best;
    float best_d = FLT_MAX;
    for (int p = 0; p < PALETTE_N; p++) {
        float penalty = 0.0f;
        if (p == left)     penalty += 0.50f;
        if (p == up)       penalty += 0.50f;
        if (p == up_left)  penalty += 0.22f;
        if (p == up_right) penalty += 0.22f;
        float cluster_scale = (reg & REG_FLAT) ? 1.35f : ((reg & REG_EDGE) ? 0.55f : 1.0f);
        float d = raw[p] + penalty * cluster_scale + region_palette_bias(reg, L, p);
        if (d < best_d) { best_d = d; best_idx = p; }
    }
    return best_idx;
}

// ------------------------------------------------------ E6 physical picker
//
// Classic mode above is a colour-distance dither with modest spatial cleanup.
// The E6 mode adds a display-physics bias: Spectra 6 dots are not equal on
// paper.  A single black/red/blue particle is much more visible in a highlight
// than a yellow or white decision, and warm/skin/sky regions are easily ruined
// by one wrong complementary ink.  These terms only affect close calls; strong
// colour evidence still wins through the CIEDE2000 term.

static inline float e6_palette_bias(uint8_t reg, float L,
                                    uint8_t r, uint8_t g, uint8_t b, int p)
{
    float bias = region_palette_bias(reg, L, p) * 0.72f;
    int maxc = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int minc = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int chroma = maxc - minc;
    int y = (77 * r + 150 * g + 29 * b + 128) >> 8;

    if (chroma < 22 && y > 112) {
        float k = (float)(y - 112) / 88.0f;
        if (k > 1.0f) k = 1.0f;
        if (p == PIDX_BLACK)  bias += 1.20f * k;
        if (p == PIDX_RED)    bias += 0.74f * k;
        if (p == PIDX_BLUE)   bias += 0.62f * k;
        if (p == PIDX_GREEN)  bias += 0.26f * k;
        if (p == PIDX_YELLOW) bias -= 0.14f * k;
        if (p == PIDX_WHITE)  bias -= 0.10f * k;
    }

    if ((reg & REG_WARM_SKIN) || (r > g + 14 && r > b + 10 && chroma > 18)) {
        if (p == PIDX_BLUE)  bias += 1.25f;
        if (p == PIDX_GREEN) bias += 0.92f;
        if (p == PIDX_RED)   bias -= 0.24f;
        if (p == PIDX_YELLOW)bias -= 0.14f;
    }
    if ((reg & REG_SKY_COOL) || (b > r + 14 && b >= g + 4 && chroma > 18)) {
        if (p == PIDX_RED)    bias += 1.22f;
        if (p == PIDX_YELLOW) bias += 0.78f;
        if (p == PIDX_GREEN)  bias += 0.38f;
        if (p == PIDX_BLUE)   bias -= 0.28f;
        if (p == PIDX_WHITE)  bias -= 0.10f;
    }
    if (g > r + 12 && g >= b - 3 && chroma > 18) {
        if (p == PIDX_RED)    bias += 1.08f;
        if (p == PIDX_BLUE)   bias += 0.38f;
        if (p == PIDX_GREEN)  bias -= 0.32f;
        if (p == PIDX_YELLOW) bias += 0.10f;
    }

    return bias;
}

static inline float e6_screen_tiebreak(int x, int y, int p)
{
    static const uint8_t bayer8[64] = {
        0,48,12,60, 3,51,15,63,
       32,16,44,28,35,19,47,31,
        8,56, 4,52,11,59, 7,55,
       40,24,36,20,43,27,39,23,
        2,50,14,62, 1,49,13,61,
       34,18,46,30,33,17,45,29,
       10,58, 6,54, 9,57, 5,53,
       42,26,38,22,41,25,37,21
    };
    int v = bayer8[((y & 7) << 3) | (x & 7)];
    // Rotate the matrix per ink so ties do not make all inks share one grid.
    v = (v + p * 11) & 63;
    return ((float)v / 63.0f - 0.5f) * 0.10f;
}

static inline int pick_palette_e6_mix(float L, float a, float b, uint8_t reg,
                                      uint8_t r, uint8_t g, uint8_t bb,
                                      int x, int y,
                                      int left, int up, int up_left, int up_right)
{
    float raw[PALETTE_N];
    int raw_best = 0;
    float raw_best_d = FLT_MAX;
    for (int p = 0; p < PALETTE_N; p++) {
        float d = ciede2000(L, a, b,
                            PALETTE_LAB[p][0],
                            PALETTE_LAB[p][1],
                            PALETTE_LAB[p][2]);
        raw[p] = d;
        if (d < raw_best_d) { raw_best_d = d; raw_best = p; }
    }
    if (raw_best_d < 1.45f) return raw_best;

    int best_idx = raw_best;
    float best_d = FLT_MAX;
    for (int p = 0; p < PALETTE_N; p++) {
        float same = 0.0f;
        if (p == left)     same += 0.70f;
        if (p == up)       same += 0.70f;
        if (p == up_left)  same += 0.28f;
        if (p == up_right) same += 0.28f;

        float cluster_scale = (reg & REG_FLAT) ? 1.55f : ((reg & REG_EDGE) ? 0.42f : 1.05f);
        float d = raw[p]
                + same * cluster_scale
                + e6_palette_bias(reg, L, r, g, bb, p)
                + e6_screen_tiebreak(x, y, p);
        if (p == raw_best) d -= 0.08f;
        if (d < best_d) { best_d = d; best_idx = p; }
    }
    return best_idx;
}

// --------------------------------------------------------- smoothness helper
//
// smoothness (0..100) maps to a ΔE2000 threshold via `smoothness * 0.4`:
//
//     0   → 0    (snap disabled, pure FS)
//    30   → 12   (default — flat near-palette regions snap to pure ink)
//   100   → 40   (aggressive — image becomes near-posterised)
//
// A linear ramp from 0.5*threshold to threshold softens the transition so
// the boundary between snapped and dithered regions doesn't form a hard
// edge.  See header note (4) and the inline ciede2000() call below.

#define SMOOTHNESS_SCALE 0.4f

static inline void smoothness_thresholds(int smoothness, float *t_hard, float *t_soft)
{
    if (smoothness < 0)   smoothness = 0;
    if (smoothness > 100) smoothness = 100;
    *t_soft = (float)smoothness * SMOOTHNESS_SCALE;
    *t_hard = 0.5f * (*t_soft);
}

static inline float diffuse_factor(float dE, float t_hard, float t_soft)
{
    if (t_soft <= 0.0f) return 1.0f;
    if (dE <= t_hard)   return 0.0f;
    if (dE >= t_soft)   return 1.0f;
    return (dE - t_hard) / (t_soft - t_hard);
}

// ------------------------------------------------------------- packed write
static inline void pack_nibble(uint8_t *out, int x, int y, int w, uint8_t code)
{
    size_t byte_idx = ((size_t)y * w + x) >> 1;
    if ((x & 1) == 0) {
        out[byte_idx] = (out[byte_idx] & 0x0F) | (code << 4);
    } else {
        out[byte_idx] = (out[byte_idx] & 0xF0) | (code & 0x0F);
    }
}

// --------------------------------------------- edge-aware Stucki diffuser
//
// Stucki kernel, mirrored on serpentine rows:
//
//              x+1  x+2          8/42  4/42
// x-2 x-1  x   x+1  x+2    2/42  4/42  8/42  4/42 2/42
// x-2 x-1  x   x+1  x+2    1/42  2/42  4/42  2/42 1/42
//
// On strong edges we attenuate the second future row.  Mathematically this is
// a space-varying low-pass: flat gradients get long-range error distribution
// for smooth transitions; high gradients keep error local to avoid bleeding
// colour across contours and reducing apparent resolution.

static inline float source_edge_strength(const uint8_t *rgb, int w, int h, int x, int y)
{
    int xl = x > 0 ? x - 1 : x;
    int xr = x < w - 1 ? x + 1 : x;
    int yu = y > 0 ? y - 1 : y;
    int yd = y < h - 1 ? y + 1 : y;
    const uint8_t *pl = rgb + (y * w + xl) * 3;
    const uint8_t *pr = rgb + (y * w + xr) * 3;
    const uint8_t *pu = rgb + (yu * w + x) * 3;
    const uint8_t *pd = rgb + (yd * w + x) * 3;
    float yl = 0.299f * pl[0] + 0.587f * pl[1] + 0.114f * pl[2];
    float yr = 0.299f * pr[0] + 0.587f * pr[1] + 0.114f * pr[2];
    float yu8 = 0.299f * pu[0] + 0.587f * pu[1] + 0.114f * pu[2];
    float yd8 = 0.299f * pd[0] + 0.587f * pd[1] + 0.114f * pd[2];
    return fabsf(yr - yl) + fabsf(yd8 - yu8);
}

static inline void stucki_diffuse(float *err_cur, float *err_nxt, float *err_nxt2,
                                  int x, int w, int h, int y, int dir, float edge,
                                  float dL, float da, float db)
{
    // Full far-row diffusion in flat areas; fade it out over strong edges.
    // Preserve total error energy as the far row fades.  Earlier revisions
    // simply removed far-row weights on edges, losing up to 10/42 of the
    // residual and causing local tone drift.  Here the near weights are
    // renormalised so DC tone remains stable while diffusion distance adapts.
    float far = 1.0f - ((edge - 14.0f) / 58.0f);
    if (far < 0.0f) far = 0.0f;
    if (far > 1.0f) far = 1.0f;
    const float norm = 42.0f / (32.0f + 10.0f * far);

    add_err(err_cur, x + dir,     w, S8 * norm, dL, da, db);
    add_err(err_cur, x + 2 * dir, w, S4 * norm, dL, da, db);

    if (y + 1 < h) {
        add_err(err_nxt, x - 2 * dir, w, S2 * norm, dL, da, db);
        add_err(err_nxt, x - dir,     w, S4 * norm, dL, da, db);
        add_err(err_nxt, x,           w, S8 * norm, dL, da, db);
        add_err(err_nxt, x + dir,     w, S4 * norm, dL, da, db);
        add_err(err_nxt, x + 2 * dir, w, S2 * norm, dL, da, db);
    }
    if (far > 0.0f && y + 2 < h) {
        add_err(err_nxt2, x - 2 * dir, w, S1 * far * norm, dL, da, db);
        add_err(err_nxt2, x - dir,     w, S2 * far * norm, dL, da, db);
        add_err(err_nxt2, x,           w, S4 * far * norm, dL, da, db);
        add_err(err_nxt2, x + dir,     w, S2 * far * norm, dL, da, db);
        add_err(err_nxt2, x + 2 * dir, w, S1 * far * norm, dL, da, db);
    }
}

// ---------------------------------------------- iterative local refinement
//
// Initial error diffusion is causal: it only sees the past.  This second stage
// is non-causal and optimises the final halftone as a local-average problem.
// Each proposed colour change is accepted only when the 5x5 neighbourhood's
// average Lab moves closer to the target average, with small penalties for
// changing stable pixels and for adding same-colour neighbours.

#define REFINE_RADIUS 2
#define REFINE_ITERS  2

static inline float lab_avg_error5(const float *target_lab, const uint8_t *idx,
                                   int w, int h, int x, int y, int cand)
{
    float tL = 0.0f, ta = 0.0f, tb = 0.0f;
    float oL = 0.0f, oa = 0.0f, ob = 0.0f;
    float ws = 0.0f;

    for (int yy = y - REFINE_RADIUS; yy <= y + REFINE_RADIUS; yy++) {
        if (yy < 0 || yy >= h) continue;
        int dy = yy > y ? yy - y : y - yy;
        for (int xx = x - REFINE_RADIUS; xx <= x + REFINE_RADIUS; xx++) {
            if (xx < 0 || xx >= w) continue;
            int dx = xx > x ? xx - x : x - xx;
            float wt = (float)((REFINE_RADIUS + 1 - dx) * (REFINE_RADIUS + 1 - dy));
            const float *tl = target_lab + ((size_t)yy * w + xx) * 3;
            int pi = (xx == x && yy == y) ? cand : idx[(size_t)yy * w + xx];
            tL += tl[0] * wt; ta += tl[1] * wt; tb += tl[2] * wt;
            oL += PALETTE_LAB[pi][0] * wt;
            oa += PALETTE_LAB[pi][1] * wt;
            ob += PALETTE_LAB[pi][2] * wt;
            ws += wt;
        }
    }

    float inv = ws > 0.0f ? 1.0f / ws : 1.0f;
    float dL = (tL - oL) * inv;
    float da = (ta - oa) * inv;
    float db = (tb - ob) * inv;
    return dL*dL * 1.20f + (da*da + db*db) * 0.58f;
}

static inline float neighbour_cluster_cost(const uint8_t *idx, int w, int h,
                                           int x, int y, int cand)
{
    float c = 0.0f;
    for (int yy = y - 1; yy <= y + 1; yy++) {
        if (yy < 0 || yy >= h) continue;
        for (int xx = x - 1; xx <= x + 1; xx++) {
            if (xx < 0 || xx >= w || (xx == x && yy == y)) continue;
            if (idx[(size_t)yy * w + xx] == cand) c += 1.0f;
        }
    }
    return c;
}

static void refine_indices_local(const float *target_lab, const uint8_t *region, uint8_t *idx, int w, int h)
{
    for (int it = 0; it < REFINE_ITERS; it++) {
        int changes = 0;
        for (int y = 0; y < h; y++) {
            int x0 = (it & 1) ? w - 1 : 0;
            int xe = (it & 1) ? -1 : w;
            int xs = (it & 1) ? -1 : 1;
            for (int x = x0; x != xe; x += xs) {
                size_t pos = (size_t)y * w + x;
                int old = idx[pos];
                uint8_t reg = region[pos];
                const float *tc = target_lab + pos * 3;

                float center_de = ciede2000(tc[0], tc[1], tc[2],
                                            PALETTE_LAB[old][0],
                                            PALETTE_LAB[old][1],
                                            PALETTE_LAB[old][2]);
                if (center_de < 1.8f) continue;

                float cluster_w = (reg & REG_FLAT) ? 0.060f : ((reg & REG_EDGE) ? 0.012f : 0.035f);
                float improve_eps = (reg & REG_EDGE) ? 0.055f : 0.020f;
                float max_pixel_loss = (reg & REG_EDGE) ? 4.0f : ((reg & REG_FLAT) ? 13.0f : 9.0f);

                float best_score = lab_avg_error5(target_lab, idx, w, h, x, y, old)
                                 + neighbour_cluster_cost(idx, w, h, x, y, old) * cluster_w
                                 + region_palette_bias(reg, tc[0], old) * 0.12f;
                int best = old;

                for (int p = 0; p < PALETTE_N; p++) {
                    if (p == old) continue;
                    float pixel_de = ciede2000(tc[0], tc[1], tc[2],
                                               PALETTE_LAB[p][0],
                                               PALETTE_LAB[p][1],
                                               PALETTE_LAB[p][2]);
                    // Avoid sacrificing a strong local colour for only a tiny
                    // neighbourhood-average improvement.
                    if (pixel_de > center_de + max_pixel_loss) continue;

                    float score = lab_avg_error5(target_lab, idx, w, h, x, y, p)
                                + neighbour_cluster_cost(idx, w, h, x, y, p) * cluster_w
                                + region_palette_bias(reg, tc[0], p) * 0.12f
                                + 0.045f;
                    if (score < best_score - improve_eps) {
                        best_score = score;
                        best = p;
                    }
                }

                if (best != old) {
                    idx[pos] = (uint8_t)best;
                    changes++;
                }
            }
        }
        ESP_LOGI(TAG, "refine pass %d changes=%d", it + 1, changes);
        if (changes == 0) break;
    }
}

static void repack_indices(const uint8_t *idx, int w, int h, uint8_t *out_packed)
{
    const size_t packed_bytes = ((size_t)w * h + 1u) / 2u;
    memset(out_packed, 0, packed_bytes);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            pack_nibble(out_packed, x, y, w, PALETTE_INK_CODE[idx[(size_t)y * w + x]]);
        }
    }
}

// ----------------------------------------------------- pure flat-fill entry
//
// "Russian" is a non-halftone pipeline inspired by Soviet constructivist
// posters: limited ink set, hard figure/ground contrast, strong reds/blacks,
// and clean colour fields.  The important colour-science constraint is that
// this is still scored in the measured reflective panel Lab space; the style
// bias is deliberately smaller than the CIEDE2000 term, so strong green/blue
// evidence remains green/blue instead of repeating the earlier red-cast bug.

static inline float constructivist_palette_bias(uint8_t reg, float L,
                                                uint8_t r, uint8_t g, uint8_t b,
                                                int p)
{
    float bias = region_palette_bias(reg, L, p) * 0.22f;

    // Constructivist visual grammar: paper, black structure, red accents,
    // yellow secondary planes.  Blue/green are allowed but must be earned by
    // source hue because they are less central to the style.
    if (p == PIDX_RED)    bias -= 0.28f;
    if (p == PIDX_BLACK)  bias -= 0.18f;
    if (p == PIDX_YELLOW) bias -= 0.12f;
    if (p == PIDX_WHITE)  bias -= 0.06f;
    if (p == PIDX_BLUE)   bias += 0.20f;
    if (p == PIDX_GREEN)  bias += 0.26f;

    int maxc = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int minc = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int chroma = maxc - minc;
    int y = (77 * r + 150 * g + 29 * b + 128) >> 8;

    // Guardrails against hue inversion.  Red ink is very conspicuous on this
    // panel, so only use it for genuinely warm/red evidence or dark accents.
    if (g > r + 14 && g >= b - 4 && chroma > 18) {
        if (p == PIDX_RED)    bias += 2.40f;
        if (p == PIDX_GREEN)  bias -= 0.48f;
        if (p == PIDX_YELLOW) bias += 0.20f;
    }
    if (b > r + 16 && b > g + 6 && chroma > 18) {
        if (p == PIDX_RED)    bias += 2.10f;
        if (p == PIDX_YELLOW) bias += 0.70f;
        if (p == PIDX_BLUE)   bias -= 0.45f;
    }
    if (r > g + 18 && r > b + 12 && chroma > 20) {
        if (p == PIDX_RED)    bias -= 0.52f;
        if (p == PIDX_GREEN)  bias += 0.90f;
        if (p == PIDX_BLUE)   bias += 0.70f;
    }

    // Human visibility asymmetry on reflective white: yellow is low-contrast,
    // red/blue/black dots are high-contrast.  Since this preset is flat-fill,
    // ambiguous highlights should become white/yellow planes, not red noise.
    if (L > 54.0f || y > 170) {
        float k = (L > 54.0f) ? (L - 54.0f) / 18.0f : 0.6f;
        if (k > 1.0f) k = 1.0f;
        if (p == PIDX_BLACK)  bias += 1.15f * k;
        if (p == PIDX_RED)    bias += 0.90f * k;
        if (p == PIDX_BLUE)   bias += 0.72f * k;
        if (p == PIDX_GREEN)  bias += 0.30f * k;
        if (p == PIDX_YELLOW) bias -= 0.14f * k;
        if (p == PIDX_WHITE)  bias -= 0.10f * k;
    }

    return bias;
}

static inline int pick_constructivist_flat(float L, float a, float b,
                                           uint8_t reg,
                                           uint8_t r, uint8_t g, uint8_t bb)
{
    int best = 0;
    float best_score = FLT_MAX;
    for (int p = 0; p < PALETTE_N; p++) {
        float d = ciede2000(L, a, b,
                            PALETTE_LAB[p][0],
                            PALETTE_LAB[p][1],
                            PALETTE_LAB[p][2]);
        float score = d + constructivist_palette_bias(reg, L, r, g, bb, p);
        if (score < best_score) {
            best_score = score;
            best = p;
        }
    }
    return best;
}

static void cleanup_constructivist_blocks(const float *target_lab,
                                          const float *edge_map,
                                          uint8_t *idx, int w, int h)
{
    for (int it = 0; it < 3; it++) {
        int changes = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t pos = (size_t)y * w + x;
                int old = idx[pos];
                int counts[PALETTE_N] = {0};
                for (int yy = y - 1; yy <= y + 1; yy++) {
                    if (yy < 0 || yy >= h) continue;
                    for (int xx = x - 1; xx <= x + 1; xx++) {
                        if (xx < 0 || xx >= w) continue;
                        counts[idx[(size_t)yy * w + xx]]++;
                    }
                }

                int mode = old;
                int mode_count = counts[old];
                for (int p = 0; p < PALETTE_N; p++) {
                    if (counts[p] > mode_count) {
                        mode_count = counts[p];
                        mode = p;
                    }
                }
                if (mode == old) continue;

                const float *tc = target_lab + pos * 3;
                float old_de = ciede2000(tc[0], tc[1], tc[2],
                                         PALETTE_LAB[old][0],
                                         PALETTE_LAB[old][1],
                                         PALETTE_LAB[old][2]);
                float new_de = ciede2000(tc[0], tc[1], tc[2],
                                         PALETTE_LAB[mode][0],
                                         PALETTE_LAB[mode][1],
                                         PALETTE_LAB[mode][2]);

                float edge = edge_map[pos];
                float allowance = edge > 54.0f ? 1.6f : (edge > 26.0f ? 3.2f : 6.4f);
                int old_count = counts[old];
                if (mode_count >= 5 && old_count <= 3 && new_de <= old_de + allowance) {
                    idx[pos] = (uint8_t)mode;
                    changes++;
                }
            }
        }
        if (changes == 0) break;
    }
}

int flat_fill_constructivist(const uint8_t *in_rgb888, int w, int h,
                             uint8_t *out_packed,
                             uint8_t *out_idx_opt)
{
    uint8_t *idx = (uint8_t *)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float *target_lab = (float *)heap_caps_malloc((size_t)w * h * 3 * sizeof(float),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float *edge_map = (float *)heap_caps_malloc((size_t)w * h * sizeof(float),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!idx || !target_lab || !edge_map) {
        ESP_LOGE(TAG, "flat-fill: alloc failed");
        free(idx);
        free(target_lab);
        free(edge_map);
        return -1;
    }

    for (int y = 0; y < h; y++) {
        dither_progress_row = y;
        for (int x = 0; x < w; x++) {
            size_t pos = (size_t)y * w + x;
            const uint8_t *src = in_rgb888 + pos * 3;
            float edge = source_edge_strength(in_rgb888, w, h, x, y);
            uint8_t reg = classify_region_rgb(src[0], src[1], src[2], edge);
            float L, a, b;
            rgb_to_lab_u8(src[0], src[1], src[2], &L, &a, &b);
            map_source_to_panel_lab(&L, &a, &b);
            target_lab[pos * 3 + 0] = L;
            target_lab[pos * 3 + 1] = a;
            target_lab[pos * 3 + 2] = b;
            edge_map[pos] = edge;
            idx[pos] = (uint8_t)pick_constructivist_flat(L, a, b, reg, src[0], src[1], src[2]);
        }
    }

    cleanup_constructivist_blocks(target_lab, edge_map, idx, w, h);
    repack_indices(idx, w, h, out_packed);
    if (out_idx_opt) memcpy(out_idx_opt, idx, (size_t)w * h);

    free(idx);
    free(target_lab);
    free(edge_map);
    return 0;
}

// ----------------------------------------------------------- 1×1 entry point
static int dither_fs_core(const uint8_t *in_rgb888, int w, int h,
                          uint8_t *out_packed,
                          uint8_t *out_idx_opt,
                          int smoothness,
                          bool e6_mix_mode)
{
    const size_t row_bytes = (size_t)w * 3 * sizeof(float);
    float *err_cur = (float *)heap_caps_calloc(1, row_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float *err_nxt = (float *)heap_caps_calloc(1, row_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float *err_nxt2 = (float *)heap_caps_calloc(1, row_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *idx_prev = (uint8_t *)heap_caps_malloc((size_t)w, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *idx_cur = (uint8_t *)heap_caps_malloc((size_t)w, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *idx_all = (uint8_t *)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *region = (uint8_t *)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float *target_lab = (float *)heap_caps_malloc((size_t)w * h * 3 * sizeof(float),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!err_cur || !err_nxt || !err_nxt2 || !idx_prev || !idx_cur || !idx_all || !region || !target_lab) {
        ESP_LOGE(TAG, "no internal RAM for dither buffers (%u-byte rows)", (unsigned)row_bytes);
        free(err_cur);
        free(err_nxt);
        free(err_nxt2);
        free(idx_prev);
        free(idx_cur);
        free(idx_all);
        free(region);
        free(target_lab);
        return -1;
    }
    memset(idx_prev, 0xFF, (size_t)w);
    memset(idx_cur, 0xFF, (size_t)w);

    const size_t packed_bytes = ((size_t)w * h + 1u) / 2u;
    memset(out_packed, 0, packed_bytes);

    float t_hard, t_soft;
    smoothness_thresholds(smoothness, &t_hard, &t_soft);

    bool serp = false;
    dither_progress_row = 0;

    for (int y = 0; y < h; y++) {
        dither_progress_row = y;
        int xstart, xend, xstep;
        if (!serp) { xstart = 0;     xend = w;   xstep =  1; }
        else       { xstart = w - 1; xend = -1;  xstep = -1; }
        const int dir = xstep;

        for (int x = xstart; x != xend; x += xstep) {
            const uint8_t *src = in_rgb888 + (y * w + x) * 3;
            float edge = source_edge_strength(in_rgb888, w, h, x, y);
            uint8_t reg = classify_region_rgb(src[0], src[1], src[2], edge);
            region[(size_t)y * w + x] = reg;

            float L, a, b;
            rgb_to_lab_u8(src[0], src[1], src[2], &L, &a, &b);
            map_source_to_panel_lab(&L, &a, &b);
            float *tl = target_lab + ((size_t)y * w + x) * 3;
            tl[0] = L; tl[1] = a; tl[2] = b;

            float *ec = err_cur + x * 3;
            L += ec[0]; a += ec[1]; b += ec[2];

            int left_idx = (x - xstep >= 0 && x - xstep < w) ? idx_cur[x - xstep] : -1;
            int up_idx = (y > 0) ? idx_prev[x] : -1;
            int up_left_idx = (y > 0 && x - xstep >= 0 && x - xstep < w) ? idx_prev[x - xstep] : -1;
            int up_right_idx = (y > 0 && x + xstep >= 0 && x + xstep < w) ? idx_prev[x + xstep] : -1;
            int best_idx = e6_mix_mode
                ? pick_palette_e6_mix(L, a, b, reg, src[0], src[1], src[2],
                                      x, y, left_idx, up_idx, up_left_idx, up_right_idx)
                : pick_palette_spatial(L, a, b, reg,
                                       left_idx, up_idx, up_left_idx, up_right_idx);

            idx_cur[x] = (uint8_t)best_idx;
            idx_all[(size_t)y * w + x] = (uint8_t)best_idx;
            if (out_idx_opt) out_idx_opt[y * w + x] = (uint8_t)best_idx;
            pack_nibble(out_packed, x, y, w, PALETTE_INK_CODE[best_idx]);

            float dL = L - PALETTE_LAB[best_idx][0];
            float da = a - PALETTE_LAB[best_idx][1];
            float db = b - PALETTE_LAB[best_idx][2];

            if (t_soft > 0.0f) {
                float dE = ciede2000(L, a, b,
                                     PALETTE_LAB[best_idx][0],
                                     PALETTE_LAB[best_idx][1],
                                     PALETTE_LAB[best_idx][2]);
                float f = diffuse_factor(dE, t_hard, t_soft);
                dL *= f; da *= f; db *= f;
            }

            stucki_diffuse(err_cur, err_nxt, err_nxt2, x, w, h, y, dir, edge, dL, da, db);
        }

        float *tmp = err_cur; err_cur = err_nxt; err_nxt = err_nxt2; err_nxt2 = tmp;
        memset(err_nxt2, 0, row_bytes);
        uint8_t *itmp = idx_prev; idx_prev = idx_cur; idx_cur = itmp;
        memset(idx_cur, 0xFF, (size_t)w);
        serp = !serp;

        if ((y & 0x1F) == 0) {
            ESP_LOGI(TAG, "%s dither row %d/%d",
                     e6_mix_mode ? "e6" : "classic", y, h);
        }
    }

    refine_indices_local(target_lab, region, idx_all, w, h);
    repack_indices(idx_all, w, h, out_packed);
    if (out_idx_opt) memcpy(out_idx_opt, idx_all, (size_t)w * h);

    free(err_cur);
    free(err_nxt);
    free(err_nxt2);
    free(idx_prev);
    free(idx_cur);
    free(idx_all);
    free(region);
    free(target_lab);
    return 0;
}

int dither_ved_fs(const uint8_t *in_rgb888, int w, int h,
                  uint8_t *out_packed,
                  uint8_t *out_idx_opt,
                  int smoothness)
{
    return dither_fs_core(in_rgb888, w, h, out_packed, out_idx_opt,
                          smoothness, false);
}

int dither_e6_mix_fs(const uint8_t *in_rgb888, int w, int h,
                     uint8_t *out_packed,
                     uint8_t *out_idx_opt,
                     int smoothness)
{
    return dither_fs_core(in_rgb888, w, h, out_packed, out_idx_opt,
                          smoothness, true);
}

// ----------------------------------------------------- 1.5× supersample path
//
// The earlier revision averaged 4 source pixels per panel pixel with a
// hand-rolled box filter.  Box-filtering after the browser had already
// done a Lanczos resize from the phone photo amounted to chaining two
// low-pass steps, which left the output noticeably softer than asking
// the browser for 400×600 in one shot.  This revision replaces the inner
// downsample with a 4×4 **Catmull-Rom** cubic kernel (a = -0.5) which has
// slight negative side lobes that preserve edge contrast — same family
// the browser already uses, but applied to a 1.5× pre-resize so the
// dither sees finer chroma detail than a single resampling step alone
// would deliver.  After the downsample we run a mild unsharp mask
// (radius 1, amount 0.3) to compensate for the residual softening
// introduced by any two-step resize chain.
//
// Algorithm:
//   1. Catmull-Rom downsample 600×900 → 400×600  (in this function)
//   2. Unsharp mask in-place at 400×600
//   3. Delegate to dither_ved_fs() for the FS pass
//
// Cost on a 2025 mid-range phone: ~80 ms for steps 1+2 + ~400 ms for the
// dither.  Old box-filter+inline-dither was ~400 ms total; the visual
// gain is worth the extra ~80 ms.

static inline float crom_w(float t)
{
    // Catmull-Rom cubic, a = -0.5.  Symmetric — caller can pass |t|.
    if (t < 0.0f) t = -t;
    if (t < 1.0f) return  1.5f * t*t*t - 2.5f * t*t + 1.0f;
    if (t < 2.0f) return -0.5f * t*t*t + 2.5f * t*t - 4.0f * t + 2.0f;
    return 0.0f;
}

// Clamp + round float channel to uint8.  Out-of-range overshoots are
// expected from Catmull-Rom near hard edges, so we clip explicitly.
static inline uint8_t u8_sat(float v)
{
    int i = (int)(v + 0.5f);
    if (i < 0)   return 0;
    if (i > 255) return 255;
    return (uint8_t)i;
}

// Resample src_rgb (src_w × src_h) into dst_rgb (panel_w × panel_h) using
// a 4×4 separable Catmull-Rom kernel.  Source center for output (x, y)
// is (1.5*x + 0.75, 1.5*y + 0.75) — phase-aligned to the supersample grid.
static void catmull_rom_downsample(const uint8_t *src_rgb, int src_w, int src_h,
                                   uint8_t *dst_rgb, int panel_w, int panel_h)
{
    for (int y = 0; y < panel_h; y++) {
        float sy = 1.5f * y + 0.75f;
        int   syf = (int)sy;
        float fy = sy - syf;
        float wy0 = crom_w(1.0f + fy);
        float wy1 = crom_w(fy);
        float wy2 = crom_w(1.0f - fy);
        float wy3 = crom_w(2.0f - fy);

        for (int x = 0; x < panel_w; x++) {
            float sx = 1.5f * x + 0.75f;
            int   sxf = (int)sx;
            float fx = sx - sxf;
            float wx0 = crom_w(1.0f + fx);
            float wx1 = crom_w(fx);
            float wx2 = crom_w(1.0f - fx);
            float wx3 = crom_w(2.0f - fx);

            float R = 0.0f, G = 0.0f, B = 0.0f, ws = 0.0f;
            for (int ky = 0; ky < 4; ky++) {
                int iy = syf + ky - 1;
                if (iy < 0)         iy = 0;
                if (iy >= src_h)    iy = src_h - 1;
                float wy = (ky == 0) ? wy0 : (ky == 1) ? wy1
                         : (ky == 2) ? wy2 : wy3;
                for (int kx = 0; kx < 4; kx++) {
                    int ix = sxf + kx - 1;
                    if (ix < 0)      ix = 0;
                    if (ix >= src_w) ix = src_w - 1;
                    float wx = (kx == 0) ? wx0 : (kx == 1) ? wx1
                             : (kx == 2) ? wx2 : wx3;
                    float w = wx * wy;
                    const uint8_t *p = src_rgb + (iy * src_w + ix) * 3;
                    R += w * p[0];
                    G += w * p[1];
                    B += w * p[2];
                    ws += w;
                }
            }
            // Catmull-Rom weights sum to ~1 by construction; renormalise
            // for numerical hygiene at edge-clamped positions.
            float inv = (ws != 0.0f) ? (1.0f / ws) : 0.0f;
            uint8_t *d = dst_rgb + (y * panel_w + x) * 3;
            d[0] = u8_sat(R * inv);
            d[1] = u8_sat(G * inv);
            d[2] = u8_sat(B * inv);
        }
    }
}

// In-place 3×3 box-blur unsharp mask.  amount=0.3 gives a subtle pop;
// values above 0.6 start showing halos against the dither.  Allocates
// one w*h*3 scratch buffer (panel_w=400, panel_h=600 → 720 KiB) — fine in
// WASM heap and PSRAM on-device.
static void unsharp_mask_inplace(uint8_t *rgb, int w, int h, float amount)
{
    if (amount <= 0.0f) return;
    const size_t n = (size_t)w * h * 3;
    uint8_t *tmp  = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *blur = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!tmp || !blur) {
        ESP_LOGW(TAG, "unsharp: alloc failed, skipping");
        free(tmp); free(blur);
        return;
    }
    // Horizontal 3-tap blur.
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int x0 = (x > 0) ? x - 1 : 0;
            int x2 = (x < w - 1) ? x + 1 : w - 1;
            for (int c = 0; c < 3; c++) {
                int s = rgb[(y*w + x0)*3 + c]
                      + rgb[(y*w + x )*3 + c]
                      + rgb[(y*w + x2)*3 + c];
                tmp[(y*w + x)*3 + c] = (uint8_t)((s + 1) / 3);
            }
        }
    }
    // Vertical 3-tap blur.
    for (int y = 0; y < h; y++) {
        int y0 = (y > 0) ? y - 1 : 0;
        int y2 = (y < h - 1) ? y + 1 : h - 1;
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 3; c++) {
                int s = tmp[(y0*w + x)*3 + c]
                      + tmp[(y *w + x)*3 + c]
                      + tmp[(y2*w + x)*3 + c];
                blur[(y*w + x)*3 + c] = (uint8_t)((s + 1) / 3);
            }
        }
    }
    free(tmp);
    // result = orig + amount * (orig - blur), clamped.
    for (size_t i = 0; i < n; i++) {
        int orig = rgb[i];
        int b    = blur[i];
        int sharp = orig + (int)((orig - b) * amount + (orig > b ? 0.5f : -0.5f));
        if (sharp < 0)   sharp = 0;
        if (sharp > 255) sharp = 255;
        rgb[i] = (uint8_t)sharp;
    }
    free(blur);
}

static int dither_15x_core(const uint8_t *src_rgb, int src_w, int src_h,
                           int panel_w, int panel_h,
                           uint8_t *out_packed,
                           uint8_t *out_idx_opt,
                           int smoothness,
                           bool e6_mix_mode)
{
    if (src_w != panel_w * 3 / 2 || src_h != panel_h * 3 / 2) {
        ESP_LOGE(TAG, "%s 15x: src must be 1.5× panel (%dx%d vs %dx%d panel)",
                 e6_mix_mode ? "e6" : "classic", src_w, src_h, panel_w, panel_h);
        return -1;
    }

    // Materialise the 400×600 downsampled target so the FS dither can sweep
    // it row-by-row.  720 KiB allocation — fits in WASM heap; on-device this
    // can land in PSRAM, but the FS error buffers still need SRAM.
    uint8_t *target = (uint8_t *)heap_caps_malloc((size_t)panel_w * panel_h * 3,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!target) {
        // Fallback to internal RAM if PSRAM is absent (host build / no PSRAM).
        target = (uint8_t *)heap_caps_malloc((size_t)panel_w * panel_h * 3,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!target) {
        ESP_LOGE(TAG, "15x: target buffer alloc failed");
        return -1;
    }

    catmull_rom_downsample(src_rgb, src_w, src_h, target, panel_w, panel_h);
    unsharp_mask_inplace(target, panel_w, panel_h, 0.48f);

    int rc = e6_mix_mode
           ? dither_e6_mix_fs(target, panel_w, panel_h, out_packed, out_idx_opt, smoothness)
           : dither_ved_fs(target, panel_w, panel_h, out_packed, out_idx_opt, smoothness);
    free(target);
    return rc;
}

int dither_ved_fs_15x(const uint8_t *src_rgb, int src_w, int src_h,
                      int panel_w, int panel_h,
                      uint8_t *out_packed,
                      uint8_t *out_idx_opt,
                      int smoothness)
{
    return dither_15x_core(src_rgb, src_w, src_h, panel_w, panel_h,
                           out_packed, out_idx_opt, smoothness, false);
}

int dither_e6_mix_fs_15x(const uint8_t *src_rgb, int src_w, int src_h,
                         int panel_w, int panel_h,
                         uint8_t *out_packed,
                         uint8_t *out_idx_opt,
                         int smoothness)
{
    return dither_15x_core(src_rgb, src_w, src_h, panel_w, panel_h,
                           out_packed, out_idx_opt, smoothness, true);
}

int flat_fill_constructivist_15x(const uint8_t *src_rgb, int src_w, int src_h,
                                 int panel_w, int panel_h,
                                 uint8_t *out_packed,
                                 uint8_t *out_idx_opt)
{
    if (src_w != panel_w * 3 / 2 || src_h != panel_h * 3 / 2) {
        ESP_LOGE(TAG, "flat-fill 15x: src must be 1.5× panel (%dx%d vs %dx%d panel)",
                 src_w, src_h, panel_w, panel_h);
        return -1;
    }

    uint8_t *target = (uint8_t *)heap_caps_malloc((size_t)panel_w * panel_h * 3,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!target) {
        target = (uint8_t *)heap_caps_malloc((size_t)panel_w * panel_h * 3,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!target) {
        ESP_LOGE(TAG, "flat-fill 15x: target buffer alloc failed");
        return -1;
    }

    catmull_rom_downsample(src_rgb, src_w, src_h, target, panel_w, panel_h);
    // Stronger sharpening than the dither path is acceptable here because no
    // diffusion texture can amplify halos; it preserves thin constructivist
    // edges before hard palette assignment.
    unsharp_mask_inplace(target, panel_w, panel_h, 0.62f);

    int rc = flat_fill_constructivist(target, panel_w, panel_h, out_packed, out_idx_opt);
    free(target);
    return rc;
}

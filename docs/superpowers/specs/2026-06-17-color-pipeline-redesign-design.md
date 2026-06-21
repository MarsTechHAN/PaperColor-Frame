# E6 Color Pipeline — First-Principles Redesign

**Date:** 2026-06-17
**Status:** Design proposal — pending user review & on-panel A/B sign-off
**Scope:** The full photo→panel color rendering pipeline (browser WASM + on-device C).

This supersedes the narrower 2026-05-28 "color rendering modes" patch, which tuned
knobs on top of a core whose *objective* and *color space* are wrong. This document
reasons from the physics up and proposes a layered, model-based pipeline shipped as an
experimental **v2 renderer** (default OFF, A/B against Legacy on the physical panel).

---

## 1. The panel, from first principles

E6 is a **reflective, subtractive-ish, 6-ink electrophoretic** display. The decisive
facts (measured Lab, D65, the values the dither actually matches against):

| Ink | L* | a* | b* | C* | note |
|---|---|---|---|---|---|
| White | 65.5 | −3.5 | −1.2 | 3.7 | "paper" is only L65 and slightly cyan |
| Black | 30.6 | 3.0 | −6.4 | 7.1 | |
| Yellow | 62.9 | −7.5 | 45.1 | 45.7 | **the only bright, high-chroma ink** |
| Red | 35.7 | 21.6 | 8.6 | 23.2 | dark |
| Blue | 40.7 | −2.3 | −24.7 | 24.8 | dark |
| Green | 41.1 | −14.6 | 11.8 | 18.8 | dark, low chroma |

Three physical truths follow, and the whole redesign hangs on them:

1. **Tiny, dark dynamic range.** Usable L* is ≈[30, 65] — a ~5:1 medium. There is no
   "bright." The viewer adapts to paper-white (L65) as their reference white.
2. **Extreme anisotropy.** Chroma is high only for yellow and only near L60. Reds/blues/
   greens are dark (L35–41). **On E6, chroma costs lightness** — every saturated color is
   built from dark ink.
3. **Color is made by optical dot-mixing.** A region's perceived color is the
   **reflectance** average of its dots, blurred by the eye — a Neugebauer / Yule–Nielsen
   process, **not** a CIELAB average of ink vertices.

A correct pipeline is therefore a *display-referred color-management problem*: map a
scene-referred sRGB image into this small dark gamut, then halftone using a model of how
the dots actually combine — exactly the chain E Ink itself patents (US12100369B2 "DHHG":
degamma → HDR-style shadow lift → hue correction → gamut map → dither).

---

## 2. What the current pipeline does, and why it looks bad

Default path (`wasm_dither_e6_15x` in browser; `dither_e6_mix_fs` on device):

```
adjust → enhance_eink (S-curve + clarity unsharp + chroma boost)
       → [browser only] Catmull-Rom 1.5×→1× → 2nd unsharp(0.48)
       → per pixel: rgb→Lab → map_source_to_panel_lab (toe^0.92, chroma ×0.44, cusp bend)
       → pick nearest of 6 Lab vertices by CIEDE2000 + ~40 hand-tuned biases
       → Stucki error diffusion of the residual IN LAB
       → local refine (5×5 linear-Lab average)
```

### Root cause A — the dither optimizes the wrong objective in the wrong space *(central)*

The dither picks the nearest of the **6 Lab vertices** and diffuses the residual **in
Lab**, implicitly assuming dots mix linearly in Lab. They do not — they mix in
reflectance. I verified this against the panel's **own 28 measured halftone patches**:

| Mixing model | Mean ΔE76 vs measured | Chromatic mixes only |
|---|---|---|
| **Linear Lab average** ← what the dither assumes | **3.45** | **3.74** |
| Linear reflectance avg (Murray–Davies, n=1) | 3.06 | 3.04 |
| **Yule–Nielsen in reflectance, n≈1.4** | **2.42** | **2.53** |

> Metric note: figures are ΔE76 on the measured SCI patches; the ordering is robust — the
> adversarial reviewer reproduced 3.31 / 3.02 / 2.36 (n≈1.45) on the LEGACY (RGB-roundtrip)
> palette the device actually runs, and LOOCV puts the YN exponent at n∈[1.42, 1.50].

The reflectance/YN model is clearly the best, and the linear-Lab model the dither uses is
biased by **≈1.7 L\*** (up to ~3.5 on chromatic pairs). The sign, corrected after review:
because L\* = 116·f(Y)−16 is **concave**, the Lab-mean of two inks has **lower** L\* than the
true reflectance mean — i.e. the dither's internal model thinks its halftones are ~1.7 L\*
*darker* (and more chromatic) than the panel really shows. The damage is therefore primarily
one of **color accuracy and preview↔panel agreement**: the dither optimizes against a model
that mispredicts the real panel by ~3.3 ΔE, so error diffusion does not converge to the
correct *optical* mean and the browser preview is not a faithful predictor. (The visible
*darkening / shallow DR* the user reports is driven mainly by Root Causes B and C — the tone
crush and the chroma squash + cusp bend; this one is the color-fidelity/consistency defect.)
`dither.c:996–1009` (Lab residual), `dither.c:639–644` (linear-Lab refine average). The YN
model exists in JS (`index.html:5026 rgbFromIndexWeights`) but **only drives the
preview/auto-exposure, never the dither**, and it mixes in **sRGB (gamma) space rather than
reflectance** — so even the preview over-darkens. (Its n≈1.85 is only the no-calibration
*fallback*; fitted to the 28 patches in sRGB it lands near 1.0 — the wrong domain either way.)

### Root cause B — tone is *expanded* then *crushed*, never shaped

`enhance_eink_rgb888` does contrast **expansion** (S-curve 0.10 + clarity unsharp +
unconditional chroma boost) in scene-referred RGB; then `map_source_to_panel_lab` does the
only real tone compression as a **near-linear** squash of L\*[0,100]→[30.6,65.5] (×0.349).
So manufactured contrast is either clipped at 0/255 or multiplied flat. The literature
(DHHG) says do the opposite: **lift shadows before quantizing** (treat sRGB as HDR relative
to the panel) so darks get halftone headroom instead of dither-noisy near-black, and add a
soft highlight **shoulder**. There is also **no dim-surround compensation** and **no
white-point adaptation** to paper-white. `color_pipeline.c:365–379`, `dither.c:211–254`.

### Root cause C — uniform ×0.44 chroma squash fights an anisotropic gamut

A single scalar cannot fit a gamut where yellow reaches C46 and green only C19. Measured
target/ceiling ratios under ×0.44: yellow **0.85** (under-driven → dull yellows, wasted
brightness), blue **1.29**, green **1.47**, red **1.56** (driven 16–18 ΔE past their hull
→ same-signed dark-ink pile-up → muddy mids). Worse, the **cusp bend** (`dither.c:234–242`)
pulls bright yellows *down*: a sunlit yellow (src L88/C88) maps to **L44.9/C34.5** while the
yellow ink itself is **L62.9/C45.7** — throwing away ~18 L\* and ~11 C\* of headroom on the
panel's single bright chromatic ink. This is the largest "shallow dynamic range"
contributor.

### Root cause D — CIELAB is poor exactly where this gamut lives

5 of 6 inks are dark (L35–41) and one is a deep blue. CIELAB's two best-documented
failures — blue→purple hue drift and poor uniformity on dark colors — land squarely on the
gamut. Oklab fixes both at lower cost than CIEDE2000.

### Root cause E — ~40 hand-tuned biases compensating for the wrong core

`region_palette_bias` + `e6_palette_bias` (×0.72) + its own raw-RGB re-tests +
`e6_screen_tiebreak` + refine reuse (×0.12) triple-apply the same rules and only bite in
the near-neutral band (the raw ΔE gap to the runner-up is 3–17 while the whole bias stack is
±0.4–2.8). Most anti-red / pro-yellow / anti-dark-ink terms exist to paper over the
dark/over-chromatic bias of the linear-Lab core (root cause A). Fix the core and most
evaporate; keep **two** real perceptual facts explicitly.

### Root cause F — browser ↔ device parity gap

The device `.jpg` path (`loop_display.c:341–353`) uses a **fixed** default adjust, **no**
auto-exposure, **no** 1.5× supersample and **no** second sharpen — the *same* `dither_e6_mix_fs`
core but a categorically weaker front-end than the preview the user tunes. (Baked `.bin`
uploads are fine; the gap is untuned/slideshow images.) `adjust_cfg_t` has no exposure field
at all. A second, subtler gap: the device never calls `palette_set_mode`/`color_render_set_mode`,
so it stays pinned to the compile-time **LEGACY palette + 6COLOR** anchors, while the browser
can push calibrated/SCE Lab — so even a baked `.bin` was halftoned against ink anchors the
device's own model wouldn't reproduce. Closing parity means pinning both targets to the same
palette **and** render mode.

### Secondary

- ±24 Lab residual clamp (`dither.c:74`) truncates the legitimate large residual that
  out-of-gamut colors need.
- Smoothness snap (`dither.c:1000–1007`) zeros **all** residual on near-palette flats →
  kills sky/skin gradients.
- 1.5× downsample averages in sRGB, not linear → midtone gamma error (`dither.c:1124`).

---

## 3. The redesign — a layered, model-based pipeline

One coherent chain, unified in a **reflectance + Oklab** framework, **identical in WASM and
C**, shipped as experimental **v2** (default off). Mirrors DHHG order + model-based
halftoning.

```
A. Linearize (sRGB→linear; gamma-correct resize/downsample)
B. Display-referred TONE   (shadow lift + highlight shoulder + dim-surround contrast
                            + optional white-point adaptation to paper-white)   — shapes L*, AWB on a*b*
C. Gamut-aware CHROMA       (Oklab; preserve in-gamut core; soft per-hue compression of
                            ONLY out-of-gamut colors toward Cmax+δ, stop short of surface;
                            bend L toward each hue's cusp)                       — leaves out-of-gamut residual
D. Model-based HALFTONE     (Yule–Nielsen-in-reflectance objective, n re-fit ≈1.4;
                            select in Oklab; diffuse residual in REFLECTANCE;
                            MBVC low-L-spread ink sets; structure-aware threshold mod)
E. Spatial cleanup          (one ink-visibility term + one clumping/blue-noise term;
                            void-and-cluster mask for flats; refine on the unified objective)
F. Parity                   (same code C↔WASM; bake .bin at upload; exposure in adjust_cfg_t;
                            display-referred transform is the always-on default everywhere)
```

### Stage B — display-referred tone (replaces S-curve + toe + cusp-L)
One curve, scene-luminance → panel L\*:
- **Shadow lift** keyed to panel L_min(~30): source blacks land a few L\* above L_min so
  shadows have halftone headroom (DHHG's stated rationale; fixes dark dither noise).
- **Soft highlight shoulder** into L_max(~65): low-chroma highlights keep separation
  instead of bunching in the top 3 L\*.
- **Contrast-around-pivot** (~1.10 at L\*≈47) to compensate dim-surround flattening
  (Bartleson–Breneman).
- **Optional white-point adaptation**: re-anchor source white (D65/L100) toward measured
  WHITE (L65.5, a−3.5, b−1.2) so paper-white becomes the reference → large bright areas
  resolve to **clean solid WHITE ink** (not grainy gray), perceived brightness rises.

Operates on L\* (+ a\*b\* re-anchor for AWB) only; chroma residual untouched.

### Stage C — gamut-aware chroma (replaces uniform ×0.44)
In Oklab: build the 6-ink+28-mix hull once. Colors **inside** the hull pass through
untouched (preserve the core — HPMINDE finding). Colors **outside**: soft per-hue sigmoid
toward a ceiling `Cmax(hue)+δ` (δ ≈ 8–12 ΔE of deliberate overshoot) that **stops short of
the surface**, and bend L\* toward that hue's cusp L (yellow's cusp is high, so bright
yellows stay bright). Keep ≈0.44 as an explicit **residual budget** that now applies only
where needed. This is categorically different from the rejected 17³ LUT, which snapped
*onto* the surface — here residual is preserved by construction (see §4).

### Stage D — model-based halftoning (the core fix)
- **Appearance model:** Yule–Nielsen in **linear reflectance**, `R^(1/n) = Σ fᵢ·Rᵢ^(1/n)`,
  with **n re-fit ≈1.4** from the 28 patches (LOOCV n∈[1.42,1.50]; the JS value — a 1.85
  fallback, ~1.0 when actually fitted in sRGB — is in the wrong domain). Precompute each
  ink's `Rᵢ^(1/n)`; one `pow` per cluster. **One model
  shared by dither, preview, and AE** → preview finally predicts the panel.
- **Selection:** Euclidean ΔE in **Oklab** computed from the YN-predicted local reflectance
  (drop CIEDE2000 → faster *and* better on dark/blue).
- **Diffusion:** carry residual in **reflectance** (Stucki kept) so neighborhoods converge
  to the correct mean reflectance. Split the clamp (more L\* latitude, damp a\*b\*); bound
  the *running* working color to the hull before quantizing (E Ink "gamut projector") to
  stop oversaturation worms — this bounds *accumulated* error, it does **not** pre-compress
  the source.
- **MBVC/MBVQ:** penalize ink sets by their L\* spread → prefer low-spread mixes
  (yellow+green over white+black) → removes salt-and-pepper "mud" (peer-reviewed).
- **Structure-aware threshold modulation** (Li & Mould: darker→blacker, lighter→whiter,
  gated by the existing `source_edge_strength`) → claws back apparent contrast from the tiny
  L-range, replacing the brittle double-unsharp pre-filter.

### Stage E — principled cleanup (replaces the bias pile)
Two terms, applied once: (1) **ink-visibility-in-highlights** = `max(0,(L_local−55)/18) ×
darkness(ink)` from each ink's measured L\* (the one real fact behind most region biases —
a dark/red dot in an L>55 highlight is genuinely conspicuous because panel red is only L36);
(2) **dot-clumping** penalty. For flat regions, a precomputed **void-and-cluster blue-noise
mask** replaces the ordered tiebreak (removes periodic texture). Refine uses the **same**
unified YN objective with balanced L/chroma weights (currently 1.20·dL² vs 0.58·dab² — it
actively trades chroma away).

### Stage F — parity
Same renderer in C and WASM. Add `exposure` (+ local highlight protection) to
`adjust_cfg_t` so the device default can place the histogram. **Bake the `.bin` at upload**
for every image so the device always displays exactly what the browser produced. Demote the
14-candidate panel-aware AE to an experimental refine; make the display-referred transform
the always-on default in both targets.

---

## 4. Reconciliation with the hard constraints

1. **"Never pre-compress source Lab onto the gamut surface" (the rejected 17³-LUT
   regression).** Honored, and honored *better*. Nothing here snaps the source onto the
   hull: Stage C compresses only out-of-gamut chroma and **stops short** of the boundary
   (asymptote at Cmax+δ); Stage D diffuses residual in reflectance and only bounds the
   *running accumulated* error (the working color), never the source. The residual becomes
   a **first-class, physically-meaningful, tunable** quantity instead of an accidental
   byproduct of ×0.44. The prior regression came from mapping *onto* vertices/surface →
   zero residual → solid blocks; this design keeps signed residual alive everywhere.
2. **Experimental, default OFF until on-panel sign-off.** Every stage ships behind a v2
   toggle; Legacy stays byte-identical and selectable for A/B.
3. **Runs on WASM and ESP32-S3.** YN uses precomputed `Rᵢ^(1/n)` tables + a small `^n` LUT;
   Oklab is matrix→cbrt→matrix (cheaper than CIEDE2000); error rows stay 3 floats/pixel; the
   blue-noise mask lives in flash/PSRAM, not SRAM. Per-pixel cost to be benchmarked.
4. **The panel is the acceptance test.** Each stage is A/B'd on the physical panel before any
   default flip.

---

## 5. Staged implementation plan (lowest-risk first, each A/B'd on panel)

Ordered so the highest-leverage, lowest-risk change ships first and is independently
verifiable on the panel — matching the project's fast build→flash→observe loop.

| Stage | Change | Risk | Why first |
|---|---|---|---|
| **1** | **Unify the optical model in linear reflectance** + re-fit n (offline in `tools/wfsim`). Port YN-reflectance into C (`palette_mix_model_yn`), use it in `optical_avg_error5` for both modes; fix `rgbFromIndexWeights`/`opticalMixExponent` to mix in linear light. | Low | Panel-side only (the sanctioned "move ink anchors, not source Lab" lever). Fixes the +1.7 L\* dark bias; makes preview honest. |
| **2** | **Diffuse residual in reflectance** + YN-in-loop picker objective; relax/split the ±24 clamp. | Medium | The central correctness fix (root cause A). |
| **3** | **Oklab working space** for selection + diffusion (drop CIEDE2000). | Medium | Fixes blue/dark (root cause D); also faster. |
| **4** | **Display-referred tone** (shadow lift + shoulder + dim-surround); remove S-curve & one unsharp. | Medium | Root cause B; biggest "DR" win. |
| **5** | **Gamut-aware chroma** (per-hue, out-of-gamut-only, cusp-L). | High | Root cause C; highest reward, highest regression risk → most on-panel tuning. |
| **6** | **MBVC** + structure-aware threshold mod + **collapse the bias pile** to 2 terms + blue-noise flats. | Medium | Cleanup enabled by 1–3. |
| **7** | **Device parity**: exposure in `adjust_cfg_t`, bake `.bin` at upload, unify default transform. | Low/Med | Root cause F; closes "looked fine in preview". |

Stage 1 is buildable immediately and is the natural first on-panel A/B. Each subsequent
stage is a separate experimental toggle so regressions are isolable.

---

## 6. Validation

- **Offline:** the `tools/wfsim` YN re-fit reports per-patch ΔE; target < ~2.5 mean (vs
  3.45 today). Bit-exact Legacy regression guard on a fixed test image.
- **Build:** `web_wasm/build.sh` (WASM) + `idf.py build` (firmware).
- **On-panel (the gate):** render a fixed photo set in Legacy vs each v2 stage; user picks.
  No fix is "done" before this.

## 7. Open questions for the user

- Build order: start with Stage 1 (model unification) as proposed, or prioritize the tone
  redesign (Stage 4) which may show the most visible change first?
- White-point adaptation to paper-white (Stage B) is the most "opinionated" change (it
  shifts the rendering intent) — include in v2 or hold as its own separate toggle?
- Is per-image auto-exposure worth keeping at all, or replace with one robust
  display-referred transform + a simple auto-level?

---

## 8. Implementation status — v1 shipped 2026-06-17 (experimental, default OFF)

A de-risked first slice of the method shipped as a new **`COLOR_RENDER_REFLECTANCE`**
(value 2) option in the existing Experimental "Color rendering" dropdown — selectable as
**"Reflectance v2"**, A/B'able against Legacy/6color/mixpatch with one click. Legacy paths
are byte-identical (every change is gated on the mode). Builds verified: WASM
(`web_wasm/build.sh`) + firmware (`idf.py build`, 35% flash free); smoke test
`web_wasm/smoke_v2.mjs` passes all three modes (no reserved ink codes, non-degenerate,
reflectance differs from 6color by 63% of bytes; reflectance uses more white+yellow and
less dark red/blue/green, as predicted).

**What v1 does (gated by the mode):**
- **Reflectance-domain error diffusion.** Residual carried in pseudo-reflectance
  `p = XYZ^(1/n)`, n = 1.4 (`palette.c` PALETTE_REFL_P; `dither.c` lab_to_p/p_to_lab,
  inner loop). A halftone neighbourhood now converges to the YN optical mean, not the
  linear-Lab mean. Clamp re-scaled for p-space (±0.6 vs ±24 Lab).
- **YN refine objective.** `optical_avg_error5` scores against `palette_mix_model_yn`
  (Yule–Nielsen) with balanced L/chroma weights (1.0/1.0 vs legacy 1.20/0.58).
- **Display-referred tone.** Stronger lift (`t^0.80`) for brighter mids; the cusp bend
  that was darkening bright yellows to L45 is dropped. Endpoints preserved; chroma stays
  ×0.44 so out-of-gamut residual is intact.
- **Softer pre-enhance.** No chroma boost, gentler clarity, no upstream S-curve
  (`enhance_eink_rgb888`); post-resample unsharp 0.48→0.25.
- Selection stays CIEDE2000-in-Lab against the 6 vertices with the existing picker/biases —
  deliberately unchanged so this A/B isolates the *colour-space + tone* fix.

**v1.1 anti-speckle (2026-06-17, follow-up to "麻点很重"):**
- **Neutral de-confetti** (`e6_palette_bias`, v2 only): penalise red/blue/green in low-source-chroma
  pixels (`chroma < 30`, ramped) across *all* lightnesses — the prior suppression only covered
  highlights (`y>112`). A stray coloured dot on a grey/skin/wall reads as confetti; this keeps
  neutral regions on white/black/yellow. Measured on a near-neutral gradient: coloured-ink usage
  16.6%→10.1%, blue/green confetti cut ~4× (blue 3.5→0.6%, green 5.6→1.3%). Yellow exempt (L≈63 ≈
  paper white → low-contrast filler, not speckle).
- Post-resample sharpen lowered 0.25→0.10 (every sharpen overshoot becomes an extra halftone dot).
- *Inherent limit:* luminance grain in smooth **mid-grey** is not fully removable — the panel has no
  mid-L neutral ink, so L≈40–55 must dither white(L65)+black(L30). Levers: the Smoothness slider
  (snaps flatter regions), or a future blue-noise distribution pass (finer/more-uniform dots).

**v1.2 — unified forward model + gamma correctness (2026-06-17, "rethink"):**
User feedback: stop bolting biases; gamma/auto still poor; want preview==panel and the calibration data
actually used. Root insight: the dither optimised one model (reflectance YN) while the JS preview + AE
scored a *different* one (YN in gamma-encoded sRGB), so the preview/auto chased something the panel never
shows. Fix:
- **One forward model everywhere.** `rgbFromIndexWeights` / preview / AE scorer rewritten to the *same*
  linear-reflectance Yule–Nielsen model the C dither uses (`PANEL_YN_N`=1.4, ink anchors = calibrated
  Lab→XYZ). Validated against the measured patches: W+R 50/50 → predicted L52.4 vs measured L52.4; W+Y
  50/50 within ~0.3; neutral ramp within ~1.4 ΔE — vs the old sRGB model's ~4–5 ΔE dark bias. **Preview now
  predicts the panel.**
- **Gamma-correct resampling.** v2 Catmull-Rom downsample now averages in **linear light**
  (`color_srgb_to_linear`/`color_linear_to_srgb`), not gamma-encoded sRGB — removes the mid-tone/edge
  bias of the old byte averaging.
- **Still TODO (next, now that preview is truthful — tune visibly, not blind):** (a) the source→panel
  **tone curve** (replace `t^0.80` with a calibrated display-referred curve anchored to the measured
  neutral ramp); (b) **auto redo** — drop the scene-classified per-image AE for a fixed transform + robust
  histogram black/white/mid auto-level.

**Deferred (separate future experiments, per the staging table):** Oklab working space
(Stage 3); per-hue out-of-gamut-only chroma compression (Stage 5); MBVC low-L-spread ink
selection; structure-aware threshold modulation; bias-pile cleanup; white-point adaptation;
device parity (exposure in `adjust_cfg_t`, bake-at-upload, pin palette+render mode).

**Known follow-ups flagged by the adversarial review:**
- The **smoothness snap** (ΔE-12 default, `dither.c`) is coupled to chroma compression: if a
  later stage tightens chroma it can re-trigger the solid-block regression via the snap.
  v1 leaves chroma at ×0.44 so this is dormant, but Stage 5 must co-tune the snap
  (chroma-only attenuation, or lower threshold).
- Cost: v1's reflectance path adds ~a dozen `powf`/pixel (≈11% slower in the WASM smoke
  test); acceptable for browser preview. If the device adopts it, precompute/LUT the `^n`.

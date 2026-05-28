# Color Rendering Modes — Design

**Date:** 2026-05-28
**Status:** Approved (design), pending spec review

## Problem

On the physical E6 panel, current output is **oversaturated, darker, and has shallow
dynamic range** (easy to over/under-expose) compared to an earlier version. The user
judges quality on the reflective panel, not the browser preview.

## Root-cause analysis (from reflective-display first principles)

E6 is reflective and low-contrast: measured white ≈ L\*65, black ≈ L\*31, and the
colored inks are dark (red ≈ L\*38, blue/green ≈ L\*42; yellow ≈ L\*64 is the only light
chromatic ink). The decisive physics: **on E6, chroma costs lightness** — any saturated
color is built from dark inks, so chasing chroma drags a region darker.

The active path for a default user is `wasm_dither_e6_15x`:

```
Catmull-Rom 1.5×→1× downsample
  → enhance_eink_rgb888   (S-curve + clarity unsharp + 1.06–1.14× chroma)   color_pipeline.c:294
  → unsharp_mask_inplace(0.48)                                              dither.c:1230
  → per pixel: gamut_lab_from_rgb → map_source_to_panel_lab                 dither.c:202
  → pick_palette_e6_mix (CIEDE2000 + region biases + 28-patch mix model)    dither.c:441
  → Stucki error diffusion
```

Contributing flaws (the controllable ones):

1. **Stacked contrast/sharpening on a ~5:1 medium.** An S-curve + clarity unsharp in
   `enhance_eink_rgb888`, then a *second* `unsharp_mask_inplace(0.48)` in the 1.5× path,
   expand contrast and create overshoots **before** the source is hard-mapped into the
   ~34-L\* panel window. Result: clipped highlights/shadows (shallow DR, easy
   over-exposure) and halos. Reflective media wants gentle tonal *compression*, not
   expansion.
2. **Unconditional chroma boost.** `enhance_eink_rgb888` multiplies chroma by
   `1.06 + 0.08·edge` on every pixel → feeds more saturation into a pipeline that
   already over-commits to colored ink.
3. **Mix-model eagerness (the concrete regression vs the "better" previous version).**
   The 28-patch mix model (commit `e613e28`) convinces `pick_palette_e6_mix` it can reach
   intermediate saturated colors via halftone, so it commits to dark colored-ink mixes
   far more than the prior 6-solid-ink picker. More colored dots → darker AND more
   saturated.

## Hard constraint (do not violate)

**Never pre-compress source Lab into the panel gamut — by any route (gamut LUT, higher
`chroma_scale`, or early cusp projection).** This starves the error-diffusion residual →
solid-color blocks → smaller dynamic range. This was tried (the 17³ cusp LUT,
`buildGamutMapLut`/`pushGamutLutToWasm`) and rejected on 2026-05-21 ("效果很垃圾, 动态范围
反而小了"). `chroma_scale` stays at 0.44. Fixes operate **only** on enhancement and picker
knobs, leaving `map_source_to_panel_lab` and source Lab untouched.

Also: per project rule, new color behavior ships as **Experimental, default off**, until
on-panel sign-off.

## Approach

Add candidate **color rendering modes**, surfaced as a dropdown in the Experimental
section, so the user can A/B them on the panel and pick a winner. Each mode is a coherent,
rule-compliant combination of the three controllable knobs. Plumbed identically to the
existing SCE colorimetry mode (C-side enum → WASM bridge → drives both live preview and
final render).

### Modes

| Mode | Knob changes | Targets |
|---|---|---|
| **Legacy** (default) | none — bit-exact current pipeline | baseline reference |
| **Gentle** | dither unsharp 0.48→0; `enhance` S-curve coeff 0.10→0.04; clarity `0.26+0.30·edge`→`0.12+0.15·edge`. Saturation + mix unchanged | shallow DR, over-exposure, halos |
| **Clean** | `enhance` saturation `1.06+0.08·edge`→`1.0` (no boost); mix-model weight 0.34→0.15. Tone unchanged | oversaturation, colored speckle |
| **Natural** | Gentle + Clean combined | balanced best-guess |

Exact anchor points:
- S-curve: `color_pipeline.c:355` (`0.10f` coefficient).
- Clarity: `color_pipeline.c:349` (`0.26f + 0.30f * edge`).
- Saturation: `color_pipeline.c:360` (`1.06f + 0.08f * edge`).
- 1.5× unsharp: `dither.c:1230` (`unsharp_mask_inplace(..., 0.48f)`).
- Mix-model weight: `dither.c:474` (`(mix_de - raw[p]) * 0.34f`).

## Components & data flow

- **C state (color_pipeline.h/.c):** new `color_render_mode_t { LEGACY, GENTLE, CLEAN, NATURAL }`,
  a file-static `s_color_render_mode` (default LEGACY), and `color_render_set_mode` /
  `color_render_get_mode`. Two derived helpers keep call sites readable, e.g.
  `color_render_unsharp_scale()` and `color_render_sat_enabled()` / `color_render_mix_weight()`,
  OR the consuming functions branch directly on the enum — chosen at implementation time
  for minimal churn.
- **enhance_eink_rgb888 (color_pipeline.c):** reads the mode; selects S-curve coeff,
  clarity coeffs, and saturation multiplier accordingly.
- **dither.c:** `dither_15x_core` selects the unsharp amount from the mode;
  `pick_palette_e6_mix` selects the mix-model weight from the mode.
- **WASM bridge (wasm_entry.c):** `wasm_set_color_render_mode(int)` /
  `wasm_get_color_render_mode(void)`, mirroring `wasm_set_palette_mode`.
- **Web UI (index.html):** a new "Color rendering" `<select>` in the Experimental section
  (separate from the SCE colorimetry select), default `legacy`; `onchange` pushes the mode
  to WASM and re-renders preview; persisted in `calib.pipeline.colorRenderMode` via the
  same path as `colorimetryMode`; EN/ZH i18n strings for label + option names + hint.
- **Device (loop_display.c):** unchanged — global defaults to LEGACY, so on-device
  rendering keeps current behavior. Modes are browser-only for v1.

## Non-goals (v1)

- No source-side gamut mapping, no `chroma_scale` change, no ink-anchor moves (the SCE
  anchor experiment regressed; out of scope here).
- No change to the JS auto-exposure analyzer (`analyzeCanvasForAuto`) — it is
  content-adaptive and left intact.
- No device-side UI for mode selection.

## Testing / validation

- **Build:** WASM rebuild via `web_wasm/build.sh`; full firmware build via
  `idf.py build` (after `source ~/esp/esp-idf/export.sh`).
- **Smoke:** extend `web_wasm/smoke_test.mjs` to call `wasm_set_color_render_mode` for each
  mode and confirm `wasm_dither_e6_15x` still returns 0 and a non-degenerate framebuffer.
- **Bit-exactness:** Legacy must produce byte-identical output to the pre-change pipeline
  (regression guard) — compare packed output on a fixed test image.
- **On-panel (user-owned, the real acceptance gate):** user renders the same photo set in
  each mode on the physical panel and picks the winner. No fix is claimed to "work" before
  this.

## Open questions

- Whether to keep the chosen winner as a new default (flipping `s_color_render_mode`) or
  fold it into the Legacy path is deferred until the user picks on-panel.

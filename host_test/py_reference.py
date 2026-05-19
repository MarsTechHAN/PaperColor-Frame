#!/usr/bin/env python3
"""Reference Python implementation of the dither pipeline that mirrors the
C code under main/dither.c bit-for-bit (modulo float vs double precision).

Differences from the original main.py:
  * Uses the new measured 6-colour palette baked into the C code so both
    sides match.
  * Implements CIEDE2000 and serpentine Floyd-Steinberg natively in NumPy/
    Python (no colormath) and *mirrors* the FS offsets on serpentine rows.
    The original main.py did not flip the offsets, which causes 11/16 of
    the error budget to vanish on every other row.  The C side flips, so
    the reference must flip too for a fair comparison.

Outputs:
   <output_prefix>.bmp           — 6-colour preview (palette RGB lookup)
   <output_prefix>_indices.bin   — uint8 palette indices, row-major, 0..5

CLI:
   python3 py_reference.py [--no-adjust] input.raw out_prefix
"""
import argparse
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np

TW, TH = 400, 600

# Nominal sRGB primaries for labels/tests. Dither matching uses the measured
# reflective palette, then maps source Lab into that limited paper gamut.
PALETTE_RGB = np.array([
    [  0,   0,   0],   # 0 black
    [255, 255, 255],   # 1 white
    [255, 221,   0],   # 2 yellow
    [206,  38,  54],   # 3 red
    [  0,  64, 255],   # 4 blue
    [  0, 128,   0],   # 5 green
], dtype=np.uint8)
PALETTE_RGB_MEASURED = np.array([
    [ 70,  71,  80], [151, 161, 160], [163, 154,  69],
    [118,  69,  70], [ 60,  97, 134], [ 77, 100,  76],
], dtype=np.uint8)
PALETTE_NAMES = ["black","white","yellow","red","blue","green"]

# ----------------------------------------------------------------- sRGB → LAB

D65 = np.array([0.95047, 1.00000, 1.08883], dtype=np.float64)

def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)

def rgb_to_lab(rgb):
    # rgb in 0..255 uint8 or float.
    R, G, B = [srgb_to_linear(rgb[..., i] / 255.0) for i in range(3)]
    X = R * 0.4124564 + G * 0.3575761 + B * 0.1804375
    Y = R * 0.2126729 + G * 0.7151522 + B * 0.0721750
    Z = R * 0.0193339 + G * 0.1191920 + B * 0.9503041
    xr = X / D65[0]; yr = Y / D65[1]; zr = Z / D65[2]
    def f(t): return np.where(t > 0.008856, np.cbrt(t), 7.787*t + 16.0/116.0)
    fx, fy, fz = f(xr), f(yr), f(zr)
    L = 116.0 * fy - 16.0
    a = 500.0 * (fx - fy)
    b = 200.0 * (fy - fz)
    return np.stack([L, a, b], axis=-1)


def map_source_to_panel_lab(lab_img, palette_lab):
    out = lab_img.copy()
    black = palette_lab[0]
    white = palette_lab[1]
    t = np.clip(out[..., 0] / 100.0, 0.0, 1.0) ** 0.92
    neutral_a = black[1] + (white[1] - black[1]) * t
    neutral_b = black[2] + (white[2] - black[2]) * t
    out[..., 0] = black[0] + (white[0] - black[0]) * t
    out[..., 1] = neutral_a + out[..., 1] * 0.44
    out[..., 2] = neutral_b + out[..., 2] * 0.44
    return out

# ----------------------------------------------------------------- CIEDE2000

def ciede2000(L1, a1, b1, L2, a2, b2):
    """Scalar CIEDE2000.  Mirrors the C implementation."""
    C1 = math.hypot(a1, b1)
    C2 = math.hypot(a2, b2)
    Cb = 0.5 * (C1 + C2)
    Cb7 = Cb ** 7
    G  = 0.5 * (1.0 - math.sqrt(Cb7 / (Cb7 + 25**7)))
    a1p = (1 + G) * a1
    a2p = (1 + G) * a2
    C1p = math.hypot(a1p, b1)
    C2p = math.hypot(a2p, b2)
    h1p = 0.0 if (a1p == 0 and b1 == 0) else math.atan2(b1, a1p)
    h2p = 0.0 if (a2p == 0 and b2 == 0) else math.atan2(b2, a2p)
    if h1p < 0: h1p += 2*math.pi
    if h2p < 0: h2p += 2*math.pi
    h1p = math.degrees(h1p); h2p = math.degrees(h2p)
    dLp = L2 - L1
    dCp = C2p - C1p
    dhp = 0.0
    if C1p * C2p != 0:
        diff = h2p - h1p
        if abs(diff) <= 180:        dhp = diff
        elif diff > 180:            dhp = diff - 360
        else:                       dhp = diff + 360
    dHp = 2.0 * math.sqrt(C1p * C2p) * math.sin(math.radians(dhp) / 2.0)
    Lbp = 0.5 * (L1 + L2)
    Cbp = 0.5 * (C1p + C2p)
    hbp = h1p + h2p
    if C1p * C2p != 0:
        if abs(h1p - h2p) <= 180: hbp = 0.5 * (h1p + h2p)
        else:                      hbp = 0.5 * (h1p + h2p + 360) if (h1p+h2p < 360) else 0.5 * (h1p + h2p - 360)
    T = (1
         - 0.17 * math.cos(math.radians(hbp - 30))
         + 0.24 * math.cos(math.radians(2 * hbp))
         + 0.32 * math.cos(math.radians(3 * hbp + 6))
         - 0.20 * math.cos(math.radians(4 * hbp - 63)))
    dth = 30 * math.exp(-((hbp - 275) / 25) ** 2)
    Cbp7 = Cbp ** 7
    Rc = 2 * math.sqrt(Cbp7 / (Cbp7 + 25**7))
    Sl = 1 + (0.015 * (Lbp - 50)**2) / math.sqrt(20 + (Lbp - 50)**2)
    Sc = 1 + 0.045 * Cbp
    Sh = 1 + 0.015 * Cbp * T
    Rt = -math.sin(math.radians(2 * dth)) * Rc
    return math.sqrt((dLp/Sl)**2 + (dCp/Sc)**2 + (dHp/Sh)**2 + Rt * (dCp/Sc) * (dHp/Sh))

# ------------------------------------------------------------ adjust pipeline

def apply_adjust_rgb(rgb, brightness=0, contrast=108, saturation=81, gamma=54, temperature=100):
    """Mirrors the C apply_adjust_rgb888()."""
    c = contrast / 100.0
    sat = saturation / 100.0
    g = max(gamma/100.0, 0.01)
    temp = temperature / 100.0
    out = rgb.astype(np.float32) * c + brightness
    if temp > 1.0:
        out[..., 0] *= temp; out[..., 2] /= temp
    elif temp < 1.0:
        out[..., 0] *= temp; out[..., 2] /= (2 - temp)
    out = np.clip(out, 0, 255)
    # saturation
    if sat != 1.0:
        maxc = out.max(axis=-1)
        minc = out.min(axis=-1)
        v = maxc
        s = np.where(maxc > 0, (maxc - minc) / np.maximum(maxc, 1e-9), 0.0)
        s_new = np.clip(s * sat, 0, 1)
        k = (1 - s_new) * v
        scale = np.where((maxc - minc) > 0, (v - k) / np.maximum(maxc - minc, 1e-9), 0.0)
        for ch in range(3):
            out[..., ch] = k + (out[..., ch] - minc) * scale
    # gamma
    out = np.clip(out, 0, 255)
    out = ((out / 255.0) ** (1.0/g)) * 255.0
    return np.clip(out, 0, 255).astype(np.uint8)

# ---------------------------------------------------- vector-error FS dither

W7, W3, W5, W1 = 7/16, 3/16, 5/16, 1/16

def dither_ved_fs(lab_img, palette_lab):
    h, w = lab_img.shape[:2]
    err_cur = np.zeros((w, 3), dtype=np.float64)
    err_nxt = np.zeros((w, 3), dtype=np.float64)
    idx_out = np.zeros((h, w), dtype=np.uint8)
    serp = False
    for y in range(h):
        if serp:
            xs = range(w-1, -1, -1)
            dx_fwd = -1
        else:
            xs = range(w)
            dx_fwd = +1
        for x in xs:
            L, a, b = lab_img[y, x] + err_cur[x]
            # nearest palette via CIEDE2000
            best_d, best_i = 1e30, 0
            for i, (PL, Pa, Pb) in enumerate(palette_lab):
                d = ciede2000(L, a, b, PL, Pa, Pb)
                if d < best_d:
                    best_d, best_i = d, i
            idx_out[y, x] = best_i
            dL = L - palette_lab[best_i, 0]
            da = a - palette_lab[best_i, 1]
            db = b - palette_lab[best_i, 2]
            xr = x + dx_fwd
            xl = x - dx_fwd
            if 0 <= xr < w:
                err_cur[xr] += (dL*W7, da*W7, db*W7)
            if y + 1 < h:
                if 0 <= xl < w:
                    err_nxt[xl] += (dL*W3, da*W3, db*W3)
                err_nxt[x] += (dL*W5, da*W5, db*W5)
                if 0 <= xr < w:
                    err_nxt[xr] += (dL*W1, da*W1, db*W1)
        # roll rows
        err_cur, err_nxt = err_nxt, err_cur
        err_nxt[:] = 0.0
        serp = not serp
        if y % 32 == 0:
            print(f"  py dither row {y}/{h}", file=sys.stderr)
    return idx_out

def write_bmp(path, rgb):
    """24-bit BI_RGB BMP, top-down via negative height."""
    h, w, _ = rgb.shape
    row_stride = (w * 3 + 3) & ~3
    pad = row_stride - w * 3
    file_sz = 54 + row_stride * h
    with open(path, "wb") as fp:
        fp.write(b"BM" + struct.pack("<IHHI", file_sz, 0, 0, 54))
        fp.write(struct.pack("<IiiHHIIiiII",
                             40, w, -h, 1, 24, 0, 0, 0, 0, 0, 0))
        bgr = rgb[:, :, ::-1]
        zero = b"\x00" * pad
        for y in range(h):
            fp.write(bgr[y].tobytes())
            if pad:
                fp.write(zero)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("out_prefix")
    ap.add_argument("--no-adjust", action="store_true",
                    help="skip apply_adjust (raw dither only)")
    ap.add_argument("--palette-json", default=None,
                    help="load palette LAB from JSON (same schema as "
                         "epd6color_palette.json).  Otherwise the C-side measured "
                         "palette is used.")
    ap.add_argument("--adjust-json", default=None,
                    help="load brightness/contrast/saturation/gamma/temperature "
                         "from JSON (same schema as epd6color_adjust.json).")
    ap.add_argument("--recompute-lab", action="store_true",
                    help="ignore JSON LAB values; recompute D65 sRGB→LAB from RGB. "
                         "Use this to bypass colormath-corrupted LAB columns.")
    args = ap.parse_args()

    data = Path(args.input).read_bytes()
    if len(data) != TW * TH * 3:
        sys.exit(f"input must be {TW*TH*3} bytes, got {len(data)}")
    rgb = np.frombuffer(data, dtype=np.uint8).reshape(TH, TW, 3).copy()

    adjust_kwargs = {}
    if args.adjust_json:
        cfg = json.loads(Path(args.adjust_json).read_text())
        adjust_kwargs = dict(brightness=cfg.get("brightness", 0),
                             contrast=cfg.get("contrast", 100),
                             saturation=cfg.get("saturation", 100),
                             gamma=cfg.get("gamma", 100),
                             temperature=cfg.get("temperature", 100))
    else:
        adjust_kwargs = dict(brightness=0, contrast=104, saturation=112, gamma=98, temperature=108)

    if not args.no_adjust:
        rgb = apply_adjust_rgb(rgb, **adjust_kwargs)

    global PALETTE_RGB
    if args.palette_json:
        # Schema: { "black": {"rgb":[r,g,b], "lab":[L,a,b]}, ... }
        pal = json.loads(Path(args.palette_json).read_text())
        pal_rgb = np.array([pal[n]["rgb"] for n in PALETTE_NAMES], dtype=np.uint8)
        if args.recompute_lab:
            pal_lab = rgb_to_lab(pal_rgb[None, :, :])[0]
        else:
            pal_lab = np.array([pal[n]["lab"] for n in PALETTE_NAMES], dtype=np.float64)
        PALETTE_RGB = pal_rgb
    else:
        pal_lab = rgb_to_lab(PALETTE_RGB_MEASURED[None, :, :])[0]
        PALETTE_RGB = PALETTE_RGB_MEASURED
    print("palette LAB:")
    for n, p, l in zip(PALETTE_NAMES, PALETTE_RGB, pal_lab):
        print(f"  {n:<7} rgb={tuple(p)}  lab={tuple(round(x,2) for x in l)}")

    lab = map_source_to_panel_lab(rgb_to_lab(rgb), pal_lab)
    idx = dither_ved_fs(lab, pal_lab)

    Path(f"{args.out_prefix}_indices.bin").write_bytes(idx.tobytes())
    preview = PALETTE_RGB[idx]
    write_bmp(f"{args.out_prefix}.bmp", preview)

    hist = np.bincount(idx.flatten(), minlength=6)
    print("python histogram:")
    for n, h in zip(PALETTE_NAMES, hist):
        print(f"  {n:<7} : {h} ({100*h/hist.sum():.1f}%)")
    print(f"wrote {args.out_prefix}.bmp and {args.out_prefix}_indices.bin")

if __name__ == "__main__":
    main()

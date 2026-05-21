// Standalone sanity check of the gamut LUT logic — re-implements the
// helpers used by buildGamutMapLut in isolation, without trying to eval
// the full HTML file.

const _srgbLinearLut = (() => {
  const lut = new Float32Array(256);
  for (let i = 0; i < 256; i++) {
    const v = i / 255;
    lut[i] = v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
  }
  return lut;
})();
const _LAB_EPS = 216 / 24389, _LAB_KAPPA = 24389 / 27;
function _labF(t) { return t > _LAB_EPS ? Math.cbrt(t) : (_LAB_KAPPA * t + 16) / 116; }
function srgbToLab(r, g, b) {
  const R = _srgbLinearLut[r|0], G = _srgbLinearLut[g|0], B = _srgbLinearLut[b|0];
  const X = (0.4124*R + 0.3576*G + 0.1805*B) / 0.95047;
  const Y =  0.2126*R + 0.7152*G + 0.0722*B;
  const Z = (0.0193*R + 0.1192*G + 0.9505*B) / 1.08883;
  const fx = _labF(X), fy = _labF(Y), fz = _labF(Z);
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
}

// All values from PaperColor-Cali-Data-20260521.xlsx column B/C/D
// ("Read from machine", D65 measured).
const PALETTE = {
  black:  [72, 71, 82],
  white:  [151, 161, 161],
  yellow: [164, 154, 69],
  red:    [120, 70, 71],
  blue:   [60, 99, 136],
  green:  [79, 103, 77],
};
const MIXTURES = {
  white_black_25:  [130, 138, 142],
  white_black_50:  [112, 118, 123],
  white_black_75:  [93, 96, 104],
  white_yellow_25: [155, 159, 141],
  white_yellow_50: [158, 157, 121],
  white_yellow_75: [161, 156, 97],
  white_red_25:    [143, 142, 141],
  white_red_50:    [136, 122, 120],
  white_red_75:    [129, 99, 97],
  white_blue_25:   [133, 148, 155],
  white_blue_50:   [113, 134, 149],
  white_blue_75:   [90, 118, 143],
  white_green_25:  [141, 151, 140],
  white_green_50:  [127, 140, 121],
  white_green_75:  [128, 140, 121],
  black_yellow_50: [126, 118, 81],
  black_red_50:    [96, 72, 78],
  black_blue_50:   [70, 81, 104],
  black_green_50:  [80, 93, 95],
  yellow_red_50:   [146, 122, 72],
  yellow_blue_50:  [125, 132, 111],
  yellow_green_50: [132, 137, 79],
  red_blue_50:     [95, 84, 106],
  red_green_50:    [103, 96, 81],
  blue_green_50:   [73, 107, 118],
  skin_wyr:        [151, 153, 136],
  sky_wb:          [132, 147, 156],
  foliage_gyk:     [108, 119, 87],
};

// Build all measured Lab points.
const labs = Object.values(PALETTE).map(rgb => srgbToLab(...rgb));
for (const rgb of Object.values(MIXTURES)) labs.push(srgbToLab(...rgb));

let lMin = Infinity, lMax = -Infinity;
for (const lab of labs) { if (lab[0] < lMin) lMin = lab[0]; if (lab[0] > lMax) lMax = lab[0]; }

// Cusps from primary inks only — must be truly chromatic (C >= 12) so
// the slight-tint neutrals (measured black has C≈7) don't claim hue cusps.
const inkCusps = [];
for (const name of ['black','white','yellow','red','blue','green']) {
  const rgb = PALETTE[name];
  const lab = srgbToLab(...rgb);
  const C = Math.hypot(lab[1], lab[2]);
  if (C < 12) continue;
  const h = (Math.atan2(lab[2], lab[1]) + 2 * Math.PI) % (2 * Math.PI);
  inkCusps.push({ L: lab[0], C, h, name });
}
const lMidFallback = (lMin + lMax) * 0.5;
function cuspAt(h) {
  if (inkCusps.length === 0) return { C: 0, L: lMidFallback };
  let best = inkCusps[0], bestDH = Infinity;
  for (const c of inkCusps) {
    let dh = Math.abs(h - c.h);
    if (dh > Math.PI) dh = 2 * Math.PI - dh;
    if (dh < bestDH) { bestDH = dh; best = c; }
  }
  const att = Math.exp(-bestDH * bestDH * 2.5);
  return { C: best.C * att, L: best.L };
}

function buildLut() {
  const STEPS = 17;
  const lut = new Float32Array(STEPS * STEPS * STEPS * 3);
  const scale = 255 / (STEPS - 1);
  for (let ri = 0; ri < STEPS; ri++) {
    const R = Math.round(ri * scale);
    for (let gi = 0; gi < STEPS; gi++) {
      const G = Math.round(gi * scale);
      for (let bi = 0; bi < STEPS; bi++) {
        const B = Math.round(bi * scale);
        const lab = srgbToLab(R, G, B);
        const Lin = lab[0], ain = lab[1], bin_ = lab[2];
        const Cin = Math.hypot(ain, bin_);
        const hin = Cin > 0.5 ? (Math.atan2(bin_, ain) + 2 * Math.PI) % (2 * Math.PI) : 0;
        const cusp = cuspAt(hin);
        const tLn = Math.max(0, Math.min(1, Lin / 100));
        const Lneutral = lMin + (lMax - lMin) * tLn;
        const cuspW = Cin > 5 ? Math.min(1, (Cin - 5) / Math.max(1, cusp.C - 5)) : 0;
        const Lout = Lneutral + (cusp.L - Lneutral) * cuspW * 0.55;
        const lRangeUp = Math.max(1, lMax - cusp.L);
        const lRangeDown = Math.max(1, cusp.L - lMin);
        const lDist = Lout > cusp.L ? (Lout - cusp.L) / lRangeUp : (cusp.L - Lout) / lRangeDown;
        const cMaxAtL = Math.max(0, cusp.C * (1 - lDist));
        const knee = cMaxAtL * 0.80;
        let Cout = Cin;
        if (Cin > knee) {
          Cout = knee + (Cin - knee) / (1 + (Cin - knee) / Math.max(1, cMaxAtL * 0.5));
          if (Cout > cMaxAtL) Cout = cMaxAtL;
        }
        const aOut = Cin > 0.5 ? Math.cos(hin) * Cout : 0;
        const bOut = Cin > 0.5 ? Math.sin(hin) * Cout : 0;
        const k = ((ri * STEPS + gi) * STEPS + bi) * 3;
        lut[k] = Lout; lut[k + 1] = aOut; lut[k + 2] = bOut;
      }
    }
  }
  return lut;
}

const lut = buildLut();
const STEPS = 17;
function lookup(r, g, b) {
  const ri = Math.round(r * (STEPS - 1) / 255);
  const gi = Math.round(g * (STEPS - 1) / 255);
  const bi = Math.round(b * (STEPS - 1) / 255);
  const k = ((ri * STEPS + gi) * STEPS + bi) * 3;
  return [lut[k], lut[k + 1], lut[k + 2]];
}

console.log('panel L* range:', lMin.toFixed(2), '..', lMax.toFixed(2));
console.log('primary-ink cusps:');
for (const c of inkCusps) {
  console.log(`  ${c.name.padEnd(7)} h=${(c.h*180/Math.PI).toFixed(1).padStart(6)}°  C*=${c.C.toFixed(1).padStart(5)}  L*=${c.L.toFixed(1).padStart(5)}`);
}

console.log();
console.log('  source RGB      |  source Lab        |  LUT Lab (target)  | drift');
console.log('  ----------------+--------------------+--------------------+------');
const samples = [
  [255,255,255, 'pure white'],
  [0,0,0,       'pure black'],
  [128,128,128, 'mid grey'],
  [255,0,0,     'pure red'],
  [255,255,0,   'pure yellow'],
  [0,255,0,     'pure green'],
  [0,0,255,     'pure blue'],
  [200,100,80,  'warm skin-ish'],
  [60,120,180,  'blue sky'],
  [255,200,160, 'bright skin'],
  [240,40,60,   'vivid sunset red'],
  [220,180,80,  'gold'],
  [255,128,0,   'orange'],
];
for (const [r,g,b,name] of samples) {
  const src = srgbToLab(r,g,b);
  const lut3 = lookup(r,g,b);
  const fmt = a => a.map(v => v.toFixed(1).padStart(6)).join(' ');
  const drift = Math.hypot(src[0]-lut3[0], src[1]-lut3[1], src[2]-lut3[2]).toFixed(1);
  console.log('  ' + [r,g,b].join(',').padEnd(15) + ' | ' + fmt(src) + '   | ' + fmt(lut3) + '   | ΔE76=' + drift + '   [' + name + ']');
}

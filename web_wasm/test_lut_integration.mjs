// Integration test: builds the gamut LUT exactly as JS does, pushes it
// into WASM via wasm_set_gamut_lut, runs the e6 dither on solid red, and
// asserts the result is sensible.
import {readFileSync} from 'node:fs';

const W = 400, H = 600;
const wasmBytes = readFileSync(new URL('../main/web/dither.wasm', import.meta.url));
let exports = null;
const fdWrite = (fd, iov, iovcnt, nwrittenPtr) => {
  const view = new DataView(exports.memory.buffer);
  let total = 0;
  for (let i = 0; i < iovcnt; i++) total += view.getUint32(iov + i*8 + 4, true);
  view.setUint32(nwrittenPtr, total, true);
  return 0;
};

const mod = await WebAssembly.instantiate(wasmBytes, {
  wasi_snapshot_preview1: {
    proc_exit: c => { throw new Error('wasm exit ' + c); },
    fd_close: () => 0, fd_seek: () => 0, fd_write: fdWrite, fd_read: () => 0,
    environ_sizes_get: () => 0, environ_get: () => 0,
  },
  env: { emscripten_notify_memory_growth: () => {} },
});
exports = mod.instance.exports;
const e = exports;
if (e._initialize) e._initialize();
e.wasm_init();
if (!e.wasm_set_gamut_lut || !e.wasm_clear_gamut_lut) {
  throw new Error('missing gamut LUT exports');
}

// --- Build the gamut LUT (mirrors index.html buildGamutMapLut) ---
const _srgbLinearLut = new Float32Array(256);
for (let i = 0; i < 256; i++) {
  const v = i / 255;
  _srgbLinearLut[i] = v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
}
const EPS = 216/24389, KAPPA = 24389/27;
const labF = t => t > EPS ? Math.cbrt(t) : (KAPPA * t + 16) / 116;
function srgbToLab(r, g, b) {
  const R = _srgbLinearLut[r|0], G = _srgbLinearLut[g|0], B = _srgbLinearLut[b|0];
  const X = (0.4124*R + 0.3576*G + 0.1805*B) / 0.95047;
  const Y =  0.2126*R + 0.7152*G + 0.0722*B;
  const Z = (0.0193*R + 0.1192*G + 0.9505*B) / 1.08883;
  return [116*labF(Y) - 16, 500*(labF(X) - labF(Y)), 200*(labF(Y) - labF(Z))];
}
const PALETTE = [
  [72,71,82],   [151,161,161], [164,154,69],
  [120,70,71],  [60,99,136],   [79,103,77],
];
const MIX = [
  [130,138,142],[112,118,123],[93,96,104],
  [161,162,138],[167,161,110],[167,158,90],
  [144,137,137],[137,117,116],[130,95,95],
  [128,142,152],[105,130,148],[85,116,144],
  [134,144,137],[115,134,121],[98,124,100],
  [122,113,76], [97,71,76],   [65,87,108],  [76,89,78],
  [144,113,51], [114,130,102],[124,130,70],
  [90,85,105],  [99,88,73],   [73,102,109],
  [186,145,124],[110,137,154],[102,109,76],
];
const labs = [...PALETTE, ...MIX].map(rgb => srgbToLab(...rgb));
let lMin = Infinity, lMax = -Infinity;
for (const lab of labs) { if (lab[0] < lMin) lMin = lab[0]; if (lab[0] > lMax) lMax = lab[0]; }
const NHUE = 24;
const envC = new Float32Array(NHUE);
const envLAt = new Float32Array(NHUE);
for (let i = 0; i < NHUE; i++) envLAt[i] = (lMin + lMax) * 0.5;
for (const lab of labs) {
  const C = Math.hypot(lab[1], lab[2]); if (C < 0.5) continue;
  const h = (Math.atan2(lab[2], lab[1]) + 2*Math.PI) % (2*Math.PI);
  const bin = Math.floor(h / (2*Math.PI) * NHUE) % NHUE;
  if (C > envC[bin]) { envC[bin] = C; envLAt[bin] = lab[0]; }
}
const lMidFallback = (lMin + lMax) * 0.5;
function cuspAt(h) {
  let bestEffC = 0, bestRawC = 0, bestL = lMidFallback;
  for (let i = 0; i < NHUE; i++) {
    if (envC[i] < 0.5) continue;
    const ba = (i + 0.5) / NHUE * 2 * Math.PI;
    let dh = Math.abs(h - ba); if (dh > Math.PI) dh = 2*Math.PI - dh;
    if (dh > Math.PI/4) continue;
    const att = Math.exp(-dh*dh*4);
    const effC = envC[i] * att;
    if (effC > bestEffC) bestEffC = effC;
    if (envC[i] > bestRawC) { bestRawC = envC[i]; bestL = envLAt[i]; }
  }
  return { C: bestEffC, L: bestL };
}
const STEPS = 17;
const lut = new Float32Array(STEPS*STEPS*STEPS*3);
const scale = 255/(STEPS-1);
for (let ri = 0; ri < STEPS; ri++) {
  for (let gi = 0; gi < STEPS; gi++) {
    for (let bi = 0; bi < STEPS; bi++) {
      const lab = srgbToLab(Math.round(ri*scale), Math.round(gi*scale), Math.round(bi*scale));
      const Lin = lab[0], ain = lab[1], bin_ = lab[2];
      const Cin = Math.hypot(ain, bin_);
      const hin = Cin > 0.5 ? (Math.atan2(bin_, ain) + 2*Math.PI) % (2*Math.PI) : 0;
      const cusp = cuspAt(hin);
      const tLn = Math.max(0, Math.min(1, Lin/100));
      const Lneutral = lMin + (lMax-lMin)*tLn;
      const cuspW = Cin > 5 ? Math.min(1, (Cin-5)/Math.max(1, cusp.C-5)) : 0;
      const Lout = Lneutral + (cusp.L - Lneutral) * cuspW * 0.55;
      const lDist = Lout > cusp.L ? (Lout - cusp.L)/Math.max(1, lMax - cusp.L)
                                  : (cusp.L - Lout)/Math.max(1, cusp.L - lMin);
      const cMaxAtL = Math.max(0, cusp.C * (1 - lDist));
      const knee = cMaxAtL * 0.80;
      let Cout = Cin;
      if (Cin > knee) {
        Cout = knee + (Cin-knee)/(1 + (Cin-knee)/Math.max(1, cMaxAtL*0.5));
        if (Cout > cMaxAtL) Cout = cMaxAtL;
      }
      const aOut = Cin > 0.5 ? Math.cos(hin) * Cout : 0;
      const bOut = Cin > 0.5 ? Math.sin(hin) * Cout : 0;
      const k = ((ri*STEPS + gi)*STEPS + bi)*3;
      lut[k] = Lout; lut[k+1] = aOut; lut[k+2] = bOut;
    }
  }
}

// Push the LUT into WASM.
const lutPtr = e.malloc(lut.byteLength);
new Float32Array(e.memory.buffer, lutPtr, lut.length).set(lut);
e.wasm_set_gamut_lut(lutPtr);
e.free(lutPtr);

// Dither solid red, same as smoke_test.mjs.
const rgbLen = W * H * 3;
const rgbPtr = e.malloc(rgbLen);
const heap = new Uint8Array(e.memory.buffer, rgbPtr, rgbLen);
for (let i = 0; i < rgbLen; i += 3) { heap[i] = 206; heap[i+1] = 38; heap[i+2] = 54; }
const cfgPtr = e.malloc(28);
new Int32Array(e.memory.buffer, cfgPtr, 7).set([0,104,112,98,108,98,12]);
const packedLen = W * H / 2;
const packedPtr = e.malloc(packedLen);

const t0 = performance.now();
const rc = e.wasm_dither_e6(rgbPtr, W, H, cfgPtr, packedPtr, 0);
const t1 = performance.now();
if (rc !== 0) throw new Error('dither_e6 rc=' + rc);
console.log(`LUT-on dither rc=0, ${(t1-t0).toFixed(0)} ms`);

const packed = new Uint8Array(e.memory.buffer, packedPtr, packedLen);
const counts = {};
for (let i = 0; i < packedLen; i++) {
  counts[packed[i] >> 4] = (counts[packed[i] >> 4] || 0) + 1;
  counts[packed[i] & 0xF] = (counts[packed[i] & 0xF] || 0) + 1;
}
console.log('LUT-on histogram:', counts);

// Clear LUT and re-run to confirm fallback path also works.
e.wasm_clear_gamut_lut();
for (let i = 0; i < rgbLen; i += 3) { heap[i] = 206; heap[i+1] = 38; heap[i+2] = 54; }
const t2 = performance.now();
const rc2 = e.wasm_dither_e6(rgbPtr, W, H, cfgPtr, packedPtr, 0);
const t3 = performance.now();
if (rc2 !== 0) throw new Error('fallback dither_e6 rc=' + rc2);
console.log(`LUT-off dither rc=0, ${(t3-t2).toFixed(0)} ms`);
const counts2 = {};
for (let i = 0; i < packedLen; i++) {
  counts2[packed[i] >> 4] = (counts2[packed[i] >> 4] || 0) + 1;
  counts2[packed[i] & 0xF] = (counts2[packed[i] & 0xF] || 0) + 1;
}
console.log('LUT-off histogram:', counts2);

const reserved = (counts[4] || 0) + (counts[7] || 0);
if (reserved !== 0) throw new Error('reserved ink in LUT-on output: ' + reserved);
console.log('OK — LUT integration end-to-end runs clean');

// Smoke test for the v2 (reflectance) color-render mode.
// Runs the default browser path (wasm_dither_e6_15x, 600x900 -> 400x600) in all
// three render modes (6color=0, mixpatch=1, reflectance=2) on a synthetic image
// with a tone gradient + saturated color bands, and verifies:
//   * rc == 0
//   * no reserved ink codes (0x4 / 0x7) ever appear
//   * output is non-degenerate (>1 distinct ink) and uses all-ish of the palette
//   * reflectance mode produces a DIFFERENT halftone than 6color (gating works)
import {readFileSync} from 'node:fs';

const PW = 400, PH = 600;          // panel
const SW = PW * 3 / 2, SH = PH * 3 / 2;   // 1.5x source = 600x900
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
for (const fn of ['wasm_dither_e6_15x','wasm_set_color_render_mode','wasm_get_color_render_mode'])
  if (!e[fn]) throw new Error('missing export: ' + fn);

// Synthetic source: top->bottom luminance gradient, overlaid with 6 vertical
// saturated color bands so every ink has a reason to appear.
const BANDS = [[230,40,40],[40,120,230],[40,180,60],[240,210,30],[20,20,20],[245,245,245]];
const rgbLen = SW * SH * 3;
const rgbPtr = e.malloc(rgbLen);
function paint() {
  const h = new Uint8Array(e.memory.buffer, rgbPtr, rgbLen);
  for (let y = 0; y < SH; y++) {
    const g = Math.round(255 * y / (SH - 1));           // vertical gradient
    for (let x = 0; x < SW; x++) {
      const band = BANDS[Math.floor(x / (SW / BANDS.length))];
      const i = (y * SW + x) * 3;
      // blend band color with the gradient so there's tone within color
      h[i]   = Math.round(0.55 * band[0] + 0.45 * g);
      h[i+1] = Math.round(0.55 * band[1] + 0.45 * g);
      h[i+2] = Math.round(0.55 * band[2] + 0.45 * g);
    }
  }
}
const cfgPtr = e.malloc(28);
new Int32Array(e.memory.buffer, cfgPtr, 7).set([0,100,100,100,100,100,30]);
const packedLen = PW * PH / 2;
const packedPtr = e.malloc(packedLen);

const NAMES = ['black','white','yellow','red','blue','green'];
const histos = {};
for (const mode of [0, 1, 2]) {
  e.wasm_set_color_render_mode(mode);
  if (e.wasm_get_color_render_mode() !== mode) throw new Error('mode set/get mismatch');
  paint();
  const t0 = performance.now();
  const rc = e.wasm_dither_e6_15x(rgbPtr, PW, PH, cfgPtr, packedPtr, 0);
  const t1 = performance.now();
  if (rc !== 0) throw new Error(`mode ${mode}: rc=${rc}`);
  const packed = new Uint8Array(e.memory.buffer, packedPtr, packedLen);
  const counts = new Array(8).fill(0);
  const code2idx = {0:0,1:1,2:2,3:3,5:4,6:5};
  for (let i = 0; i < packedLen; i++) { counts[packed[i] >> 4]++; counts[packed[i] & 0xF]++; }
  if ((counts[4] + counts[7]) !== 0) throw new Error(`mode ${mode}: reserved ink codes appeared`);
  const idxCounts = NAMES.map((_, k) => 0);
  for (const [code, idx] of Object.entries(code2idx)) idxCounts[idx] = counts[code];
  const distinct = idxCounts.filter(c => c > 0).length;
  if (distinct < 2) throw new Error(`mode ${mode}: degenerate (only ${distinct} ink)`);
  const total = PW * PH;
  const pct = idxCounts.map(c => (100*c/total).toFixed(1).padStart(5));
  histos[mode] = packed.slice();
  console.log(`mode ${mode} (${['6color','mixpatch','reflectance'][mode]}): rc=0 ${(t1-t0).toFixed(0)}ms  ` +
              NAMES.map((n,i)=>`${n}=${pct[i]}%`).join(' '));
}

// Gating sanity: reflectance must differ from 6color.
let diff = 0;
for (let i = 0; i < packedLen; i++) if (histos[0][i] !== histos[2][i]) diff++;
const diffPct = (100 * diff / packedLen).toFixed(1);
console.log(`reflectance vs 6color: ${diffPct}% of bytes differ`);
if (diff === 0) throw new Error('reflectance mode produced identical output to 6color (gating broken)');

e.free(rgbPtr); e.free(cfgPtr); e.free(packedPtr);
console.log('OK');

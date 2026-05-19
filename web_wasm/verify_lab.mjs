import {readFileSync} from 'node:fs';
const wasmBytes = readFileSync(new URL('../main/web/dither.wasm', import.meta.url));
let exports = null;
const fdWrite = (fd, iov, iovcnt, n) => {
  const v = new DataView(exports.memory.buffer);
  let t = 0; for (let i = 0; i < iovcnt; i++) t += v.getUint32(iov + i*8 + 4, true);
  v.setUint32(n, t, true); return 0;
};
const mod = await WebAssembly.instantiate(wasmBytes, {
  wasi_snapshot_preview1: { proc_exit: c => { throw c }, fd_close: () => 0, fd_seek: () => 0, fd_write: fdWrite, fd_read: () => 0, environ_sizes_get: () => 0, environ_get: () => 0 },
  env: { emscripten_notify_memory_growth: () => {} },
});
exports = mod.instance.exports;
if (exports._initialize) exports._initialize();
exports.wasm_init();
const palette = {
  black:  [94, 64, 70],
  white:  [210, 186, 166],
  yellow: [208, 165, 27],
  red:    [152, 39, 25],
  blue:   [97, 110, 151],
  green:  [129, 130, 95],
};
const lab = exports.malloc(12);
const arr = new Float32Array(exports.memory.buffer, lab, 3);
console.log('name      RGB              CORRECT LAB             JSON LAB');
const json = {
  black:  [28.84,  9.91, -16.86],
  white:  [74.75, -1.70, -14.36],
  yellow: [63.09,-19.31, -31.57],
  red:    [23.42, 37.58, -62.33],
  blue:   [50.21, 14.45,  13.65],
  green:  [51.82,-12.48,  -3.46],
};
for (const [n, [r,g,b]] of Object.entries(palette)) {
  exports.wasm_rgb_to_lab(r, g, b, lab);
  const L = arr[0].toFixed(2), a = arr[1].toFixed(2), b2 = arr[2].toFixed(2);
  const j = json[n].map(x => x.toFixed(2)).join(', ');
  console.log(`${n.padEnd(8)} [${r},${g},${b}].padEnd(15) [${L}, ${a}, ${b2}]  vs  [${j}]`);
}

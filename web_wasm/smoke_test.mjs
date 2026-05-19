// Smoke test for dither.wasm. Builds a 400×600 RGB888 buffer of solid red,
// runs the WASM pipeline, then checks every output nibble is in {0..6}\{4,7}
// and that "red" pixels (ink code 3) dominate.
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

const rgbLen = W * H * 3;
const rgbPtr = e.malloc(rgbLen);
const heap = new Uint8Array(e.memory.buffer, rgbPtr, rgbLen);
for (let i = 0; i < rgbLen; i += 3) { heap[i] = 206; heap[i+1] = 38; heap[i+2] = 54; }

const cfgPtr = e.malloc(28);
new Int32Array(e.memory.buffer, cfgPtr, 7).set([0,104,112,98,108,98,12]);

const packedLen = W * H / 2;
const packedPtr = e.malloc(packedLen);

const t0 = performance.now();
const rc = e.wasm_dither(rgbPtr, W, H, cfgPtr, packedPtr, 0);
const t1 = performance.now();
if (rc !== 0) throw new Error('dither rc=' + rc);
console.log(`dither rc=0, ${(t1-t0).toFixed(0)} ms`);

const packed = new Uint8Array(e.memory.buffer, packedPtr, packedLen);
const counts = {};
for (let i = 0; i < packedLen; i++) {
  const hi = packed[i] >> 4, lo = packed[i] & 0xF;
  counts[hi] = (counts[hi] || 0) + 1;
  counts[lo] = (counts[lo] || 0) + 1;
}
console.log('ink-code histogram:', counts);

const reserved = (counts[4] || 0) + (counts[7] || 0);
if (reserved !== 0) throw new Error('reserved ink codes appeared: ' + reserved);
if (!counts[3] || counts[3] < W*H/2) throw new Error('solid red did not dither mostly to red ink 0x3');
console.log('OK');

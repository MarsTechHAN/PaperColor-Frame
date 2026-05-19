import {readFileSync} from 'node:fs';
const W = 4, H = 6;
const wasmBytes = readFileSync(new URL('../main/web/dither.wasm', import.meta.url));
const mod = await WebAssembly.instantiate(wasmBytes, {
  wasi_snapshot_preview1: {
    proc_exit: c => { throw new Error('wasm exit ' + c); },
    fd_close: () => 0, fd_seek: () => 0, fd_write: () => 0, fd_read: () => 0,
    environ_sizes_get: () => 0, environ_get: () => 0,
  },
  env: { emscripten_notify_memory_growth: () => {} },
});
const e = mod.instance.exports;
if (e._initialize) e._initialize();
e.wasm_init();
const rgbPtr = e.malloc(W*H*3);
new Uint8Array(e.memory.buffer, rgbPtr, W*H*3).fill(128);  // gray
const packedPtr = e.malloc(W*H/2);
console.log('starting dither 4x6...');
const t0 = Date.now();
const rc = e.wasm_dither(rgbPtr, W, H, 0, packedPtr);
console.log('rc=', rc, 'ms=', Date.now()-t0);
const packed = new Uint8Array(e.memory.buffer, packedPtr, W*H/2);
console.log('packed bytes:', Array.from(packed));

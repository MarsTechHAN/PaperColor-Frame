import {readFileSync} from 'node:fs';
const W = 40, H = 60;  // 1/100 the size to test the path
const wasmBytes = readFileSync(new URL('../main/web/dither.wasm', import.meta.url));
console.log('wasm bytes:', wasmBytes.length);
const mod = await WebAssembly.instantiate(wasmBytes, {
  wasi_snapshot_preview1: {
    proc_exit: c => { throw new Error('wasm exit ' + c); },
    fd_close: () => 0, fd_seek: () => 0, fd_write: () => 0, fd_read: () => 0,
    environ_sizes_get: () => 0, environ_get: () => 0,
  },
  env: { emscripten_notify_memory_growth: () => {} },
});
console.log('instantiated');
const e = mod.instance.exports;
console.log('exports:', Object.keys(e));
e.wasm_init();
console.log('wasm_init ok');
const rgbLen = W * H * 3;
const rgbPtr = e.malloc(rgbLen);
console.log('rgbPtr=', rgbPtr);
const heap = new Uint8Array(e.memory.buffer, rgbPtr, rgbLen);
for (let i = 0; i < rgbLen; i += 3) { heap[i] = 206; heap[i+1] = 38; heap[i+2] = 54; }
const cfgPtr = e.malloc(20);
new Int32Array(e.memory.buffer, cfgPtr, 5).set([0,100,100,100,100]);
const packedLen = W * H / 2;
const packedPtr = e.malloc(packedLen);
console.log('calling dither...');
const t0 = Date.now();
const rc = e.wasm_dither(rgbPtr, W, H, cfgPtr, packedPtr);
console.log('rc=', rc, 'ms=', Date.now()-t0);

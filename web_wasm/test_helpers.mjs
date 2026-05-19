import {readFileSync} from 'node:fs';
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
console.log('ciede2000(50,0,0, 50,10,0) =', e.wasm_ciede2000(50,0,0, 50,10,0));
console.log('ciede2000(50,0,0, 50,0,0) =', e.wasm_ciede2000(50,0,0, 50,0,0));
const lab = e.malloc(12);
e.wasm_rgb_to_lab(206, 38, 54, lab);
const out = new Float32Array(e.memory.buffer, lab, 3);
console.log('rgb_to_lab(206,38,54) L,a,b =', Array.from(out));

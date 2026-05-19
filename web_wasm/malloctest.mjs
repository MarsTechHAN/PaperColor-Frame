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
if (e._initialize) { e._initialize(); console.log('initialized'); }
console.log('memory.buffer.byteLength:', e.memory.buffer.byteLength);
const p1 = e.malloc(480);
const p2 = e.malloc(480);
const p3 = e.malloc(480);
console.log('three mallocs:', p1, p2, p3);
e.free(p1); e.free(p2); e.free(p3);
const p4 = e.malloc(480);
console.log('after free, malloc:', p4);

import {readFileSync} from 'node:fs';
const W = 40, H = 60;
const wasmBytes = readFileSync(new URL('../main/web/dither.wasm', import.meta.url));
let mem;
function fdWrite(fd, iov, iovcnt, nwrittenPtr) {
  // Tell the caller every iovec was "fully written".  Returning 0-bytes-written
  // makes libc's write-loop spin forever.
  const view = new DataView(mem.buffer);
  let total = 0;
  for (let i = 0; i < iovcnt; i++) {
    total += view.getUint32(iov + i*8 + 4, true);
  }
  view.setUint32(nwrittenPtr, total, true);
  return 0;
}
const mod = await WebAssembly.instantiate(wasmBytes, {
  wasi_snapshot_preview1: {
    proc_exit: c => { throw new Error('wasm exit ' + c); },
    fd_close: () => 0,
    fd_seek: () => 0,
    fd_write: fdWrite,
    fd_read: () => 0,
    environ_sizes_get: () => 0, environ_get: () => 0,
  },
  env: { emscripten_notify_memory_growth: () => {} },
});
mem = mod.instance.exports.memory;
const e = mod.instance.exports;
if (e._initialize) e._initialize();
e.wasm_init();
console.log('init done');

const rgbLen = W * H * 3;
const rgbPtr = e.malloc(rgbLen);
const heap = new Uint8Array(e.memory.buffer, rgbPtr, rgbLen);
for (let i = 0; i < rgbLen; i += 3) { heap[i] = 206; heap[i+1] = 38; heap[i+2] = 54; }
const packedLen = W * H / 2;
const packedPtr = e.malloc(packedLen);

console.log('dither cfg=null...');
const t0 = Date.now();
const rc = e.wasm_dither(rgbPtr, W, H, 0, packedPtr);
console.log('rc=', rc, 'ms=', Date.now()-t0);

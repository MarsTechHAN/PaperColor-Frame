// png_to_welcome_bin.mjs — convert a source image (JPEG/PNG/etc) into a 400×600
// 4bpp packed E6 framebuffer using the project's WASM dither pipeline.
//
// Pipeline:
//   1. Use macOS `sips` to cover-resize the source to 600×900 and emit a BMP24.
//   2. Decode the BMP24 into a raw 600×900 RGB888 buffer (no extra JS deps).
//   3. Load main/web/dither.wasm with the same WASI shim as smoke_test.mjs.
//   4. Call wasm_dither_e6_15x with the balanced default adjust_cfg_t.
//   5. Write the 120000-byte packed output to disk.
//
// Usage:
//   node web_wasm/png_to_welcome_bin.mjs <input> <output.bin>
//
// Reuses the exact dither path used by the browser upload (ditherCanvas in
// main/web/index.html) so the on-device render matches the web preview.

import {readFileSync, writeFileSync, mkdtempSync, rmSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import {tmpdir} from 'node:os';
import {join} from 'node:path';

// Panel resolution and the 1.5× supersample dims the E6_15x path expects.
const TW = 400, TH = 600;
const SW = 600, SH = 900;

// Balanced-preset defaults the browser stamps onto fresh uploads
// (ensurePipelineConfig -> CALIB_FALLBACK.adjust, post-migration).
// Order matches writeAdjustCfg in index.html:
// [brightness, contrast, saturation, gamma, temperature, tint, smoothness]
const ADJUST_CFG = [0, 106, 136, 96, 106, 99, 10];

function usage() {
  console.error('Usage: node web_wasm/png_to_welcome_bin.mjs <input> <output.bin>');
  process.exit(2);
}

const [, , inPath, outPath] = process.argv;
if (!inPath || !outPath) usage();

// ---------- Step 1: sips cover-resize to 600×900 BMP24 ----------
//
// sips cannot do "cover" in a single step. To emulate cover we proportionally
// resample to the shorter target side first, which leaves the longer side
// >= the target, then center-crop to exactly 600×900. The source 687×1024
// aspect (0.671) and the target 600×900 aspect (0.667) are nearly identical,
// so this introduces only a few pixels of crop on the long edge.

const work = mkdtempSync(join(tmpdir(), 'pe6-welcome-'));
const bmpPath = join(work, 'resized.bmp');
try {
  // First, pick the proportional resample axis: scale so that the *short*
  // side of the result fully covers the target's matching side. That's the
  // standard cover trick.
  const probe = execFileSync('sips', ['-g', 'pixelWidth', '-g', 'pixelHeight', inPath], {encoding: 'utf8'});
  const srcW = parseInt(/pixelWidth:\s*(\d+)/.exec(probe)[1], 10);
  const srcH = parseInt(/pixelHeight:\s*(\d+)/.exec(probe)[1], 10);
  const scale = Math.max(SW / srcW, SH / srcH);
  const resampledH = Math.max(SH, Math.round(srcH * scale));
  const resampledW = Math.max(SW, Math.round(srcW * scale));

  const stage1 = join(work, 'stage1.png');
  execFileSync('sips', [
    '-z', String(resampledH), String(resampledW),
    inPath, '--out', stage1,
  ], {stdio: ['ignore', 'ignore', 'inherit']});

  // Center-crop to exactly the target.  `-c H W` crops centered.
  const stage2 = join(work, 'stage2.png');
  execFileSync('sips', [
    '-c', String(SH), String(SW),
    stage1, '--out', stage2,
  ], {stdio: ['ignore', 'ignore', 'inherit']});

  // Re-emit as BMP for trivial in-process decoding.
  execFileSync('sips', [
    '-s', 'format', 'bmp',
    stage2, '--out', bmpPath,
  ], {stdio: ['ignore', 'ignore', 'inherit']});

  // ---------- Step 2: BMP24 → RGB888 ----------
  //
  // BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40+).  We support the common
  // case: 24bpp, no compression, either top-down (negative height) or the
  // default bottom-up.  Rows are padded to a multiple of 4 bytes.
  const bmp = readFileSync(bmpPath);
  if (bmp[0] !== 0x42 || bmp[1] !== 0x4D) throw new Error('not a BMP file');
  const dataOff = bmp.readUInt32LE(0x0A);
  const dibSize = bmp.readUInt32LE(0x0E);
  const width   = bmp.readInt32LE(0x12);
  const heightRaw = bmp.readInt32LE(0x16);
  const bpp     = bmp.readUInt16LE(0x1C);
  const compress = bmp.readUInt32LE(0x1E);
  if (width !== SW || Math.abs(heightRaw) !== SH) {
    throw new Error(`BMP dims ${width}×${heightRaw} != ${SW}×${SH}`);
  }
  if (bpp !== 24) throw new Error('expected 24bpp BMP, got ' + bpp);
  if (compress !== 0) throw new Error('expected uncompressed BMP, got compress=' + compress);
  void dibSize; // unused

  const topDown = heightRaw < 0;
  const absH = Math.abs(heightRaw);
  const rowStride = (width * 3 + 3) & ~3;

  const rgb = new Uint8Array(SW * SH * 3);
  for (let y = 0; y < absH; y++) {
    // Row 0 in our output is the top of the image.
    const srcRow = topDown ? y : (absH - 1 - y);
    const srcOff = dataOff + srcRow * rowStride;
    const dstOff = y * SW * 3;
    for (let x = 0; x < SW; x++) {
      // BMP24 stores BGR; we want RGB.
      const b = bmp[srcOff + x * 3 + 0];
      const g = bmp[srcOff + x * 3 + 1];
      const r = bmp[srcOff + x * 3 + 2];
      rgb[dstOff + x * 3 + 0] = r;
      rgb[dstOff + x * 3 + 1] = g;
      rgb[dstOff + x * 3 + 2] = b;
    }
  }

  // ---------- Step 3: load dither.wasm (WASI shim ripped from smoke_test) ----------
  const wasmBytes = readFileSync(new URL('../main/web/dither.wasm', import.meta.url));
  let exportsRef = null;
  const fdWrite = (fd, iov, iovcnt, nwrittenPtr) => {
    const view = new DataView(exportsRef.memory.buffer);
    let total = 0;
    for (let i = 0; i < iovcnt; i++) total += view.getUint32(iov + i * 8 + 4, true);
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
  const e = mod.instance.exports;
  exportsRef = e;
  if (e._initialize) e._initialize();
  e.wasm_init();
  if (!e.wasm_dither_e6_15x) throw new Error('wasm_dither_e6_15x missing');

  // ---------- Step 4: allocate buffers, dither, copy out ----------
  const rgbLen = SW * SH * 3;
  const rgbPtr = e.malloc(rgbLen);
  if (!rgbPtr) throw new Error('malloc rgb failed');
  new Uint8Array(e.memory.buffer, rgbPtr, rgbLen).set(rgb);

  const cfgPtr = e.malloc(28);
  if (!cfgPtr) throw new Error('malloc cfg failed');
  new Int32Array(e.memory.buffer, cfgPtr, 7).set(ADJUST_CFG);

  const packedLen = TW * TH / 2;   // 120000
  const packedPtr = e.malloc(packedLen);
  if (!packedPtr) throw new Error('malloc packed failed');

  const t0 = performance.now();
  const rc = e.wasm_dither_e6_15x(rgbPtr, TW, TH, cfgPtr, packedPtr, 0);
  const t1 = performance.now();
  if (rc !== 0) throw new Error('wasm_dither_e6_15x rc=' + rc);

  // Copy out before freeing — malloc/free may move the heap.
  const packed = new Uint8Array(packedLen);
  packed.set(new Uint8Array(e.memory.buffer, packedPtr, packedLen));

  e.free(rgbPtr); e.free(cfgPtr); e.free(packedPtr);

  // ---------- Step 5: write output ----------
  writeFileSync(outPath, packed);

  // Sanity-sample the slot regions welcome_overlay paints into.  Slot bytes
  // should be mostly white (ink code 0x1) so the text overlay reads cleanly.
  const SLOT_X = 40, SLOT_W = 320, SLOT_H = 40;
  const slots = [
    {name: 'SSID', y: 220},
    {name: 'PWD',  y: 300},
  ];
  function whiteFraction(y0) {
    let white = 0, total = 0;
    for (let y = y0; y < y0 + SLOT_H; y++) {
      for (let x = SLOT_X; x < SLOT_X + SLOT_W; x += 2) {
        const byteIdx = (y * TW + x) >> 1;
        const b = packed[byteIdx];
        const hi = b >> 4, lo = b & 0xF;
        if (hi === 1) white++;
        if (lo === 1) white++;
        total += 2;
      }
    }
    return white / total;
  }
  const ssidWhite = whiteFraction(slots[0].y);
  const pwdWhite  = whiteFraction(slots[1].y);

  console.log(`OK ${outPath} (${packed.length} bytes, ${(t1 - t0).toFixed(0)} ms dither)`);
  console.log(`  slot SSID y=${slots[0].y}: white=${(ssidWhite * 100).toFixed(1)}%`);
  console.log(`  slot PWD  y=${slots[1].y}: white=${(pwdWhite  * 100).toFixed(1)}%`);
} finally {
  try { rmSync(work, {recursive: true, force: true}); } catch {}
}

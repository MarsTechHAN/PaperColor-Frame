// image_to_welcome_flat.mjs — convert a palette-locked welcome illustration
// into a 400×600 4bpp E6 framebuffer using FLAT nearest-color mapping.
//
// Unlike png_to_welcome_bin.mjs (which runs the full FS-dither pipeline),
// this path assumes the input image was already drawn with the constrained
// 6-ink palette (the hex codes baked into the nano-banana-pro2 prompt) and
// just snaps each pixel to its closest source anchor — no error diffusion,
// no halftone. Solid color regions stay solid, edges stay crisp.
//
// Usage:
//   node web_wasm/image_to_welcome_flat.mjs <input> <output.bin>

import {readFileSync, writeFileSync, mkdtempSync, rmSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import {tmpdir} from 'node:os';
import {join} from 'node:path';

const TW = 400, TH = 600;

// E6 ink codes (must match palette.h EPD_INK_*).
const INK_BLACK = 0x0, INK_WHITE = 0x1, INK_YELLOW = 0x2;
const INK_RED   = 0x3, INK_BLUE  = 0x5, INK_GREEN  = 0x6;

// Source-side anchor palette. These are the hex values the nano-banana-pro2
// prompt requires the model to use, NOT the panel's measured ink RGBs. The
// generated artwork lives in this color space; the firmware will paint the
// matching ink which renders as the measured value on the physical panel.
const ANCHORS = [
  {name: 'white',  rgb: [0xE9, 0xE6, 0xD7], ink: INK_WHITE},
  {name: 'black',  rgb: [0x2D, 0x2D, 0x33], ink: INK_BLACK},
  {name: 'yellow', rgb: [0xD9, 0xB8, 0x33], ink: INK_YELLOW},
  {name: 'red',    rgb: [0xB8, 0x43, 0x4A], ink: INK_RED},
  {name: 'blue',   rgb: [0x2F, 0x5F, 0x8C], ink: INK_BLUE},
  {name: 'green',  rgb: [0x5A, 0x82, 0x55], ink: INK_GREEN},
];

// sRGB → CIELAB (D65). Same math as rgb_to_lab_u8 in color_pipeline.c so the
// nearest match here mirrors the dither path's color judgment.
function srgbChannel(c) {
  c /= 255;
  return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}
function rgbToLab(r, g, b) {
  const R = srgbChannel(r), G = srgbChannel(g), B = srgbChannel(b);
  // sRGB → XYZ (D65)
  let X = R * 0.4124564 + G * 0.3575761 + B * 0.1804375;
  let Y = R * 0.2126729 + G * 0.7151522 + B * 0.0721750;
  let Z = R * 0.0193339 + G * 0.1191920 + B * 0.9503041;
  // Normalize by D65 white point
  X /= 0.95047; Y /= 1.00000; Z /= 1.08883;
  const f = t => t > 0.008856 ? Math.cbrt(t) : (7.787 * t + 16 / 116);
  const fx = f(X), fy = f(Y), fz = f(Z);
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
}

function usage() {
  console.error('Usage: node web_wasm/image_to_welcome_flat.mjs <input> <output.bin>');
  process.exit(2);
}

const [, , inPath, outPath] = process.argv;
if (!inPath || !outPath) usage();

// Pre-compute anchor LABs.
for (const a of ANCHORS) a.lab = rgbToLab(a.rgb[0], a.rgb[1], a.rgb[2]);

const work = mkdtempSync(join(tmpdir(), 'pe6-welcome-flat-'));
try {
  // Cover-resize to 400×600 using sips (proportional resample + center crop).
  const probe = execFileSync('sips', ['-g', 'pixelWidth', '-g', 'pixelHeight', inPath], {encoding: 'utf8'});
  const srcW = parseInt(/pixelWidth:\s*(\d+)/.exec(probe)[1], 10);
  const srcH = parseInt(/pixelHeight:\s*(\d+)/.exec(probe)[1], 10);
  const scale = Math.max(TW / srcW, TH / srcH);
  const stage1 = join(work, 'stage1.png');
  const stage2 = join(work, 'stage2.png');
  const bmpPath = join(work, 'final.bmp');
  execFileSync('sips', [
    '-z', String(Math.max(TH, Math.round(srcH * scale))),
          String(Math.max(TW, Math.round(srcW * scale))),
    inPath, '--out', stage1,
  ], {stdio: ['ignore', 'ignore', 'inherit']});
  execFileSync('sips', ['-c', String(TH), String(TW), stage1, '--out', stage2],
               {stdio: ['ignore', 'ignore', 'inherit']});
  execFileSync('sips', ['-s', 'format', 'bmp', stage2, '--out', bmpPath],
               {stdio: ['ignore', 'ignore', 'inherit']});

  // Decode BMP24 → RGB888 (top-down, row-stride padded to 4).
  const bmp = readFileSync(bmpPath);
  if (bmp[0] !== 0x42 || bmp[1] !== 0x4D) throw new Error('not a BMP file');
  const dataOff = bmp.readUInt32LE(0x0A);
  const width   = bmp.readInt32LE(0x12);
  const heightRaw = bmp.readInt32LE(0x16);
  const bpp     = bmp.readUInt16LE(0x1C);
  const compress = bmp.readUInt32LE(0x1E);
  if (width !== TW || Math.abs(heightRaw) !== TH) {
    throw new Error(`BMP dims ${width}×${heightRaw} != ${TW}×${TH}`);
  }
  if (bpp !== 24 || compress !== 0) {
    throw new Error(`expected 24bpp uncompressed BMP, got bpp=${bpp} comp=${compress}`);
  }
  const topDown = heightRaw < 0;
  const absH = Math.abs(heightRaw);
  const rowStride = (width * 3 + 3) & ~3;

  // Flat nearest-color mapping in LAB space. Count anchor hits for the report.
  const packed = new Uint8Array(TW * TH / 2);
  const hits = new Array(ANCHORS.length).fill(0);

  for (let y = 0; y < absH; y++) {
    const srcRow = topDown ? y : (absH - 1 - y);
    const srcOff = dataOff + srcRow * rowStride;
    for (let x = 0; x < TW; x++) {
      const b = bmp[srcOff + x * 3 + 0];
      const g = bmp[srcOff + x * 3 + 1];
      const r = bmp[srcOff + x * 3 + 2];
      const lab = rgbToLab(r, g, b);

      let bestI = 0, bestD = Infinity;
      for (let i = 0; i < ANCHORS.length; i++) {
        const al = ANCHORS[i].lab;
        const dl = lab[0] - al[0], da = lab[1] - al[1], db = lab[2] - al[2];
        const d = dl * dl + da * da + db * db;
        if (d < bestD) { bestD = d; bestI = i; }
      }
      hits[bestI]++;
      const ink = ANCHORS[bestI].ink;
      const byteIdx = (y * TW + x) >> 1;
      if ((x & 1) === 0) packed[byteIdx] = (packed[byteIdx] & 0x0F) | ((ink & 0x0F) << 4);
      else               packed[byteIdx] = (packed[byteIdx] & 0xF0) | (ink & 0x0F);
    }
  }

  writeFileSync(outPath, packed);

  // Sample the overlay slot bands so we know the runtime text overlay will
  // land on clean white. SLOT_X / SLOT_Y / SLOT_H must match welcome_overlay.c.
  const SLOT_X = 60, SLOT_W = 280, SLOT_H = 16;
  function slotWhitePct(y0) {
    let white = 0, total = 0;
    for (let y = y0; y < y0 + SLOT_H; y++) {
      for (let x = SLOT_X; x < SLOT_X + SLOT_W; x++) {
        const byteIdx = (y * TW + x) >> 1;
        const v = (x & 1) === 0 ? (packed[byteIdx] >> 4) : (packed[byteIdx] & 0xF);
        if (v === INK_WHITE) white++;
        total++;
      }
    }
    return (white / total) * 100;
  }
  console.log(`OK ${outPath} (${packed.length} bytes, flat nearest in LAB)`);
  for (let i = 0; i < ANCHORS.length; i++) {
    const pct = (hits[i] / (TW * TH)) * 100;
    console.log(`  ${ANCHORS[i].name.padEnd(7)} ${pct.toFixed(1).padStart(5)}%`);
  }
  console.log(`  slot SSID y=210 white=${slotWhitePct(210).toFixed(1)}%`);
  console.log(`  slot PWD  y=282 white=${slotWhitePct(282).toFixed(1)}%`);
} finally {
  try { rmSync(work, {recursive: true, force: true}); } catch {}
}

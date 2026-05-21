# PaperColor Frame 实现说明

本文档描述浏览器、WASM、ESP32 固件、文件存储、校准和电子纸刷新之间的实现关系。算法原理见 [`pipeline.md`](pipeline.md)。

## 代码地图

| 路径 | 作用 |
| --- | --- |
| `main/web/index.html` | 设备内置 Web UI；上传、裁剪、预览、preset、Auto、校准向导、WASM 调用和上传逻辑 |
| `main/web/i18n.js` | Web UI 中英文文案和语言切换 |
| `main/color_pipeline.c` / `.h` | sRGB/Lab/CIEDE2000、基础调色、EINK-aware enhancement |
| `main/palette.c` / `.h` | 六色墨水编码、实测 RGB、实测 Lab 初始化 |
| `main/dither.c` / `.h` | Classic VED/Stucki dither、默认 E6 Mix dither、1.5x supersample、Russian flat-fill |
| `web_wasm/wasm_entry.c` | C 算法暴露给浏览器 WASM 的 ABI shim |
| `web_wasm/build.sh` | 使用 Emscripten 构建 `main/web/dither.wasm` |
| `web_wasm/smoke_test.mjs` | WASM 导出和基础 dither 行为 smoke test |
| `main/http_server.c` | 静态页面、WASM、照片 API、配置 API、校准 API |
| `main/config_store.c` / `.h` | NVS 配置、默认 calibration JSON、Auto/pipeline 默认值 |
| `main/photo_store.c` / `.h` | 图片和预处理 `.bin` 文件存储 |
| `main/loop_display.c` / `.h` | 设备端播放循环、校准色块显示、预处理 framebuffer 优先显示 |
| `main/epd_4in0e.c` / `.h` | 4.0 寸 6 色电子纸驱动 |
| `host_test/` | 桌面端算法验证和 BMP 输出 |

## 浏览器端两阶段流程

Web UI 是两阶段上传流程。

### Stage 1: Crop / Fit / Rotate

用户先完成构图：选择图片、fit、拖拽、旋转、确认裁剪区域。

核心状态在 `main/web/index.html`：

- `xform.src`：源图 bitmap。
- `xform.tx` / `xform.ty`：平移。
- `xform.s`：缩放。
- `xform.rot`：旋转。
- `cropGeom`：进入 Stage 2 时冻结的裁剪几何。

进入 Stage 2 时调用 `freezeCropGeom()`，后续 adjust 不再改变图像几何，避免调色阶段和最终输出不一致。

### Geometry Normalization

`normalizedCropGeom()` 保证 Stage 2 preview、Auto 分析和最终 bake 使用同一套几何：

1. 重新约束 frozen crop 仍然 cover 面板区域，避免白边。
2. 加 `1.001` overscan，抵消浏览器 canvas 子像素采样导致的边缘漏白。

`bakeCroppedCanvas(outW, outH)` 是统一入口；`bakeCanvas()` 固定输出 `600 x 900`，也就是面板 `400 x 600` 的 1.5x supersample。

### Stage 2: Adjust / Preset / Preview

Stage 2 显示 dithered preview，用户可以：

- 选择 preset。
- 使用 Auto。
- 展开 Advanced 调整参数。
- 在 Settings 里选择 dither mode 和 Auto 目标。
- 保存自定义 preset。
- 点击 preview canvas 切换 `source` / `dithered`。

默认 `previewMode = 'dithered'`。Dithered preview 现在走和 Send 相同的 1.5x WASM final path，因此预览的下采样、unsharp、dither、local refinement 和最终上传一致。

## Preset、输出模式和 Dither 模式

内置 preset 定义在 `BUILTIN_PRESETS`：

- `balanced`
- `landscape`
- `portrait`
- `anime`
- `document`
- `russian`

每个 preset 包含：

```js
{
  label: 'Balanced',
  mode: 'dither',          // 可省略，默认 dither
  ditherMode: 'e6-mix',    // 可省略，使用当前/默认 dither mode
  values: { ...adjusts }
}
```

Russian 使用：

```js
mode: 'flat-russian'
```

当前 per-image 参数保存在：

```js
perImageAdjust = {
  brightness,
  exposure,
  contrast,
  saturation,
  vibrance,
  gamma,
  temperature,
  tint,
  smoothness,
  sharpen,
  vignette
}
```

其中：

- `brightness / contrast / saturation / gamma / temperature / tint / smoothness` 写入 WASM 的 `adjust_cfg_t`。
- `exposure / vibrance / sharpen / vignette` 是 JS-side extra adjust，不进入 C ABI。
- `exposure` 的 UI 范围是 `-300..300`，按 EV/100 解释，即 `-3 EV..+3 EV`。

`outputMode` 保存大类输出 pipeline：

- `'dither'`
- `'flat-russian'`

`ditherMode` 保存照片 dither 细分模式：

- `'e6-mix'`：默认，面向真实 Spectra 6 / E6 面板优化。
- `'classic'`：保留旧的 VED/Stucki 行为，用于对比和回退。

自定义 preset 会保存 `values`、`mode` 和 `ditherMode` 到 `localStorage`。

## Settings 中的图像管线配置

Settings 里新增 Image pipeline 区域：

- Dither mode：`E6 Mix` / `Classic VED`。
- Auto AE target：默认 `-1/3 EV`，范围 `-3 EV..+3 EV`。
- Auto temperature bias：默认 `0`，范围 `-20..+20`。

这些值保存在 calibration JSON 中：

```json
{
  "auto": {"exposureBias": -33, "temperatureBias": 0},
  "pipeline": {"ditherMode": "e6-mix"}
}
```

`ensurePipelineConfig()` 会给旧配置补默认值，并把非法 dither mode 归一化到 `e6-mix`。旧默认 `saturation=106, vibrance=24` 会在运行时迁移到新的高饱和默认 `saturation=136, vibrance=64`。

## Auto Adjust

`applyAutoAdjust()` 会：

1. 使用 `bakePreviewCanvas()` 得到当前裁剪图进行快速场景分析。
2. `analyzeCanvasForAuto()` 统计亮度分位数、对比、边缘、chroma、warm/cool/green/blue、肤色、暗角等指标。
3. `buildAutoScene()` / `chooseAutoPreset()` 选择内置 preset。
4. `autoIspExposureTone()`、`autoIspColor()`、`autoIspDetailNoise()` 按 ISP pipeline 思路调 AE/tone、AWB/color、detail/noise。
5. 加入 Settings 的 `auto.exposureBias` 和 `auto.temperatureBias`。
6. `refineAutoAdjustPanelAware()` 在 `120 x 180` 面板网格上跑小尺寸 1.5x WASM dither simulation，按实测 palette/optical luma 给候选参数打分。
7. 设置 `outputMode = 'dither'`，保持用户选择的 `ditherMode`。

Auto 不会自动选择 Russian，因为 Russian 是风格化非照片 pipeline。

## WASM 加载流程

浏览器通过 `loadWasm()` 加载 `/dither.wasm`：

```js
const resp = await fetch('/dither.wasm', {cache: 'no-store'});
const mod = await WebAssembly.instantiate(bytes, imports);
```

`cache: 'no-store'` 用于绕过旧固件中可能缓存的旧 WASM，避免缺少新导出。

加载后封装导出：

```js
wasm = {
  exports,
  memory: exports.memory,
  malloc: exports.malloc,
  free: exports.free,
  init: exports.wasm_init,
  setPalette: exports.wasm_set_palette_lab,
  clearMix: exports.wasm_clear_mix_patches,
  setMixPatch: exports.wasm_set_mix_patch_lab,
  dither: exports.wasm_dither,
  dither15x: exports.wasm_dither_15x,
  ditherE6: exports.wasm_dither_e6,
  ditherE6Mix15x: exports.wasm_dither_e6_15x,
  flatFill: exports.wasm_flat_fill,
  flatFill15x: exports.wasm_flat_fill_15x,
};
```

`loadWasm()` 是 single-flight：多个并发调用会共享同一个加载 Promise，避免重复 instantiate。

## WASM ABI

WASM shim 在 `web_wasm/wasm_entry.c`。

### 初始化

```c
EXPORT void wasm_init(void)
```

内部调用：

```c
color_pipeline_init();
palette_init();
```

### Palette / Mix Calibration

```c
EXPORT void wasm_set_palette_lab(const float *lab18)
EXPORT void wasm_clear_mix_patches(void)
EXPORT void wasm_set_mix_patch_lab(int patch_idx, const float *lab3)
```

浏览器从 `/api/calib` 读取实测 palette RGB，再调用 WASM 的 `wasm_rgb_to_lab()` 转换成 Lab，最后通过 `wasm_set_palette_lab()` 写回 `PALETTE_LAB`。这样 JS 和 C 使用同一个 Lab 实现，避免色差漂移。

Mixed patch 按 `CALIB_TARGETS` 中非 solid 项的顺序映射到 `PALETTE_MIX_PATCH_N=28`。`pushMixPatchesToWasm()` 会先清空 C 层 mixed model，再把已有的 mixed patch Lab 逐项写入；如果没有 mixed patch，dither 自动回退到旧 palette-only 局部平均。

### RGB to Lab Helper

```c
EXPORT void wasm_rgb_to_lab(uint8_t r, uint8_t g, uint8_t b, float *out_lab3)
```

用于浏览器侧校准 palette Lab，不用于批量处理图片。

### Classic Dither

```c
EXPORT int wasm_dither(...)
EXPORT int wasm_dither_15x(...)
```

流程：

```text
apply_adjust_rgb888
enhance_eink_rgb888
dither_ved_fs / dither_ved_fs_15x
```

Classic 用来保留旧输出风格，不再是默认照片路径。

### E6 Mix Dither

```c
EXPORT int wasm_dither_e6(...)
EXPORT int wasm_dither_e6_15x(...)
```

流程：

```text
apply_adjust_rgb888
enhance_eink_rgb888
dither_e6_mix_fs / dither_e6_mix_fs_15x
```

E6 Mix 是默认照片路径。它保留 Classic 的 Lab gamut mapping、Stucki diffusion 和 local refinement，但在 palette pick 中加入面板可见性、肤色/天空/绿植保护和更弱周期的 tie-break。当前版本还会使用 mixed-patch calibration：picker 用已量化邻居估计候选局部混色，refinement 的 5x5 平均用 `palette_mix_model_lab()` 而不是纯墨水 Lab 线性平均。

### Russian Flat-fill

```c
EXPORT int wasm_flat_fill(...)
EXPORT int wasm_flat_fill_15x(...)
```

流程：

```text
apply_adjust_rgb888
enhance_eink_rgb888
flat_fill_constructivist / flat_fill_constructivist_15x
```

Russian 禁用 error diffusion，只做风格化纯色块量化。

## Browser Preview Implementation

`runPreviewUpdate()` 做交互预览。

流程：

1. `bakeCanvas()` 生成 `600 x 900` 裁剪图。
2. 从 canvas RGBA 提取 RGB888。
3. 写入 WASM heap 并先运行 `applyExtraAdjustRgb888()`。
4. 写入 7 个 int32 的 `adjust_cfg_t`。
5. 分配 packed buffer 和 indices buffer。
6. `selectDither15xFunction()` 根据 `outputMode` / `ditherMode` 选择：
   - `wasm.ditherE6Mix15x`
   - `wasm.dither15x`
   - `wasm.flatFill15x`
7. 复制 WASM in-place adjusted `600 x 900` RGB，downsample 成 source preview。
8. 读取 panel-grid indices，按 calibration palette RGB 生成 dithered preview。
9. `applyPanelPreviewOptics()` 用 mixed-patch / optical model 修正预览观感。
10. 释放 WASM 内存。

因此 dithered preview 和最终 Send 的 quantization path 是一致的，区别只在 preview 额外需要 `indices_opt` 来渲染页面。

## Final Upload Implementation

用户点击 Send 后，`ditherCanvas()` 处理最终 framebuffer。

流程：

1. `bakeCanvas()` 生成 `600 x 900`。
2. 从 RGBA canvas 提取 RGB888。
3. JS 侧应用 exposure / vibrance / vignette / sharpen。
4. 写入 WASM heap。
5. 写入 7 个 int32 的 `adjust_cfg_t`。
6. 分配 `TW * TH / 2` packed buffer，当前 `400 * 600 / 2 = 120000` bytes。
7. `selectDither15xFunction()` 选择 E6 Mix、Classic 或 Russian。
8. 复制 packed framebuffer。
9. 释放 WASM 内存。
10. 上传 `.jpg`、`.bin`、`.json` sidecar。

`.bin` 是 4bpp EPD framebuffer，设备显示时可直接使用。

## 4bpp Framebuffer Packing

面板每像素 4 bit，每字节两个像素：

```text
byte high nibble = even pixel
byte low nibble  = odd pixel
```

墨水 code：

| Palette index | Ink | EPD code |
| --- | --- | --- |
| 0 | black | `0x0` |
| 1 | white | `0x1` |
| 2 | yellow | `0x2` |
| 3 | red | `0x3` |
| 4 | blue | `0x5` |
| 5 | green | `0x6` |

`0x4` 和 `0x7` 未使用。

打包逻辑在 `pack_nibble()`、`repack_indices()` 和校准 patch 的 `fb_pack()`。

## 上传文件和 sidecar

Send 成功时通常产生三个文件：

- `p_<timestamp>.jpg`：裁剪后的展示图，用于 gallery 和未来 Tune。
- `p_<timestamp>.bin`：已经预处理好的 4bpp framebuffer。
- `p_<timestamp>.json`：sidecar，保存 per-photo adjust 和 pipeline mode。

sidecar 示例：

```json
{
  "adjust": {
    "brightness": 0,
    "exposure": 0,
    "contrast": 106,
    "saturation": 136,
    "vibrance": 64,
    "gamma": 96,
    "temperature": 106,
    "tint": 99,
    "smoothness": 10,
    "sharpen": 34,
    "vignette": 14
  },
  "mode": "dither",
  "ditherMode": "e6-mix"
}
```

Tune 时如果找到 sidecar，会恢复 adjust、`mode` 和 `ditherMode`。

## Calibration Implementation

校准 JSON 由 `/api/calib` 读写，包含：

- `palette`：六个纯墨水色的 RGB 色差仪读数。
- `adjust`：默认 per-image 调色参数。
- `auto`：Auto AE 和色温补偿目标。
- `pipeline`：默认 dither mode。
- `mixtures`：可选 mixed-patch 色差仪读数。

校准 UI 不再使用 HSL 滑条，而是让用户直接输入色差仪输出：

- RGB：例如 `[151,161,160]`。
- CMYK：例如 `[5%,0%,0%,36%]`，浏览器会转换到 RGB。

`CALIB_TARGETS` 包含六个纯墨水和 28 个 mixed halftone patch。纯墨水读数更新 `palette`；mixed patch 更新 `mixtures.patches`，并在 WASM dither 前推送到 `PALETTE_MIX_PATCHES`。默认 calibration 已内置 `PaperColor-Cali-Data-20260521.xlsx` 的 `Read from machine` 读数。

这份测量还生成了色域分析产物：

- `doc/papercolor_gamut_20260521.png`
- `doc/papercolor_gamut_20260521.svg`
- `doc/papercolor_gamut_samples_20260521.csv`

当前估算 PaperColor 实测 CIE xy 色域约覆盖 sRGB 的 `14.44%`。

### Mixed Patch 显示

`POST /api/calib-mix?mix=<ink>:<weight>,...` 会排队一个校准混色显示命令。设备端 `run_mix_patch()` 生成整屏 4bpp framebuffer，不叠加电量图标。

校准混色图案使用 16x16 tile 的 hash permutation：

- 每个 tile 内墨水比例接近精确，适合色差仪取平均。
- 每个 tile 排列不同，避免 8x8 Bayer ordered screen 在色差仪孔径下形成条带。
- 纹理目标是高频细颗粒，而不是可见周期结构。

如果用户没有测某个 mixed patch，浏览器会 fallback 到六个纯墨水读数和 Yule-Nielsen-like optical model。

## HTTP Server 与嵌入资源

`main/http_server.c` 提供：

- `GET /`：Web UI。
- `GET /i18n.js`：Web UI 文案。
- `GET /dither.wasm`：WASM 模块。
- `GET/POST /api/config`：轮播、电源、LED 等设备配置。
- `GET/POST /api/calib`：palette、mixed patches、Auto 和 pipeline 默认值。
- `POST /api/calib-fill`：显示纯墨水校准色块。
- `POST /api/calib-mix`：显示 mixed halftone 校准色块。
- 图片列表、上传、删除、显示等 API。

`main/CMakeLists.txt` 会把 `main/web/index.html`、`main/web/i18n.js` 和 `main/web/dither.wasm` gzip 后嵌入固件。

因此修改 Web UI、i18n 或 WASM 后，需要重新 build 固件，才能烧录到设备中。

## 设备端显示流程

设备主循环在 `main/loop_display.c`。

显示一张图片时优先级：

1. 如果有同名 `.bin` 预处理 framebuffer，直接读取并发送到 EPD。
2. 如果没有 `.bin`，回退到设备端 decode / resize / adjust / dither。

优先使用 `.bin` 的原因：

- 浏览器/WASM 端更快，内存更宽裕。
- 可以使用 1.5x supersample 和更复杂的预览匹配流程。
- 设备显示时只需要 IO + EPD refresh，更稳定。

## 设备端 fallback pipeline

旧图片或没有 `.bin` 的图片会在 ESP32 上处理：

```text
JPEG decode
  -> resize 到 TARGET_W x TARGET_H
  -> apply_adjust_rgb888
  -> enhance_eink_rgb888
  -> dither_e6_mix_fs
  -> epd refresh
```

这个路径主要用于兼容，不是最高质量路径。最高质量路径是浏览器预处理 `.bin`。

## EPD Driver

`main/epd_4in0e.c` 负责和面板通信。算法输出的 packed framebuffer 已经是面板需要的 4bpp ink code 排列，因此 driver 不需要理解颜色科学，只负责刷新。

## 构建 WASM

命令：

```bash
./web_wasm/build.sh
```

脚本使用 Emscripten，并导出：

```text
_wasm_init
_wasm_set_palette_lab
_wasm_clear_mix_patches
_wasm_set_mix_patch_lab
_wasm_dither
_wasm_dither_15x
_wasm_dither_e6
_wasm_dither_e6_15x
_wasm_flat_fill
_wasm_flat_fill_15x
_wasm_rgb_to_lab
_malloc
_free
```

构建产物：

```text
main/web/dither.wasm
```

Smoke test：

```bash
node web_wasm/smoke_test.mjs
```

## 固件构建

常用命令：

```bash
IDF_PATH=/path/to/esp-idf cmake --build build
```

构建产物：

```text
build/paper_e6.bin
```

## Host Test

Host 工具用于桌面端验证算法，不经过浏览器和 WASM。

构建：

```bash
make -C host_test
```

普通 dither：

```bash
cd host_test
./host_test --raw demo_5_21_from_png.raw
```

Russian flat-fill：

```bash
cd host_test
./host_test --raw --russian demo_5_21_from_png.raw
```

输出：

- `out_preview.bmp`
- `out_indices.bin`

## 内存与性能

### WASM

WASM 使用 `ALLOW_MEMORY_GROWTH=1`，初始 4 MiB，最大 32 MiB。最终 1.5x pipeline 会分配：

- `600 x 900 x 3` RGB input，约 1.62 MiB。
- `400 x 600 x 3` target，约 0.72 MiB。
- packed output，约 120 KiB。
- indices output，预览和 Auto simulation 时约 240 KiB。
- 临时增强、refinement、region map 等 buffer。

### ESP32

Dither 使用 rolling error rows，而不是全图 error buffer：

- `err_cur`
- `err_nxt`
- `err_nxt2`

每行 `w * 3 * sizeof(float)`，`w=400` 时单行约 4.8 KiB，三行约 14.4 KiB。大 buffer 尽量放 PSRAM。

## 常见问题

### 修改 palette 后为什么要重新生成 Lab

`PALETTE_LAB` 是从 `PALETTE_RGB_MEASURED` 初始化的。如果改变实测 RGB，需要重新运行程序或 WASM init。浏览器端 palette 通过 `/api/calib` 推送到 WASM。mixed patch 读数同样需要重新推送，因为 pair residual 是相对当前 `PALETTE_LAB` 计算的。

### 为什么 preview 和最终输出现在更一致

Dithered preview 走同一条 `600 x 900 -> wasm_*_15x -> 400 x 600 indices` 路径。旧版本使用低分辨率 direct preview，采样相位和 dither 纹理会不同；现在只有浏览器显示缩放和环境光造成的感知差异。

### 为什么 E6 Mix 仍保留 Classic

E6 Mix 是默认目标：更好的实体面板观感。Classic 保留旧行为用于回归对比、用户偏好和诊断。

### 为什么提交 `.wasm`

`main/web/dither.wasm` 是固件嵌入资源的一部分。设备运行时没有 Emscripten，所以需要提交或随发布包提供构建好的 wasm。

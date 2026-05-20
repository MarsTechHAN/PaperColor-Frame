# PaperColor Frame 实现说明

本文档描述项目中浏览器、WASM、ESP32 固件、文件存储和电子纸刷新之间的具体实现关系。算法原理见 [`pipeline.md`](pipeline.md)。

## 代码地图

| 路径 | 作用 |
| --- | --- |
| `main/web/index.html` | 设备内置 Web UI，包含上传、裁剪、预览、preset、WASM 调用和上传逻辑 |
| `main/color_pipeline.c` / `.h` | sRGB/Lab/CIEDE2000、基础调色、EINK-aware enhancement |
| `main/palette.c` / `.h` | 六色墨水编码、实测 RGB、实测 Lab 初始化 |
| `main/dither.c` / `.h` | 默认 dithering pipeline、1.5x downsample、Russian flat-fill pipeline |
| `web_wasm/wasm_entry.c` | C 算法暴露给浏览器 WASM 的 ABI shim |
| `web_wasm/build.sh` | 使用 Emscripten 构建 `main/web/dither.wasm` |
| `main/http_server.c` | 静态页面、WASM、照片 API、校准 API |
| `main/photo_store.c` / `.h` | 图片和预处理 `.bin` 文件存储 |
| `main/loop_display.c` | 设备端播放循环，优先读取预处理 framebuffer |
| `main/epd_4in0e.c` / `.h` | 4.0 寸 6 色电子纸驱动 |
| `host_test/` | 桌面端算法验证和 BMP 输出 |

## 浏览器端两阶段流程

Web UI 是两阶段上传流程。

### Stage 1: Crop / Fit / Rotate

用户先完成构图：

- 选择图片。
- fit / fill。
- 拖拽位置。
- 旋转。
- 确认裁剪区域。

核心状态在 `main/web/index.html`：

- `xform.src`：源图 bitmap。
- `xform.tx` / `xform.ty`：平移。
- `xform.s`：缩放。
- `xform.rot`：旋转。
- `cropGeom`：进入 Stage 2 时冻结的裁剪几何。

进入 Stage 2 时调用 `freezeCropGeom()`，后续 adjust 不再改变图像几何，避免调色阶段出现形变或和最终输出不一致。

### Geometry Normalization

`normalizedCropGeom()` 用于保证 Stage 2 preview 和最终 bake 使用同一套几何。

它做两件事：

1. 重新约束 frozen crop 仍然 cover 面板区域，避免出现白边/黑边。
2. 加 `1.001` overscan，抵消浏览器 canvas 子像素采样导致的边缘漏白。

`bakePreviewCanvas()` 和 `bakeCanvas()` 都使用它，所以 Stage 2 预览和最终上传应该保持一致。

### Stage 2: Adjust / Preset / Preview

Stage 2 显示 dithered preview，用户可以：

- 选择 preset。
- 使用 Auto。
- 展开 Advanced 调整参数。
- 保存自定义 preset。
- 拖拽 splitter 调整 preview 和 control 区域占比。

默认 `previewMode = 'dithered'`，用户点击 preview canvas 可切换 source / dithered。

## Preset 与参数状态

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

- `brightness / contrast / saturation / gamma / temperature / tint / smoothness` 会写入 WASM 的 `adjust_cfg_t`。
- `exposure / vibrance / sharpen / vignette` 是 JS-side extra adjust，不进入 C ABI。

`outputMode` 保存当前输出 pipeline：

- `'dither'`
- `'flat-russian'`

自定义 preset 会保存 `values` 和 `mode` 到 `localStorage`。

## Auto Adjust

`applyAutoAdjust()` 会：

1. 使用 `bakePreviewCanvas()` 得到当前裁剪图。
2. 用 `analyzeCanvasForAuto()` 在低分辨率上统计亮度、对比、边缘、chroma、warm/cool/green/blue、暗角等指标。
3. 用 `chooseAutoPreset()` 选择内置 preset。
4. 用 `buildAutoAdjust()` 在 preset 基础上微调 exposure、contrast、gamma、vignette、sharpen、vibrance。
5. 设置 `outputMode = 'dither'`。

Auto 不会自动选择 Russian，因为 Russian 是风格化非照片 pipeline。

## WASM 加载流程

浏览器通过 `loadWasm()` 加载 `/dither.wasm`：

```js
const resp = await fetch('/dither.wasm', {cache: 'no-store'});
const mod = await WebAssembly.instantiate(bytes, imports);
```

`cache: 'no-store'` 很重要，因为设备端可能更新了 WASM 导出，如果浏览器缓存旧版本，会出现缺少函数的问题。

加载后封装导出：

```js
wasm = {
  exports,
  memory: exports.memory,
  malloc: exports.malloc,
  free: exports.free,
  init: exports.wasm_init,
  setPalette: exports.wasm_set_palette_lab,
  dither: exports.wasm_dither,
  dither15x: exports.wasm_dither_15x,
  flatFill: exports.wasm_flat_fill,
  flatFill15x: exports.wasm_flat_fill_15x,
};
```

随后调用 `wasm.init()`，它会初始化 color pipeline LUT 和 palette Lab。

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

### Palette Calibration

```c
EXPORT void wasm_set_palette_lab(const float *lab18)
```

浏览器从 `/api/calib` 读取实测 palette RGB，再调用 WASM 的 `wasm_rgb_to_lab()` 转换成 Lab，最后通过 `wasm_set_palette_lab()` 写回 `PALETTE_LAB`。

这样做避免 JS 和 C 使用不同 Lab 实现导致漂移。

### RGB to Lab Helper

```c
EXPORT void wasm_rgb_to_lab(uint8_t r, uint8_t g, uint8_t b, float *out_lab3)
```

用于浏览器侧校准 palette Lab，不用于批量处理图片。

### Default Preview Pipeline

```c
EXPORT int wasm_dither(
    uint8_t *rgb,
    int w,
    int h,
    const adjust_cfg_t *cfg,
    uint8_t *packed,
    uint8_t *indices_opt)
```

流程：

```text
apply_adjust_rgb888(rgb, w, h, cfg)
enhance_eink_rgb888(rgb, w, h)
dither_ved_fs(rgb, w, h, packed, indices_opt, smoothness)
```

`rgb` 会被 in-place 修改，所以 JS 在释放前会复制 adjusted buffer 用于 source preview。

### Default Final Pipeline

```c
EXPORT int wasm_dither_15x(
    uint8_t *rgb_15x,
    int panel_w,
    int panel_h,
    const adjust_cfg_t *cfg,
    uint8_t *packed,
    uint8_t *indices_opt)
```

流程：

```text
src_w = panel_w * 3 / 2
src_h = panel_h * 3 / 2
apply_adjust_rgb888(rgb_15x, src_w, src_h, cfg)
enhance_eink_rgb888(rgb_15x, src_w, src_h)
dither_ved_fs_15x(rgb_15x, src_w, src_h, panel_w, panel_h, packed, indices_opt, smoothness)
```

`dither_ved_fs_15x()` 内部会 Catmull-Rom downsample 到 panel grid，再运行默认 dither。

### Russian Preview Pipeline

```c
EXPORT int wasm_flat_fill(
    uint8_t *rgb,
    int w,
    int h,
    const adjust_cfg_t *cfg,
    uint8_t *packed,
    uint8_t *indices_opt)
```

流程：

```text
apply_adjust_rgb888(rgb, w, h, cfg)
enhance_eink_rgb888(rgb, w, h)
flat_fill_constructivist(rgb, w, h, packed, indices_opt)
```

### Russian Final Pipeline

```c
EXPORT int wasm_flat_fill_15x(
    uint8_t *rgb_15x,
    int panel_w,
    int panel_h,
    const adjust_cfg_t *cfg,
    uint8_t *packed,
    uint8_t *indices_opt)
```

流程：

```text
apply_adjust_rgb888(rgb_15x, src_w, src_h, cfg)
enhance_eink_rgb888(rgb_15x, src_w, src_h)
flat_fill_constructivist_15x(rgb_15x, src_w, src_h, panel_w, panel_h, packed, indices_opt)
```

## Browser Preview Implementation

`runPreviewUpdate()` 做交互预览。

流程：

1. `bakePreviewCanvas()` 生成 `200 x 300` 裁剪图。
2. 从 canvas RGBA 提取 RGB888。
3. 调用 `applyExtraAdjustRgb888()`。
4. 分配 WASM 内存：
   - RGB buffer。
   - `adjust_cfg_t`。
   - packed buffer。
   - indices buffer。
5. 根据 `outputMode` 选择：
   - `wasm.dither()`
   - `wasm.flatFill()`
6. 读取 in-place adjusted RGB，生成 source preview。
7. 读取 indices，按实测 palette RGB 生成 dithered / flat preview。
8. 调用 `applyPanelPreviewOptics()` 让预览更接近纸面。
9. 释放 WASM 内存。

Preview 使用 `200 x 300` 是为了交互速度。最终 Send 使用 `600 x 900 -> 400 x 600` 的 1.5x pipeline。

## Final Upload Implementation

用户点击 Send 后，`ditherCanvas()` 处理最终 framebuffer。

流程：

1. `bakeCanvas()` 生成 `SW x SH`，当前为 `600 x 900`。
2. 从 RGBA canvas 提取 RGB888。
3. JS 侧应用 exposure / vibrance / vignette / sharpen。
4. 写入 WASM heap。
5. 写入 7 个 int32 的 `adjust_cfg_t`。
6. 分配 `TW * TH / 2` packed buffer，当前 `400 * 600 / 2 = 120000` bytes。
7. 根据 `outputMode` 调用：
   - `wasm.dither15x(...)`
   - `wasm.flatFill15x(...)`
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

打包逻辑在 `pack_nibble()` 和 `repack_indices()`。

## 上传文件

Send 成功时通常产生三个文件：

- `p_<timestamp>.jpg`：原始/裁剪后的展示图，用于 gallery 和未来 Tune。
- `p_<timestamp>.bin`：已经预处理好的 4bpp framebuffer。
- `p_<timestamp>.json`：sidecar，保存 adjust 参数和 pipeline mode。

sidecar 示例：

```json
{
  "adjust": {
    "brightness": 0,
    "exposure": 0,
    "contrast": 106,
    "saturation": 106,
    "vibrance": 24,
    "gamma": 96,
    "temperature": 106,
    "tint": 99,
    "smoothness": 10,
    "sharpen": 34,
    "vignette": 14
  },
  "mode": "dither"
}
```

Tune 时如果找到 sidecar，会恢复参数和 mode。

## HTTP Server 与嵌入资源

`main/http_server.c` 提供：

- `GET /`：Web UI。
- `GET /dither.wasm`：WASM 模块。
- `GET /api/calib`：palette 和默认 adjust。
- 图片列表、上传、删除、重命名等 API。

`main/CMakeLists.txt` 会把 `main/web/index.html` 和 `main/web/dither.wasm` gzip 后嵌入固件。

因此修改 Web UI 或 WASM 后，需要重新 build 固件，才能烧录到设备中。

## 设备端显示流程

设备主循环在 `main/loop_display.c`。

显示一张图片时优先级：

1. 如果有同名 `.bin` 预处理 framebuffer，直接读取并发送到 EPD。
2. 如果没有 `.bin`，回退到设备端 decode / resize / adjust / dither。

优先使用 `.bin` 的原因：

- 浏览器/WASM 端通常更快，内存更宽裕。
- 可以使用 1.5x supersample 和更复杂的算法。
- 设备显示时只需要 IO + EPD refresh，更稳定。

## 设备端 fallback pipeline

旧图片或没有 sidecar/bin 的图片会在 ESP32 上处理：

```text
JPEG decode
  -> resize 到 TARGET_W x TARGET_H
  -> apply_adjust_rgb888
  -> enhance_eink_rgb888
  -> dither_ved_fs
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
_wasm_dither
_wasm_dither_15x
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

当前 demo 的 Russian 输出曾保存为：

- `host_test/demo_5_21_russian_flat.bmp`
- `host_test/demo_5_21_russian_indices.bin`

## 内存与性能

### WASM

WASM 使用 `ALLOW_MEMORY_GROWTH=1`，初始 4 MiB，最大 32 MiB。最终 1.5x pipeline 会分配：

- `600 x 900 x 3` RGB input，约 1.62 MiB。
- `400 x 600 x 3` target，约 0.72 MiB。
- packed output，约 120 KiB。
- 临时增强、refinement、indices 等 buffer。

### ESP32

Dither 使用 rolling error rows，而不是全图 error buffer：

- `err_cur`
- `err_nxt`
- `err_nxt2`

每行 `w * 3 * sizeof(float)`，`w=400` 时单行约 4.8 KiB，三行约 14.4 KiB。大 buffer 尽量放 PSRAM。

## 常见问题

### 修改 palette 后为什么要重新生成 Lab

`PALETTE_LAB` 是从 `PALETTE_RGB_MEASURED` 初始化的。如果改变实测 RGB，需要重新运行程序或 WASM init。浏览器端 palette 也通过 `/api/calib` 推送到 WASM。

### 为什么 preview 和最终输出可能略不同

Preview 是 `200 x 300`，最终是 `600 x 900 -> 400 x 600`。几何一致，但采样和 dithering 分辨率不同，所以纹理可能略有差异。

### 为什么 Russian 的照片层次更少

因为 Russian 明确不做 dithering，不通过空间混色表达连续 tone。它是风格化纯色块输出，不是默认照片还原算法。

### 为什么提交 `.wasm`

`main/web/dither.wasm` 是固件嵌入资源的一部分。设备运行时没有 Emscripten，所以需要提交或随发布包提供构建好的 wasm。

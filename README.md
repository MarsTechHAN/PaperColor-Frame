# PaperColor Frame

PaperColor Frame is open-source firmware for a small ESP32-S3 photo frame with a 6-color E Ink / Spectra-style panel. It creates its own Wi-Fi hotspot, serves a mobile-first web app, and lets you upload, crop, tune, calibrate, and display photos without a cloud account or companion app.

The project is built for the M5Stack PaperColor-style hardware family: ESP32-S3, SD storage, PMIC-controlled display power, buttons, status LEDs, and a 4-inch 6-color electrophoretic display.

## What it does

- **Phone-friendly web UI**: connect to the frame Wi-Fi, open the captive portal, and manage photos from a browser.
- **Polaroid-style photo wall**: upload photos, browse them as instant-print cards, and send any photo to the panel.
- **Crop and tune before sending**: rotate, fit, crop, preview dithering, apply presets, run Auto, and fine-tune image controls.
- **Panel-aware dithering**: browser-side WASM converts images into a packed 4bpp framebuffer for the 6-color panel.
- **E6 Mix mode**: an improved dither path tuned for real reflective paper, with Classic VED still available.
- **Display calibration**: enter RGB or CMYK colorimeter readings for solid inks and optional mixed patches.
- **Offline storage**: photos are stored on SD card first, with SPIFFS fallback when SD is unavailable.
- **Device settings**: adjust slideshow timing, sleep behavior, status light brightness, Wi-Fi password, language, and image processing defaults.
- **Low-power behavior**: display and SD rails are managed by the PMIC; the frame can sleep and wake by timer or button.

## Repository layout

```text
.
├── CMakeLists.txt          # ESP-IDF project entry
├── LICENSE                 # GPL-3.0-only license text
├── README.md               # Project overview and quick start
├── doc/                    # Design, image pipeline, and implementation docs
│   ├── implementation.md   # Web/WASM/device flow
│   ├── pipeline.md         # Color science and dithering pipeline
│   └── style-guide.md      # Mobile UI, copy, and i18n rules
├── main/                   # Firmware source, drivers, config, and embedded web app
│   ├── web/                # index.html, i18n.js, and generated dither.wasm
│   ├── color_pipeline.*    # Color transforms, Lab/CIEDE2000, image adjustment
│   ├── dither.*            # Classic and E6 Mix dithering
│   ├── epd_4in0e.*         # 4-inch 6-color panel driver
│   ├── http_server.*       # Captive portal, photo API, config API, calibration API
│   ├── loop_display.*      # Serialized display worker and playback loop
│   ├── power_manager.*     # Buttons, sleep policy, status LED, PMIC coordination
│   └── wifi_ap.*           # SoftAP setup and station tracking
├── web_wasm/               # Emscripten build for the browser-side dither module
├── host_test/              # Desktop-side algorithm experiments and validation helpers
├── partitions.csv          # ESP32 partition table
└── sdkconfig.defaults      # Default ESP-IDF settings
```

## Documentation

- [`doc/style-guide.md`](doc/style-guide.md): product naming, mobile UI rules, i18n rules, and copywriting guidance.
- [`doc/pipeline.md`](doc/pipeline.md): how RGB input becomes a 6-color E Ink framebuffer.
- [`doc/implementation.md`](doc/implementation.md): how the web UI, WASM module, storage, APIs, and display worker fit together.

## Requirements

- ESP-IDF 5.x with ESP32-S3 support.
- Python and the ESP-IDF toolchain installed and exported in your shell.
- Emscripten for rebuilding `main/web/dither.wasm`.
  - The current helper script expects Homebrew Emscripten at `/opt/homebrew/Cellar/emscripten/5.0.7`.
- Node.js for the WASM smoke test.

## Build

From a shell with ESP-IDF available:

```bash
. "$HOME/esp/esp-idf/export.sh"
./web_wasm/build.sh
idf.py set-target esp32s3
idf.py build
```

The firmware build gzips and embeds the web app, translation bundle, and WASM module through `main/CMakeLists.txt`.

## Flash

Replace the port with your board's serial device:

```bash
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

After boot, connect your phone or computer to the frame's Wi-Fi. The browser should open the captive portal automatically; if not, visit the AP gateway address shown in the serial log.

## WebAssembly workflow

Rebuild the browser-side dither module and run a smoke test:

```bash
./web_wasm/build.sh
node web_wasm/smoke_test.mjs
```

`main/web/dither.wasm` is generated output and is intentionally ignored by Git.

## Development checks

Useful checks before committing UI or image pipeline work:

```bash
node --check main/web/i18n.js
node web_wasm/smoke_test.mjs
idf.py build
```

For UI text changes, keep user-facing strings in `main/web/i18n.js` and update both English and Chinese entries.

## Git hygiene

Do commit:

- firmware source
- web source (`index.html`, `i18n.js`)
- default configuration
- documentation

Do not commit:

- `build/`
- `dist/`
- `sdkconfig`
- generated `main/web/dither.wasm`
- local caches or object files

## License

PaperColor Frame is licensed under GPL-3.0-only. See [`LICENSE`](LICENSE).

---

# PaperColor Frame（中文）

PaperColor Frame 是一个开源的 ESP32-S3 六色电子纸相框固件。设备会自己创建 Wi-Fi 热点并提供手机端 Web 页面，你可以直接在浏览器里上传、裁剪、调色、校准并显示照片，不需要云服务，也不需要单独安装 App。

这个项目面向 M5Stack PaperColor 类硬件：ESP32-S3、SD 卡、PMIC 控制的屏幕电源、实体按键、状态灯，以及 4 英寸 6 色电子纸屏。

## 它能做什么

- **手机端 Web UI**：连接相框 Wi-Fi 后，通过浏览器管理照片和设置。
- **拍立得风格照片墙**：上传后的照片以相纸卡片展示，可以立即发送到屏幕。
- **上传前裁剪和调色**：支持旋转、适配、裁剪、抖色预览、预设、自动调色和手动参数。
- **面板感知抖色**：浏览器端 WASM 会把图片转换成 6 色屏可直接显示的 4bpp framebuffer。
- **E6 Mix 模式**：针对真实反射式墨水屏优化的抖色路径，同时保留 Classic VED。
- **屏幕校准**：可以录入纯色墨水和可选混色色块的 RGB / CMYK 色度计读数。
- **离线存储**：优先使用 SD 卡保存照片，SD 不可用时回退到内部 SPIFFS。
- **设备设置**：可调整轮播间隔、休眠策略、状态灯亮度、Wi-Fi 密码、语言和图像处理默认值。
- **低功耗逻辑**：通过 PMIC 管理屏幕和 SD 电源，支持定时或按键唤醒。

## 目录结构

```text
.
├── CMakeLists.txt          # ESP-IDF 工程入口
├── LICENSE                 # GPL-3.0-only 许可证文本
├── README.md               # 项目说明和快速开始
├── doc/                    # 设计、图像 pipeline、实现文档
│   ├── implementation.md   # Web/WASM/设备端调用链
│   ├── pipeline.md         # 颜色科学和抖色算法说明
│   └── style-guide.md      # 手机端 UI、文案和 i18n 规范
├── main/                   # 固件源码、驱动、配置和内置 Web App
│   ├── web/                # index.html、i18n.js、生成的 dither.wasm
│   ├── color_pipeline.*    # 颜色转换、Lab/CIEDE2000、图像调色
│   ├── dither.*            # Classic 和 E6 Mix 抖色
│   ├── epd_4in0e.*         # 4 英寸 6 色屏驱动
│   ├── http_server.*       # Captive portal、照片 API、设置 API、校准 API
│   ├── loop_display.*      # 串行显示任务和轮播逻辑
│   ├── power_manager.*     # 按键、休眠、状态灯、PMIC 协调
│   └── wifi_ap.*           # SoftAP 和连接状态
├── web_wasm/               # 浏览器端抖色模块的 Emscripten 构建
├── host_test/              # 桌面端算法验证和实验工具
├── partitions.csv          # ESP32 分区表
└── sdkconfig.defaults      # 默认 ESP-IDF 配置
```

## 文档

- [`doc/style-guide.md`](doc/style-guide.md)：产品命名、手机端 UI、i18n 和文案规则。
- [`doc/pipeline.md`](doc/pipeline.md)：RGB 图片如何变成 6 色电子纸 framebuffer。
- [`doc/implementation.md`](doc/implementation.md)：Web UI、WASM、存储、API 和显示任务如何协作。

## 环境要求

- ESP-IDF 5.x，并支持 ESP32-S3。
- 已安装并导出 ESP-IDF 所需的 Python 和工具链环境。
- 如需重新构建 `main/web/dither.wasm`，需要 Emscripten。
  - 当前脚本默认使用 Homebrew 路径 `/opt/homebrew/Cellar/emscripten/5.0.7`。
- 运行 WASM smoke test 需要 Node.js。

## 构建

进入 ESP-IDF 环境后执行：

```bash
. "$HOME/esp/esp-idf/export.sh"
./web_wasm/build.sh
idf.py set-target esp32s3
idf.py build
```

固件构建时，`main/CMakeLists.txt` 会把 Web 页面、翻译文件和 WASM 模块 gzip 后嵌入固件。

## 烧录

把串口替换成你的开发板端口：

```bash
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

启动后，连接相框创建的 Wi-Fi。浏览器通常会自动弹出 captive portal；如果没有，访问串口日志中显示的 AP 网关地址。

## WebAssembly 流程

重新构建浏览器端抖色模块并运行 smoke test：

```bash
./web_wasm/build.sh
node web_wasm/smoke_test.mjs
```

`main/web/dither.wasm` 是生成产物，不提交到 Git。

## 开发检查

改 UI 或图像 pipeline 后，建议至少运行：

```bash
node --check main/web/i18n.js
node web_wasm/smoke_test.mjs
idf.py build
```

如果改了用户可见文案，请把文案放在 `main/web/i18n.js`，并同时补齐英文和中文。

## Git 约定

应该提交：

- 固件源码
- Web 源码（`index.html`、`i18n.js`）
- 默认配置
- 文档

不要提交：

- `build/`
- `dist/`
- `sdkconfig`
- 生成的 `main/web/dither.wasm`
- 本地缓存或对象文件

## License

PaperColor Frame 使用 GPL-3.0-only 许可证。详见 [`LICENSE`](LICENSE)。

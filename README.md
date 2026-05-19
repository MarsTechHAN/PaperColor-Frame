# PaperColor Paintings

PaperColor Paintings 是一个面向 ESP32-S3 与 6 色电子纸屏的照片显示固件。项目会在设备上启动 Wi-Fi AP 与本地 Web 管理页，用户可以通过手机或电脑连接热点上传图片，固件负责图片解码、缩放、调色板映射、抖动处理与电子纸刷新。

## 功能特性

- 基于 ESP-IDF 的 ESP32-S3 固件工程。
- 支持 6 色电子纸调色板与设备端图片处理流水线。
- 内置 Web UI，可上传、管理和预览图片。
- SoftAP + DNS 劫持，便于作为离线相框/展示设备使用。
- 优先使用 SD 卡保存照片，SD 不可用时可回退到内部 SPIFFS。
- 提供浏览器端 WASM 抖动模块，便于上传前预处理和预览。
- 提供 host_test 本地验证工具，用于对照调色和抖动算法输出。

## 目录结构

```text
.
├── CMakeLists.txt          # ESP-IDF 顶层工程入口
├── main/                   # 固件源码、驱动、Web 资源
│   ├── web/                # 设备内置 Web UI 与 dither.wasm
│   ├── color_pipeline.*    # 颜色空间与调色处理
│   ├── dither.*            # 抖动算法
│   ├── epd_4in0e.*         # 电子纸驱动
│   ├── http_server.*       # 本地 Web 服务
│   └── wifi_ap.*           # SoftAP 初始化
├── web_wasm/               # WASM 抖动模块构建与 smoke test
├── host_test/              # 桌面端算法验证工具与参考脚本
├── dist/                   # 可选发布固件二进制
├── partitions.csv          # 分区表
├── sdkconfig.defaults      # 默认配置
└── pipeline.md             # 图片处理流水线记录
```

## 环境要求

- ESP-IDF 5.x（工程依赖 ESP32-S3 目标）。
- Python 与 ESP-IDF 工具链已正确安装并导出环境变量。
- 如需重新构建 `main/web/dither.wasm`，需要本机安装 Emscripten；当前脚本默认使用 Homebrew 路径 `/opt/homebrew/Cellar/emscripten/5.0.7`。
- 如需运行 host 侧测试，建议安装 `clang` 与 `make`。

## 构建固件

先进入 ESP-IDF 环境，例如：

```bash
. "$HOME/esp/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
```

构建产物会生成在 `build/`，该目录不纳入 Git。`main/CMakeLists.txt` 会在固件构建时将 `main/web/index.html` 与 `main/web/dither.wasm` gzip 后嵌入固件。

## 烧录与监控

请按实际串口修改 `PORT`：

```bash
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

启动完成后，设备会创建开放热点。连接热点后访问设备入口页面即可上传和管理照片。

## 构建 WebAssembly 抖动模块

```bash
./web_wasm/build.sh
node web_wasm/smoke_test.mjs
```

`build.sh` 会生成 `main/web/dither.wasm`，该文件会随固件一起嵌入。

## 本地算法验证

```bash
cd host_test
make
./host_test --raw demo_5_21.raw
```

生成的预览图、索引文件、对象文件和可执行文件属于本地测试产物，已通过 `.gitignore` 排除。

## Git 与子模块说明

当前工程没有必须以 Git submodule 管理的第三方源码，因此未添加 `.gitmodules`。ESP-IDF、Emscripten 等工具链依赖建议由开发环境独立安装和维护，不直接提交到本仓库。

## 维护约定

- 源码、配置、Web 资源和必要的发布二进制可以提交。
- `build/`、本地缓存、对象文件和测试生成图不提交。
- 修改调色或抖动算法后，建议同时运行 WASM smoke test 与 host 侧对照测试。

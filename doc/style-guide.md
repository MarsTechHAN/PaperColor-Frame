# Paper E6 Web UI Style Guide

本文档记录 Paper E6 内置 Web UI 的视觉、交互和文案规则。目标是让手机端界面保持一致：像一面可触摸的拍立得照片墙，同时足够清楚、可操作、可本地化。

## 设计目标

- **手机优先**：主要适配 iPhone 尺寸和单手操作，所有关键按钮必须在窄屏上保持单行可读。
- **拍立得气质**：照片、预览、色块都应有相纸边框、轻微阴影、纸张纹理和温暖底色。
- **真实设备语言**：按钮和提示要说明用户下一步做什么，不写调试口吻或含糊帮助语。
- **快速可用**：页面打开后先显示照片列表，重任务如 WASM warm-up 不阻塞首屏。
- **一致系统**：图标、按钮、卡片、设置项、sheet 底栏使用同一套尺寸、圆角、阴影和字重。

## 视觉语言

### 色彩

核心颜色定义在 `main/web/index.html` 的 `:root`。

- 背景使用暖纸色和轻微渐变，不使用纯白或纯黑底。
- 主要文字使用深棕黑 `--ink`，辅助文字使用 `--ink-3`。
- 强操作使用深棕渐变按钮，不使用随机高饱和色。
- 点缀色只用于状态或进度：red/yellow/green/blue 四色来自相纸和墨水屏语义。

### 排版

- 标题优先使用 condensed / display feel，如 `Avenir Next Condensed`、`DIN Condensed`。
- 正文和控件使用同一套无衬线栈，保持粗体、短行、易扫读。
- 不允许标题被压成省略号。标题容器必须允许换行或自适应宽度。
- 按钮文字必须单行；如放不下，优先改短文案或降低字距，而不是允许换行。

### 形状与质感

- 卡片圆角以 18-30px 为主；拍立得照片可使用轻微不规则倾斜。
- 照片卡片保留大底边，像拍立得相纸。
- 使用柔和阴影和内高光 `--soft-inset`，避免硬边框堆叠。
- 页面背景要有纸张纹理或渐变气氛，不使用单色平铺。

## Layout Rules

### App Shell

- `body` 和 `.app-shell` 使用 `100dvh`，底部 dock 必须贴住 safe area。
- 主照片区底部 padding 至少覆盖 dock 高度，避免最后一行被遮挡。
- fixed bottom dock 只放高频操作：Next 和 Upload。

### Sheets

- 上传、详情、设置、校准都使用全屏 sheet。
- sheet header 固定在顶部，footer 固定在底部，中间 `.sheet__body` 可滚动。
- footer 按钮使用等宽布局，文字单行。
- 需要长内容的页面（Tune / Adjust、Settings、Calibration）必须允许上下滚动。

### Cards

设置项和校准项都用 card 承载：

- 左侧是 label + hint。
- 右侧是当前值或操作状态。
- range/select 控件放在文字区下方，和当前值同卡。
- 不用裸露表单行，也不要把相关控件拆到多个视觉层级。

## Icon Rules

- 所有 UI 图标使用 inline SVG，统一 class 为 `.ui-icon`。
- 图标必须是 stroke 风格，使用 `currentColor`，统一 round cap/join。
- 不混用 emoji、位图图标、filled icon 和线性 icon。
- 图标只辅助理解，不替代文字；关键按钮必须有文字或 aria label。

## Interaction Rules

### Gallery

- 照片卡只显示图片，不显示文件名和文件大小。
- 详情页标题使用通用文案（如 `Photo` / `照片`），不显示文件名。
- 空状态文案要直接说明“上传照片开始”，不要写 `Tap...` 这种弱提示。

### Upload / Tune

- 上传按钮必须在脚本早期绑定，避免后续模块出错导致无法点上传。
- Crop 阶段只负责构图；进入 Adjust 时冻结 crop geometry。
- Adjust 阶段默认展开高级参数，用户可向下滚动调整。
- slider 只允许拖动圆形把手改变值，触碰轨道不应跳变。
- 预览 canvas 可切换 source / dithered，但文案不要依赖 `tap` 指令。

### Settings

设置页从上到下保持这个顺序：

1. Playback：自动刷新间隔。
2. Power：手机断开后休眠、按键刷新后休眠。
3. Frame hardware：状态灯、屏幕色彩校准入口。
4. Image processing：抖色模式、自动亮度补偿、自动色温补偿。
5. Library：设备照片数、固件版本。
6. Language：语言切换。

规则：

- 每个设置项必须显示当前值。
- 修改 range/select 后立即保存；保存失败用 toast 提示。
- 固件项显示“固件版本”，内容为 git commit hash；下方显示编译时间。
- 语言默认跟随浏览器，手动切换写入 `localStorage`。

### Calibration

校准页是色度计工作流，不是“看着拖滑块”的工作流：

- 不显示无用帮助句，例如 `Tap...`。
- 当前色块用 card 显示：色块名称 + 必测/可选状态。
- “发送到相框”是主动作，按钮文字必须单行。
- 用户输入 RGB 或 CMYK 读数，保存后进入预览和抖色模型。
- 底部提示必须说人话：当前使用什么值、是否已保存、是否可跳过。
- 可选混色色块可以跳过；纯色色块是必测。

## i18n Rules

- 翻译集中放在 `main/web/i18n.js`，不要把新增用户可见文案硬编码在 `index.html` 的 JS 逻辑里。
- 支持 `en` 和 `zh`，默认英文；根据 `navigator.languages` 自动选择中文或英文。
- 手动语言切换保存到 `localStorage`，并通过 `paper-i18n-change` 刷新动态文案。
- HTML 静态文案使用 `data-i18n` / `data-i18n-title` / `data-i18n-placeholder` / `data-i18n-aria`。
- 动态文案使用 `tr(key, vars)` 或 `toastKey(key, vars)`。
- 新增 key 必须同时补英文和中文。

## Copy Rules

- 优先使用短动词 + 明确对象：`Show on frame`、`Save reading`、`Keep estimate`。
- 中文使用口语但准确的设备词：`发送到相框`、`保存读数`、`保留估算`。
- 避免工程词直接外露，除非是用户需要理解的设置项。
- 避免 `Tap...`、`Click...`、`Next ->` 这类机械提示。
- Toast 只反馈结果或失败原因，不解释长流程。

## Implementation Checklist

改 UI 前后至少检查：

- `node --check main/web/i18n.js`
- 提取并检查 `main/web/index.html` 内联 JS 语法。
- 检查所有 `data-i18n*`、`tr()`、`toastKey()` 静态 key 在英文和中文都存在。
- 运行 `IDF_PATH=/Users/bytedance/esp/esp-idf cmake --build build`。
- 若改了 WASM 导出，运行 `node web_wasm/smoke_test.mjs`。

## Source Of Truth

- UI / CSS / interaction: `main/web/index.html`
- i18n dictionary: `main/web/i18n.js`
- Web asset embedding: `main/CMakeLists.txt`
- Web routes and config API: `main/http_server.c`
- Persistent settings defaults: `main/config_store.c` / `main/config_store.h`

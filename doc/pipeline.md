# PaperColor Frame 图像处理 Pipeline

本文档解释 RGB 图片到 6 色 EINK Spectra / PaperColor E6 面板的整体处理流程、颜色科学假设、数学目标函数，以及当前三类输出 pipeline：默认 E6 Mix 照片 dithering、Classic VED dithering 和 Russian 纯色块 pipeline。

## 目标

本项目的目标不是把输入图片转换成“理想 sRGB 六色图”，而是在真实反射式电子纸上，让人眼看到的结果尽量接近原图的主要观感。所有优化都服务于实体 E6 面板最终显示效果。

约束：

- EINK 是反射式显示，不是发光显示；白色、黑色和彩色墨水的亮度范围都远小于手机屏幕。
- 6 色面板只有 black、white、yellow、red、blue、green 六种实际墨水，连续色调必须通过空间混合或纯色块取舍表达。
- 人眼对不同颜色噪声的敏感程度不同：白底上的红/蓝/黑点非常显眼，黄点相对不明显。
- 面板实测颜色和标称颜色差异很大，必须使用实测色来做匹配。
- 预览要尽量匹配最终发送到面板的 4bpp framebuffer，而不是只让手机屏幕上看起来好。

## 实测面板色彩

当前调色板来自色差仪测量结果，代码中保存在 `main/palette.c`，默认 calibration JSON 也使用同一组 RGB：

| 墨水 | 实测 RGB |
| --- | --- |
| BLACK | `[70, 71, 80]` |
| WHITE | `[151, 161, 160]` |
| YELLOW | `[163, 154, 69]` |
| RED | `[118, 69, 70]` |
| BLUE | `[60, 97, 134]` |
| GREEN | `[77, 100, 76]` |

这些值是面板在用户测试光源和 PANTONE-LS C2019 条件下的外观近似。启动时通过 `palette_init()` 转换到 CIELAB，保存在 `PALETTE_LAB` 中；浏览器校准后会通过 `wasm_set_palette_lab()` 覆盖 WASM 内的 Lab palette。

## 为什么不用理想 RGB 匹配

如果直接把输入 sRGB 和理想六色，例如 `[255,0,0]`、`[0,255,0]` 比较，会产生两个问题：

1. 面板红色实测是暗、低饱和的 `[118,69,70]`，不是屏幕上的纯红。
2. 面板白色只有约 `[151,161,160]`，黑色也只有约 `[70,71,80]`，动态范围非常小。

所以输入图片必须先被压缩到“纸面可表达的外观空间”，再和实测墨水比较。

## 颜色空间与距离

### sRGB 到线性 RGB

`main/color_pipeline.c` 维护一个 256 项 LUT，把 8-bit sRGB 反伽马到线性光：

```text
c_linear = c_srgb / 12.92,                         c_srgb <= 0.04045
c_linear = ((c_srgb + 0.055) / 1.055) ^ 2.4,       otherwise
```

### 线性 RGB 到 XYZ

使用标准 D65 sRGB 矩阵：

```text
X = 0.4124564 R + 0.3575761 G + 0.1804375 B
Y = 0.2126729 R + 0.7151522 G + 0.0721750 B
Z = 0.0193339 R + 0.1191920 G + 0.9503041 B
```

### XYZ 到 Lab

以 D65 白点归一化，然后使用 CIELAB 非线性：

```text
f(t) = t^(1/3),                 t > (6/29)^3
f(t) = t / (3 * (6/29)^2) + 4/29, otherwise

L* = 116 f(Y/Yn) - 16
a* = 500 [f(X/Xn) - f(Y/Yn)]
b* = 200 [f(Y/Yn) - f(Z/Zn)]
```

### CIEDE2000

候选墨水选择使用 CIEDE2000，而不是 RGB 欧氏距离。CIEDE2000 对 L、C、H 和 hue rotation 做感知修正，更适合在实测面板 Lab 空间中比较候选墨水。

## Source 到 Panel Lab 的压缩

照片 dither 不直接比较输入 Lab 和墨水 Lab，而是先做 `map_source_to_panel_lab()`：

```text
t = L_source / 100
t = clamp(t, 0, 1)
t = t ^ 0.92

L_panel = L_black + (L_white - L_black) * t
neutral_a = a_black + (a_white - a_black) * t
neutral_b = b_black + (b_white - b_black) * t

a_panel = neutral_a + a_source * 0.44
b_panel = neutral_b + b_source * 0.44
```

含义：

- 亮度被压缩到实测 black 到 white 的范围。
- `t ^ 0.92` 轻微抬升暗部，避免阴影全掉进黑色。
- chroma 只保留约 44%，因为六色纸面 gamut 很小。
- 中性轴使用实测黑白的 a/b 漂移，而不是假设纸白为纯中性。

## 默认 E6 Mix 照片 Pipeline

默认 pipeline 面向真实 E6 显示效果，不是单纯追求手机预览上的数学色差。整体流程：

```text
输入图片
  -> Stage 1 crop / fit / rotate
  -> 1.5x supersample canvas, 600x900
  -> JS extra adjust: exposure / vibrance / vignette / sharpen
  -> WASM adjust: brightness / contrast / saturation / gamma / temperature / tint
  -> EINK-aware enhancement
  -> Catmull-Rom downsample 到面板分辨率 400x600
  -> Unsharp mask
  -> Lab gamut mapping
  -> E6 Mix palette pick
  -> edge-aware Stucki error diffusion
  -> non-causal local refinement
  -> packed 4bpp EPD framebuffer
```

### 1.5x Supersample

最终面板为 `400 x 600`，浏览器端 bake 使用 `600 x 900`。WASM 中再用 Catmull-Rom cubic kernel 下采样到面板分辨率。

原因：

- 直接 resize 到 400x600 会丢失细节。
- 1.5x 中间图保留更多局部高频信息。
- Catmull-Rom 有轻微负旁瓣，比 box filter 更能保留边缘对比。
- 最后仍然在 panel grid 上 dither，因为可见墨水点就是 panel pixel。

Catmull-Rom kernel：

```text
w(t) = 1.5|t|^3 - 2.5|t|^2 + 1,                 |t| < 1
w(t) = -0.5|t|^3 + 2.5|t|^2 - 4|t| + 2,          1 <= |t| < 2
w(t) = 0,                                        otherwise
```

### JS Extra Adjust

`applyExtraAdjustRgb888()` 处理：

- exposure：乘法曝光，单位是 EV/100，范围 `-3 EV..+3 EV`。
- vibrance：对低饱和区域提升更大，对高饱和区域提升较小。
- vignette：补偿边角暗角。
- sharpen：用户可控的高频增强。

### WASM Adjust

`adjust_cfg_t` 包含：brightness、contrast、saturation、gamma、temperature、tint、smoothness。

处理顺序在 `apply_adjust_rgb888()`：

```text
rgb = rgb * contrast + brightness
apply temperature on red/blue axis
apply tint on green/magenta axis
HSV saturation scaling
apply gamma LUT
```

当前默认和 preset 相比早期版本整体提高了饱和度和自然饱和度，以补偿 E6 反射式低 chroma 外观。

### EINK-aware Enhancement

`enhance_eink_rgb888()` 是轻量 deterministic enhancement。它做局部 base/detail 分离、clarity/detail boost、轻微 S 曲线和 chroma separation，目的是让六色量化前的局部边缘和颜色分区更清晰。

## Classic VED / Stucki Dither

Classic 是保留的旧照片 dither，入口为：

- `dither_ved_fs()`
- `dither_ved_fs_15x()`
- WASM: `wasm_dither()` / `wasm_dither_15x()`

它使用同样的 gamut mapping、Stucki diffusion、smoothness snap、region bias 和 local refinement，但 palette picker 是较保守的 `pick_palette_spatial()`。

Classic 现在主要用于：

- 和旧版本视觉对比。
- 回退诊断。
- 用户偏好旧纹理时手动选择。

## E6 Mix Palette Picker

E6 Mix 是默认 dither mode，入口为：

- `dither_e6_mix_fs()`
- `dither_e6_mix_fs_15x()`
- WASM: `wasm_dither_e6()` / `wasm_dither_e6_15x()`

它在 Classic 的基础上改 palette pick：

```text
score(p) = CIEDE2000(target_lab, palette_lab[p])
         + same-neighbour penalty
         + E6 visibility / scene bias
         + tiny rotated screen tie-break
```

核心目标是实体面板观感：

- 高亮/低 chroma 区域强惩罚 black、red、blue 点，减少白底脏点。
- warm / skin 区域抑制 blue、green 污染。
- sky / cool 区域抑制 red、yellow、green 污染。
- green-dominant 区域抑制 red 污染，保护绿植。
- 保留 strong colour evidence：bias 只影响 close calls，不把明确颜色强行改掉。

`tiebreak` 使用每个 ink 旋转过的 8x8 小扰动，只在分数很接近时打散同分选择，避免所有颜色共用同一周期。

## Edge-aware Stucki Error Diffusion

照片 dither 使用 Stucki kernel：

```text
            x+1  x+2          8/42  4/42
x-2 x-1  x  x+1  x+2    2/42  4/42  8/42  4/42  2/42
x-2 x-1  x  x+1  x+2    1/42  2/42  4/42  2/42  1/42
```

当前实现根据 source edge strength 调整远距离扩散：

```text
far = 1 - (edge - 14) / 58
far = clamp(far, 0, 1)
norm = 42 / (32 + 10 * far)
```

- 平坦区域：保留第二未来行扩散，让过渡更平滑。
- 强边缘：减少远行扩散，避免颜色跨边界污染。
- `norm` 重新归一化近场权重，避免丢失误差能量。

## Smoothness Snap

`smoothness` 控制近似纯色区域是否停止扩散：

```text
threshold = smoothness * 0.4
hard = threshold * 0.5
```

当当前像素选择的墨水和目标色已经足够接近时，扩散误差会降低甚至归零：

```text
diffuse_factor = 0,                            dE <= hard
diffuse_factor = (dE - hard)/(threshold-hard), hard < dE < threshold
diffuse_factor = 1,                            dE >= threshold
```

这可以避免白色或接近纯墨水的区域因为累计误差突然出现孤立红/蓝/黑点。

## Region Classifier 和可见性 bias

代码使用轻量规则分类：

- `REG_EDGE`
- `REG_FLAT`
- `REG_WARM_SKIN`
- `REG_SKY_COOL`
- `REG_DARK`
- `REG_LIGHT`

它不是语义分割模型，而是 WASM/ESP32 友好的局部启发式。用途是给 palette picker 加小 bias：

- 肤色/暖色区域抑制蓝绿污染。
- 天空/冷色区域抑制黄绿/红污染。
- 高亮区域惩罚黑/红/蓝点。
- 平坦区域增强 anti-cluster。

人眼在浅色纸面上对高对比点很敏感，因此这些 bias 会牺牲一点平均 Lab 精度，换取更干净的实体观感。

## Spatial Anti-cluster

照片 picker 不只看 CIEDE2000，还看已量化邻居：

```text
score = deltaE + same-neighbour penalty + region/E6 bias
```

如果候选色和 left / up / diagonal 邻居相同，会加小惩罚。平坦区域惩罚更强，边缘区域惩罚更弱。

作用：

- 避免同色点局部聚集成斑块。
- 让 halftone 更接近蓝噪声观感。
- 边缘处不过度拆散颜色，保留结构清晰度。

## Local Refinement

Error diffusion 是因果算法，只能看到过去像素。当前实现额外做 2 次非因果 refinement：

- 半径 `REFINE_RADIUS = 2`，即 5x5 邻域。
- 每次尝试把当前像素换成其他墨水。
- 如果 5x5 局部平均 Lab 更接近目标平均，就接受。
- 同时限制单像素损失，避免为了局部平均牺牲局部强颜色。

目标函数近似：

```text
E = 1.20 * mean_delta_L^2 + 0.58 * (mean_delta_a^2 + mean_delta_b^2)
  + cluster_cost * cluster_weight
  + region_bias * 0.12
```

## Mixed-patch Calibration 和 Preview Optics

校准不只支持六个纯墨水，还支持 mixed halftone patch。目标是让预览和 Auto scoring 更接近真实纸面空间混色，而不是只把每个 panel pixel 当成独立 RGB 点。

### 读数输入

校准 UI 让用户直接输入色差仪读数：

- RGB：`[151,161,160]`
- CMYK：`[5%,0%,0%,36%]`

纯墨水读数更新 palette；mixed patch 读数更新 `calib.mixtures.patches`。

### 校准混色图案

`/api/calib-mix?mix=<ink>:<weight>,...` 在设备上显示 mixed patch。图案生成不是 Bayer ordered screen，而是每个 16x16 tile 内的 hash permutation：

- tile 内比例接近精确。
- tile 间排列不同。
- 避免色差仪孔径下出现条带/周期纹。
- 产生更适合测量平均反射率的细颗粒纹理。

### Fallback optical model

如果没有 mixed patch 读数，浏览器用六个纯墨水和 Yule-Nielsen-like exponent 估算局部混色。只要用户测了 mixed patch，`opticalMixExponent()` 会拟合更接近该面板/环境光的指数。

该模型目前用于：

- `applyPanelPreviewOptics()`：让 dithered preview 更像实体 E6。
- Auto scoring：用 optical luma 而不是只用单像素 palette RGB。

核心 dither C ABI 目前仍使用 palette + heuristic bias，不直接接收 mixed patch 表；这是后续可继续优化的方向。

## Auto ISP-like Pipeline

Auto 按图像传感器 ISP 的结构拆成三段：

1. AE / tone：用亮度分位数和场景类型决定 exposure、contrast、gamma。
2. AWB / color：对中性图做灰世界修正，同时保护森林、天空、产品色、肤色。
3. Detail / noise：用 smoothness、sharpen、vignette 控制纸面噪声和细节。

Settings 中的 Auto AE target 作为 `auto.exposureBias` 加到 AE 结果上：

- 默认 `-1/3 EV`。
- 可调范围 `-3 EV..+3 EV`。

Settings 中的 Auto temperature bias 作为 `auto.temperatureBias` 加到色温结果上。

Auto 后还会在小尺寸面板网格上跑候选参数 simulation，用 E6 dither 和 optical luma 评分，避免只优化手机预览。

## Russian 纯色填充 Pipeline

Russian preset 是非 dithering pipeline，目标不是自然照片连续色调，而是接近苏联构成主义海报的视觉语言：

- 有限色。
- 大色块。
- 白底、黑结构、红色强调。
- 强几何/边缘感。
- 尽量没有 halftone 点状纹理。

整体流程：

```text
输入图片
  -> 同样的 crop / fit / rotate
  -> 同样的 adjust + EINK enhancement
  -> 1.5x Catmull-Rom downsample
  -> 稍强 unsharp mask
  -> Lab gamut mapping
  -> constructivist flat palette pick
  -> edge-aware mode cleanup
  -> packed 4bpp EPD framebuffer
```

Russian 直接选择一个墨水色，不传播误差。它的 `constructivist_palette_bias()` 会让 red、black、yellow、white 更容易被选中，同时用 guardrail 防止绿色/蓝色图片整体推红。

## Preview 与真实面板

Dithered preview 使用最终 15x WASM quantization 的 raw indices，再用 calibration palette RGB 渲染，并用 mixed-patch optical model 修正局部观感。

这比旧的低分辨率直接 preview 更接近最终 `.bin`，但仍有差异：

- 实际环境光会影响白点和色彩饱和度。
- EINK 表面材质和视角会影响反射。
- 手机屏幕显示预览时又经过另一套显示色彩管理。

因此 preview 是“面板外观近似”，最终仍以实体面板为准。

## 当前主要 preset

| Preset | 目标 |
| --- | --- |
| Balanced | 默认自然照片，综合平衡，较早期更高 saturation/vibrance |
| Landscape | 风景，较高 chroma 和 clarity |
| Portrait | 人像，保留肤色，降低刺眼色点 |
| Anime | 线条和纯色区域更强，低 smoothness |
| Document | 文档/高对比图，锐化和对比更高，但仍提高基础 chroma |
| Russian | 非 dithering，构成主义纯色块 |

## 重要设计取舍

### 为什么没有直接用物理 XYZ 面积平均做 dither

更“物理正确”的 XYZ / reflectance 局部平均 refinement 曾导致绿色背景图出现红偏。原因是：

- 六色墨水 gamut 非常不均匀。
- 纯数学局部平均可能为了降低某个 Lab 误差，引入视觉上非常显眼的红点。
- 人眼对红点的敏感度高于它在平均 Lab 中体现的权重。

因此当前算法更偏向感知约束：宁可局部平均稍差，也不允许高可见污染点破坏观感。

### 为什么混色模型主要影响预览和 Auto

Mixed patch 数据可以很好地描述“色差仪看到的局部平均”，但 dither 的逐像素决策还需要考虑纹理、可见噪声、边缘污染和局部结构。当前策略是：

- dither C 核心使用实测 palette + E6 可见性 heuristic，保证稳定、快速、可解释。
- preview / Auto 使用 mixed-patch optical model，保证用户看到和 Auto 评分更接近真实面板。

下一步如果要继续优化，可以把 mixed patch 拟合出的参数通过 WASM ABI 传入 C dither，让 picker 直接 scoring 局部 optical mix。

### 为什么不用完整 AI 模型

当前目标是在浏览器 WASM 和 ESP32 设备流程中稳定运行。完整 segmentation 或 neural enhancement 会带来：

- WASM 体积增加。
- 初始化和推理变慢。
- 移动端兼容性复杂。
- 可解释性降低。

当前实现采用 deterministic local classifier 和 enhancement，接近“轻量 ISP block”的作用，但没有模型权重和训练依赖。

## 调参建议

- 想要最好的实体 E6 照片效果：使用默认 E6 Mix，让 Auto 先给初值。
- 想要更接近旧版本纹理：Settings 里切到 Classic VED。
- 想要更少点状噪声：提高 smoothness，但会更 posterized。
- 想要更清晰：提高 sharpen，但过高会产生边缘 halo 或错误色边。
- 绿色图发红：降低 temperature、降低红色友好的风格化 preset，避免 Russian。
- 白底脏点：提高 smoothness，降低 exposure，或把 Auto AE target 调得更负。
- 想要更强色彩：提高 saturation / vibrance；当前默认已经比早期更高。
- 想要纯设计感：使用 Russian。

# Paper E6 图像处理 Pipeline

本文档解释 RGB 图片到 6 色 EINK Spectra / PaperColor 面板的整体处理流程、颜色科学假设、数学目标函数，以及当前两类输出 pipeline：默认照片 dithering pipeline 与 Russian 纯色块 pipeline。

## 目标

本项目的目标不是把输入图片转换成“理想 sRGB 六色图”，而是在真实反射式电子纸上，让人眼看到的结果尽量接近原图的主要观感。这里有几个约束：

- EINK 是反射式显示，不是发光显示；白色、黑色和彩色墨水的亮度范围都远小于手机屏幕。
- 6 色面板只有 black、white、yellow、red、blue、green 六种实际墨水，连续色调必须通过空间混合或纯色块取舍表达。
- 人眼对不同颜色噪声的敏感程度不同：白底上的红/蓝/黑点非常显眼，黄点相对不明显。
- 面板实测颜色和标称颜色差异很大，必须使用实测色来做匹配。
- 最终判断标准是物理面板上的主观观感，因此算法会在“物理正确”和“感知更接近”之间偏向后者。

## 实测面板色彩

当前调色板来自色差仪测量结果，代码中保存在 `main/palette.c`：

| 墨水 | 实测 RGB |
| --- | --- |
| BLACK | `[70, 71, 80]` |
| WHITE | `[151, 161, 160]` |
| YELLOW | `[163, 154, 69]` |
| RED | `[118, 69, 70]` |
| BLUE | `[60, 97, 134]` |
| GREEN | `[77, 100, 76]` |

这些值并不直接作为“输入图片应该变成的 RGB”，而是作为面板在指定光源和测量条件下的外观近似。启动时通过 `palette_init()` 转换到 CIELAB，保存在 `PALETTE_LAB` 中，后续颜色距离都在 Lab / CIEDE2000 空间进行。

## 为什么不用理想 RGB 匹配

如果直接把输入 sRGB 和理想六色，例如 `[255, 0, 0]`、`[0, 255, 0]` 比较，会产生两个问题：

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

LUT 避免每个像素重复 `powf()`，对 WASM 和 ESP32 都更快。

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

Lab 不是完美的人眼模型，但比 RGB 欧氏距离更接近感知。

### CIEDE2000

候选墨水选择使用 CIEDE2000，而不是简单 `sqrt(dL^2 + da^2 + db^2)`。CIEDE2000 通过以下修正更接近人眼：

- 对低/高 chroma 的色差灵敏度不同。
- hue 差异不是线性角度差。
- 蓝紫区域有额外旋转项。
- L、C、H 三项有不同权重。

代码实现为 `ciede2000()`。对于本项目，它的作用是：在实测面板 Lab 空间中，选择人眼更可能认为相近的墨水或局部混合结果。

## Source 到 Panel Lab 的压缩

默认 pipeline 不直接比较输入 Lab 和墨水 Lab，而是先做 `map_source_to_panel_lab()`：

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
- chroma 只保留约 44%，因为六色纸面 gamut 很小，保留过多饱和度会让错误颜色点过多。
- 中性轴使用实测黑白的 a/b 漂移，而不是假设纸白为纯中性。

数学上，这是一个从手机/相机 sRGB 外观空间到面板可表达外观空间的 gamut mapping。

## 默认照片 Dithering Pipeline

默认 pipeline 面向自然照片，目标是在空间平均意义上接近原图，同时避免明显噪点和纹理。

整体流程：

```text
输入图片
  -> Stage 1 crop / fit / rotate
  -> 1.5x supersample canvas, 例如 600x900
  -> JS extra adjust: exposure / vibrance / vignette / sharpen
  -> WASM adjust: brightness / contrast / saturation / gamma / temperature / tint
  -> EINK-aware enhancement
  -> Catmull-Rom downsample 到面板分辨率 400x600
  -> Unsharp mask
  -> Lab gamut mapping
  -> CIEDE2000 palette pick
  -> edge-aware Stucki error diffusion
  -> non-causal local refinement
  -> packed 4bpp EPD framebuffer
```

### 1.5x Supersample

最终面板为 `400 x 600`，浏览器端 bake 使用 `600 x 900`。WASM 中再用 Catmull-Rom cubic kernel 下采样到面板分辨率。

这样做的原因：

- 直接把高分辨率原图 resize 到 400x600 会丢失一部分细节。
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

`main/web/index.html` 中的 `applyExtraAdjustRgb888()` 处理：

- exposure：乘法曝光，调整整体亮度。
- vibrance：对低饱和区域提升更大，对高饱和区域提升较小。
- vignette：补偿边角暗角。
- sharpen：用户可控的高频增强。

这些参数没有进入 C ABI，是浏览器端预处理。

### WASM Adjust

`adjust_cfg_t` 包含：

- brightness
- contrast
- saturation
- gamma
- temperature
- tint
- smoothness

处理顺序在 `apply_adjust_rgb888()`：

```text
rgb = rgb * contrast + brightness
apply temperature on red/blue axis
apply tint on green/magenta axis
HSV saturation scaling
apply gamma LUT
```

### EINK-aware Enhancement

`enhance_eink_rgb888()` 是一个轻量 deterministic enhancement，不依赖模型。它做局部 base/detail 分离、clarity/detail boost、轻微 S 曲线和 chroma separation，目的是让六色量化前的局部边缘和颜色分区更清晰。

### Edge-aware Stucki Error Diffusion

默认 diffusion 使用 Stucki kernel，而不是 Floyd-Steinberg：

```text
            x+1  x+2          8/42  4/42
x-2 x-1  x  x+1  x+2    2/42  4/42  8/42  4/42  2/42
x-2 x-1  x  x+1  x+2    1/42  2/42  4/42  2/42  1/42
```

优势：

- 扩散距离比 Floyd-Steinberg 更远，平滑渐变更柔和。
- 总权重为 1，理论上保持 DC tone。
- 对 EINK 这种低色数反射介质，较宽 kernel 通常比局部强噪声更自然。

当前实现还会根据 source edge strength 调整远距离扩散：

```text
far = 1 - (edge - 14) / 58
far = clamp(far, 0, 1)
norm = 42 / (32 + 10 * far)
```

含义：

- 平坦区域：保留第二未来行扩散，让过渡更平滑。
- 强边缘：减少远行扩散，避免颜色跨边界污染。
- `norm` 重新归一化近场权重，避免因为减少远场扩散而丢失误差能量。

### Smoothness Snap

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

### Region Classifier

当前代码使用轻量规则分类：

- `REG_EDGE`
- `REG_FLAT`
- `REG_WARM_SKIN`
- `REG_SKY_COOL`
- `REG_DARK`
- `REG_LIGHT`

它不是语义分割模型，而是 WASM/ESP32 友好的局部启发式。用途是给 palette picker 加小 bias：

- 肤色/暖色区域抑制蓝绿污染。
- 天空/冷色区域抑制黄绿污染。
- 高亮区域惩罚黑/红/蓝点。
- 平坦区域增强 anti-cluster。

### Human Visibility Bias

人眼在浅色纸面上对高对比色点很敏感。当前 `region_palette_bias()` 对高 L 区域做了非对称惩罚：

- black 最重。
- red / blue 较重。
- green 较轻。
- yellow 有轻微奖励。

这不是为了让颜色数值更准确，而是为了让最终观感更干净。白底黄点的色差可能存在，但视觉打扰远小于白底红点。

### Spatial Anti-cluster

`pick_palette_spatial()` 不只看 CIEDE2000，还看已量化邻居：

```text
score = deltaE + same-neighbour penalty + region bias
```

如果候选色和 left / up / diagonal 邻居相同，会加小惩罚。平坦区域惩罚更强，边缘区域惩罚更弱。

作用：

- 避免同色点局部聚集成斑块。
- 让 halftone 更接近蓝噪声观感。
- 边缘处不过度拆散颜色，保留结构清晰度。

### Local Refinement

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

这一步能改善邻近过渡色表现：不是只追求单像素最近，而是让局部窗口的平均颜色更接近目标。

## Russian 纯色填充 Pipeline

Russian preset 是新增的非 dithering pipeline，设计目标不是自然照片连续色调，而是更接近苏联构成主义海报的视觉语言：

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

### 不做 Dithering 的原因

Dithering 的数学目标是局部平均色接近原图；Russian preset 的目标是纯色块风格。两者目标不同：

- Dithering：用空间混色换取连续 tone。
- Russian：用硬量化和色块取舍换取平面设计感。

因此 Russian pipeline 直接选择一个墨水色，不传播误差。

### Constructivist Bias

`constructivist_palette_bias()` 在 CIEDE2000 基础上添加风格 bias：

- red、black、yellow、white 稍微更容易被选中。
- blue、green 稍微更难被选中。
- 但 bias 远小于强颜色证据，避免把绿色/蓝色图片整体推红。

关键 guardrail：

- 如果源图明显偏绿，强惩罚 red，奖励 green。
- 如果源图明显偏蓝，强惩罚 red/yellow，奖励 blue。
- 如果源图明显偏红，奖励 red，惩罚 blue/green。
- 高亮区域继续惩罚黑/红/蓝点，避免白底脏点。

数学形式：

```text
score(p) = CIEDE2000(target_lab, palette_lab[p])
         + constructivist_bias(region, source_rgb, p)
```

### Edge-aware Mode Cleanup

纯色填充如果逐像素最近邻，会出现小碎点。Russian pipeline 后处理 3 次 3x3 mode cleanup：

1. 统计当前像素 3x3 邻域中最多的墨水色。
2. 如果 mode 色足够占优，并且替换后的 CIEDE2000 损失不超过阈值，则替换。
3. 边缘处阈值更严格，平坦区域阈值更宽松。

近似规则：

```text
allowance = 1.6, strong edge
allowance = 3.2, medium edge
allowance = 6.4, flat region
```

作用：

- 去掉孤立 speckle。
- 形成更稳定的色块。
- 保留高对比边缘。

## Preview 与真实面板

预览图用实测 `PALETTE_RGB_MEASURED` 渲染，而不是理想颜色。这样浏览器预览更接近真实 EINK 外观。

但是仍有差异：

- 实际环境光会影响白点和色彩饱和度。
- EINK 表面材质和视角会影响反射。
- 相机/手机屏幕显示预览时又经过另一套显示色彩管理。

因此 preview 是“面板外观近似”，最终仍以实体面板为准。

## 当前主要 preset

| Preset | 目标 |
| --- | --- |
| Balanced | 默认自然照片，综合平衡 |
| Landscape | 风景，较高 chroma 和 clarity |
| Portrait | 人像，保留肤色，降低刺眼色点 |
| Anime | 线条和纯色区域更强，低 smoothness |
| Document | 文档/高对比图，锐化和对比更高 |
| Russian | 非 dithering，构成主义纯色块 |

## 重要设计取舍

### 为什么没有直接用物理 XYZ 面积平均

曾尝试过更“物理正确”的 XYZ / reflectance 局部平均 refinement，但在绿色背景图上产生明显红偏。原因是：

- 六色墨水 gamut 非常不均匀。
- 纯数学局部平均可能为了降低某个 Lab 误差，引入视觉上非常显眼的红点。
- 人眼对红点的敏感度高于它在平均 Lab 中体现的权重。

因此当前算法更偏向感知约束：宁可局部平均稍差，也不允许高可见污染点破坏观感。

### 为什么不用完整 AI 模型

当前目标是在浏览器 WASM 和 ESP32 设备流程中稳定运行。完整 segmentation 或 neural enhancement 会带来：

- WASM 体积增加。
- 初始化和推理变慢。
- 移动端兼容性复杂。
- 可解释性降低。

当前实现采用 deterministic local classifier 和 enhancement，接近“轻量 AI block”的作用，但没有模型权重和训练依赖。

## 调参建议

- 想要更接近照片：使用 Balanced / Landscape / Portrait，并让 Auto 选择。
- 想要更少点状噪声：提高 smoothness，但会更 posterized。
- 想要更清晰：提高 sharpen，但过高会产生边缘 halo 或错误色边。
- 绿色图发红：降低 temperature、降低 red-friendly preset，避免 Russian 或过高暖色 bias。
- 白底脏点：提高 smoothness，使用 Document 或降低 saturation/vibrance。
- 想要纯设计感：使用 Russian。

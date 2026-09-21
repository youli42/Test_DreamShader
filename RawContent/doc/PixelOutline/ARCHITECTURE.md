# PixelOutline 架构与原理

## 一、效果从哪来

原始实现位于 Unity 工程 `URP_Learning` 的 `ScreenPixel` Renderer Feature：

- 代码：`URP学习/Assets/Rendering/ScreenPixel/ScreenPixelRenderFeature.cs`
- 着色器：`ScreenPixelOutline.shader`（3 个 Pass）+ `ScreenPixelOutline.hlsl`（331 行，核心算法）

该 Feature 是整个 Unity 工程里**唯一处于启用状态**的描边实现（`m_Active: 1`），其余
（`BackFaceOutlines`、`Outlines_2`、`DepthNormalsOutlines`）都未启用。

## 二、三段式数据流

```
[Tonemap 之后的 SceneColor，全分辨率]
        │
        │  Pass0  PixelAndDetect（PS，全分辨率）
        │    · 线性采样做像素化（fwidth 平滑块边界）
        │    · 深度 / 法线 / 亮度 / RGB 四路 3x3 Sobel
        │    · max 合并 → smoothstep → step 二值化
        ▼
[PackedColorAndEdge]  全分辨率，PF_A16B16G16R16
        │             RGB = 像素化颜色，A = 二值边缘掩码
        │
        │  Pass1  ReduceOutline（PS，画到低分辨率网格）
        │    · 一个像素 = 一个块
        │    · 块内 A 取 max（不是平均、不是中心采样）
        │    · RGB 取块中心颜色
        ▼
[OutlineGrid]  ceil(W/bs) × ceil(H/bs)
        │
        │  Pass2  Composite（PS，全分辨率）
        │    · 用屏幕 UV 反算块索引
        │    · 网格 UV = (blockIndex + 0.5) / gridSize，Point 采样
        │    · lerp(块颜色, 描边色, step(0.5, mask) * alpha)
        ▼
[写回 SceneColor / OverrideOutput]
```

**为什么要三段，而不是"先降采样再检测"或"先检测再降采样"？**

- 先降采样再检测：细轮廓在降采样时就丢了。
- 先检测再降采样：细线被采样丢失、块内颜色不均。
- 三段式：全分辨率保留精确边缘，`max` 归约保证"块内只要有一个边缘像素，整块就是描边块"。

## 三、插入点

| Unity | UE |
| --- | --- |
| `RenderPassEvent 600` / `AfterRenderingPostProcessing` | `EPostProcessingPass::Tonemap` = `BL_SceneColorAfterTonemapping` |

语义一致：Bloom、Vignette、Tonemapping 的结果都会一起进入像素块网格。

实现方式：`FPixelOutlineViewExtension : FSceneViewExtensionBase` 订阅该 Pass，
在一个回调里**顺序**添加三个 Pass（天然满足 Pass1/2 不早于 Pass0 的依赖约束，
等价于 Unity 里 `outlinePassEvent` 被钳制到不早于 `pixelPassEvent`）。

## 四、与 Unity 的逐条对照（"必须复刻"清单）

| # | Unity 行为 | 本实现 |
| --- | --- | --- |
| 1 | 三段式：检测 → 块归约 → 块索引合成 | 三个 Pass 一一对应 |
| 2 | 块内归约取 **max** | `ReduceOutlinePS` 双层循环 `max`，含 `if (mask >= 1.0) break` 提前退出 |
| 3 | 四路信号用 **max** 合并（不是加权和） | `GetOutlineEdgeResponse()` |
| 4 | 先 `smoothstep(t, t+softness)` 再 `step(0.5)`，**归约在二值化之后** | `GetBinaryOutlineMask()` → Pass1 |
| 5 | 深度 = `log2(线性眼空间深度)`，下限取近裁剪面 | `log2(max(CalcSceneDepth(uv), NearClip))` |
| 6 | Sobel 归一化系数 `0.25` | `kSobelNormalization = 0.25` |
| 7 | 采样步长以**源像素**为单位（`1/W, 1/H`） | `SampleStep = SourceSizeAndInvSize.zw * Thickness` |
| 8 | 合成按块索引反查网格 texel：`(idx + 0.5) / gridSize` | `CompositePS`，Point 采样 |
| 9 | 像素化用 `fwidth` 平滑块边界 | `SamplePixelatedColor()`（保留 Unity 的 `boxSize/blockOffset` 推导） |
| 10 | 插入点在色调映射之后 | `EPostProcessingPass::Tonemap` |

### UE 侧必须做的语义转换

| 项 | Unity | UE | 处理 |
| --- | --- | --- | --- |
| 深度 | `LinearEyeDepth(rawDepth, ...)` | `CalcSceneDepth()` 已返回**线性世界单位深度** | 删掉线性化那一步，保留 `log2` |
| 正交分支 | `LinearDepthToEyeDepth` vs `LinearEyeDepth` | `ConvertFromDeviceZ` 已统一处理 | C++ 侧仍传 `bIsOrthographic` 备用 |
| 法线 | URP 相机法线纹理 | GBufferA，`N*0.5+0.5` 编码，**未归一化** | 解码 `*2-1` 后自行 `normalize()` |
| 亮度 | `GetLuminance` | 无同名内置 | 内联 Rec.709 `float3(0.2126,0.7152,0.0722)` |
| 近裁剪下限 | `_ProjectionParams.y` | 无直接对应 | C++ 显式传 `DepthParams.x` |
| 输出 Alpha | `1.0` 表示不透明 | **`0.0` 表示不透明**（语义相反） | Pass2 输出 `Alpha = 0` |

## 五、边缘检测细节

### Sobel 核

```
偏移量：3x3 邻域（中心权重 0）
kSobelX = {-1, 0, 1, -2, 0, 2, -1, 0, 1}
kSobelY = { 1, 2, 1,  0, 0, 0, -1,-2,-1}
```

一次 `[unroll] for (i = 0..8)` 循环内**同时**累积四路信号，避免四次重复采样偏移计算。

归一化 `0.25`：Sobel 正负权重各自总和为 4，乘 0.25 把响应拉到便于设阈值的范围。

### 四路信号

| 信号 | 取值 | 用途 |
| --- | --- | --- |
| 深度 | `log2(线性深度)` | 物体外轮廓、深度断层 |
| 世界法线 | GBufferA 解码并归一化 | 同一深度平面上的表面转折 |
| 亮度 | Rec.709 | 可选（`UseLuminanceEdge`） |
| RGB | 场景颜色 | 材质颜色边界（`ColorWeight=0` 默认关闭） |

合并方式（`max`，不是求和）：

```hlsl
GeometryEdge   = max(depth * DepthWeight, normal * NormalWeight);
LuminanceEdge  = luminance * LuminanceWeight * step(0.5, UseLuminanceEdge);
AppearanceEdge = max(LuminanceEdge, color * ColorWeight);
return max(GeometryEdge, AppearanceEdge);
```

用 `max` 是为了**避免多个低强度噪声相加被误判成真实边缘**。

### 二值化

```hlsl
softMask = smoothstep(Threshold, Threshold + Softness, response);
mask     = step(0.5, softMask);
```

先软后硬，保证归约阶段只看到"整块描边 / 整块干净"，不会出现渐变黑边。

## 六、尺寸与 UV 约定（最容易出错的地方）

三个 Pass 共用**同一套源像素尺寸**，这是块索引保持一致的前提（对应 Unity 的
`_ScreenPixelSourceSize`，由 Feature 显式传入而非读取相机尺寸）：

- `SourceSizeAndInvSize` = `(W, H, 1/W, 1/H)` —— 全分辨率源尺寸
- `GridSizeAndBlockSize.xy` = `ceil(W/bs) × ceil(H/bs)`
- `GridSizeAndBlockSize.zw` = 块尺寸（`clamp(round(PixelSize), 1, 64)`）

块索引与块中心 UV（与 Unity 逐字一致）：

```hlsl
blockIndex = min(floor(saturate(UV) * sourceSize / blockSize), gridSize - 1);

blockStart  = blockIndex * blockSize;
blockExtent = min(blockSize, sourceSize - blockStart);   // 处理最后一行/列不足整块
blockCenter = (blockStart + 0.5 * blockExtent) / sourceSize;
```

Pass2 用 `UVAndScreenPos.xy`（相对输出视口）反算块索引；网格采样用
`(blockIndex + 0.5) / gridSize` 并**Point 采样**，因此同一块内每个屏幕像素结果完全一致。

## 七、为什么归约用像素着色器而不是 compute

Unity 原实现用的是全屏 Blit（`AddBlitPass`），不是 compute dispatch。

本实现最初用 `FComputeShaderUtils::AddPass`（线程组 8×8）做归约，但实测发现
`ClearUnusedGraphResources` 会把 UAV 参数静默清空（日志实测 `UAV=NULL`），
导致 `OutlineGrid` 从未被写入、Pass2 读到未初始化显存（表现为整屏单色）。
改为 PS 画到低分辨率网格后问题消失，且与 Unity 原实现形态一致。
详见 [`NOTES-UE58.md`](NOTES-UE58.md)。

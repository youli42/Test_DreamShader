# PixelOutline

Unity URP `ScreenPixel` 像素描边效果的 UE 5.8 移植实现。

它把画面做成**像素块**，并用**严格对齐像素块的整块描边**勾出物体轮廓：

- 先在全分辨率检测精确边缘，再归约到像素块 —— 细轮廓不会因为没有经过块中心而丢失；
- 描边占据**完整像素块** —— 不会出现同一块内颜色不均或只有零散小点。

## 60 秒上手

1. 确认插件已启用：`编辑 ▸ 插件` 中搜索 `PixelOutline`（项目 `Plugins/` 下的插件默认启用）。
2. 打开展示关卡：`Content/PixelOutline/L_PixelOutline_Showcase`（地面 + 3 个立方体 + 球 + 圆柱，正交相机正对）。
3. 视口中应立刻看到像素化画面 + 白色块状描边。

效果对**任何视口**生效（编辑器视口、PIE、独立游戏都算），不需要放置后处理体积。

## 运行时参数

在控制台（`` ` `` 键）输入即可，无需重启：

| 控制台变量 | 默认 | 说明 | Unity 对应 |
| --- | --- | --- | --- |
| `r.PixelOutline.Enable` | `1` | 总开关 | Feature `m_Active` |
| `r.PixelOutline.PixelSize` | `13` | 像素块边长（源像素），钳制 `[1,64]` | `_PixelSize` |
| `r.PixelOutline.OutlineColor.R/G/B/A` | `1` | 描边颜色（RGBA 四个变量） | `_OutlineColor` |
| `r.PixelOutline.OutlineThickness` | `1.0` | Sobel 采样半径倍数（源像素） | `_OutlineThickness` |
| `r.PixelOutline.DepthWeight` | `1.5` | 深度信号权重 | `_DepthWeight` |
| `r.PixelOutline.NormalWeight` | `1.0` | 世界法线信号权重 | `_NormalWeight` |
| `r.PixelOutline.UseLuminanceEdge` | `0` | 是否启用亮度边缘（`0/1`） | `_UseLuminanceEdge` |
| `r.PixelOutline.LuminanceWeight` | `1.0` | 亮度信号权重 | `_LuminanceWeight` |
| `r.PixelOutline.ColorWeight` | `0.0` | RGB 颜色信号权重（`0` 即关闭） | `_ColorWeight` |
| `r.PixelOutline.EdgeThreshold` | `0.08` | 边缘响应阈值 | `_EdgeThreshold` |
| `r.PixelOutline.EdgeSoftness` | `0.04` | 阈值过渡宽度 | `_EdgeSoftness` |
| `r.PixelOutline.DebugMode` | `0` | 调试视图，见下表 | —— |
| `r.PixelOutline.NormalSource` | `0` | `0` GBufferA 法线；`1` 关闭法线信号（排查用） | —— |

### DebugMode（定位问题最快的方式）

| 值 | 显示内容 | 用途 |
| --- | --- | --- |
| `0` | 完整链路（像素化 + 描边） | 正常 |
| `1` | Pass0 的像素化颜色 | 确认像素化/采样是否正常 |
| `2` | **全分辨率**边缘掩码（黑白） | 确认 Sobel 信号与阈值 |
| `3` | **块级**掩码（归约后） | 确认块归约与块对齐 |

推荐排查顺序：`2`（有没有信号）→ `3`（归约对不对）→ `0`（合成对不对）。

## 相关资产

| 路径 | 说明 |
| --- | --- |
| `Content/PixelOutline/L_PixelOutline_Showcase` | 紧凑展示关卡，用来看效果 |
| `Content/PixelOutline/L_PixelOutline_Test` | 与 Unity `SampleScene` 同构的对比关卡（正交相机 = Unity `orthographic size 10.6`） |
| `Content/PixelOutline/Debug/M_Debug_WorldNormal` | 阶段 0 验证材质：直接输出 `SceneTexture:WorldNormal` |
| `Content/PixelOutline/Debug/M_Debug_SceneDepth` | 阶段 0 验证材质：输出 `log2(线性深度)` 灰度 |

调试材质由 DreamShader 生成，源文件在 `DShader/PixelOutline/Debug/*.dsm`。

## 文档

- [`ARCHITECTURE.md`](ARCHITECTURE.md) —— 三 Pass 原理、与 Unity 的逐条对照、算法与 UV 约定
- [`NOTES-UE58.md`](NOTES-UE58.md) —— UE 5.8 适配踩坑记录、已知问题与标定/验收流程

## 源码位置

```
Plugins/PixelOutline/
├── PixelOutline.uplugin
├── Source/PixelOutline/
│   ├── Public/PixelOutlineViewExtension.h       视图扩展（插入点）
│   ├── Private/PixelOutlineViewExtension.cpp    三 Pass 编排
│   ├── Private/PixelOutlineShaders.h/.cpp       全局着色器类 + 控制台变量
│   └── Private/PixelOutlineParameters.h         参数结构（CPU/GPU 共用）
└── Shaders/
    ├── PixelOutlineCommon.ush                   Sobel/像素化/块索引 公共 HLSL
    ├── PixelOutlinePixelAndDetect.usf           Pass0 像素化 + 边缘检测
    ├── PixelOutlineReduceOutline.usf            Pass1 块内 max 归约
    └── PixelOutlineComposite.usf                Pass2 合成
```

# UE 5.8 适配踩坑与问题记录

本文记录把 Unity URP `ScreenPixel` 迁到 UE 5.8 过程中**实际踩到并验证过**的坑，
以及当前遗留问题。所有结论都以本机引擎源码为准：

```
C:/Program Files/Epic Games/UE_5.8/Engine/Source/...
```

> 迁移方案文档里引用的 `E:\E_SATA_WP\Code\UnrealEngine` 在本机不存在，凡涉及源码行号
> 一律以上述路径重新核实。

---

## 一、编译与链接

### 1. 全局着色器注册时机

`IMPLEMENT_GLOBAL_SHADER` 会在模块 DLL 加载时（静态初始化）注册着色器类型。
如果模块加载太晚，会直接断言失败：

```
Assertion failed: !AreShaderTypesInitialized()  [Shader.cpp:315]
模块"PixelOutline"无法被加载，因此插件"PixelOutline"加载失败。
```

`LoadingPhase` 从 `PostEngineInit` 改到 **`PostConfigInit`** 后正常。
视图扩展的创建再延后到 `FCoreDelegates::OnPostEngineInit`，避免过早依赖场景渲染侧。

### 2. 每个 `.usf` 必须包含 `/Engine/Public/Platform.ush`

否则报错：

```
Error: Shader is required to include /Engine/Public/Platform.ush
```

### 3. 松散着色器参数需要在 `.usf` 里自己声明

UE **不会**为松散（loose）参数生成 HLSL 声明——引擎自己也是手写的，
参见 `Engine/Shaders/Private/PostProcessDownsample.usf`：

```hlsl
Texture2D InputTexture;
SamplerState InputSampler;
SCREEN_PASS_TEXTURE_VIEWPORT(Input)
```

所以 `PixelOutlineCommon.ush` 里必须自己写出全部参数变量；忘记声明会表现为
"编译通过但值恒为 0"，而不是编译错误。

### 4. uniform buffer 的 HLSL 访问名是"展平"形式

引擎生成的 uniform buffer 成员在 HLSL 里是 `<BufferName>_<Member>`，例如：

- `View_ViewSizeAndInvSize`
- `SceneTexturesStruct_SceneDepthTexture`
- `SceneTexturesStruct_GBufferATexture`
- `FScreenPassTextureViewportParameters Input` → `Input_UVViewportMin`

因此读 GBufferA 要写 `SceneTexturesStruct_GBufferATexture`，而不是
`SceneTexturesStruct.GBufferATexture`；viewport 参数要用宏
`SCREEN_PASS_TEXTURE_VIEWPORT(Input)` 声明后使用 `Input_*`。

---

## 二、渲染资源

### 5. `FSceneTextures::Get(GraphBuilder)` 在 5.8 不存在

全树无此符号。后处理回调里改用 `FPostProcessMaterialInputs::SceneTextures`：

```cpp
PassParameters->SceneTextures = Inputs.SceneTextures;   // FSceneTextureShaderParameters
```

### 6. Tonemap 之后 SceneDepth / GBufferA 仍然有效

已核实（避免"GBuffer 已被释放"的风险）：

- `DeferredShadingRenderer.cpp` 在进后处理前以 `ESceneTextureSetupMode::All` 建立 uniform buffer；
- 同一个 UB 全程传入后处理；
- AfterTonemapping 的 PPM 链仍用它，因此 `EPostProcessingPass::Tonemap` 回调里
  `SceneDepthTexture` 与 `GBufferATexture` 都可读。

### 7. UAV 会被 `ClearUnusedGraphResources` 静默清空

Pass1 最初用 compute + UAV 写 `OutlineGrid`，实测日志：

```
PixelOutline: after AddPass UAV=NULL
Pass Composite has a read dependency on PixelOutline.OutlineGrid, but it was never written to.
```

后果：`OutlineGrid` 从未写入 → Pass2 采样未初始化显存 → **整屏单色（黄色）**。
加 `ERDGPassFlags::NeverCull` 无效（问题不是被剔除，而是 UAV 参数被清空）。

**修复**：归约改为 PS 画到低分辨率网格（一个像素 = 一个块），与 Unity 的全屏 Blit 形态一致。

### 8. `int2` 参数在 HLSL 侧读到 0

参数块里混合 `FIntPoint`（int2）与 `FVector4f` 时，CPU 打包与 HLSL cbuffer 的对齐规则
不一致，`GridSize` / `BlockSize2D` 在着色器里读到 **0**，导致：

```
GetPixelBlockCenterUV: blockStart = 0, blockExtent = 0
→ 每个块的颜色都取自画面左上角那一个像素 → 整屏一个颜色
```

而 A 通道（边缘掩码）不依赖这两个值，所以表现为"**有边缘信号但画面纯色**"，非常容易误判。

**修复**：公共参数块**全部改为 `FVector4f`**（`GridSize` 与 `BlockSize2D` 合并成
`GridSizeAndBlockSize` 的 `xy` / `zw`），让 CPU 与 HLSL 布局按 16 字节对齐天然一致。

验证数据（最终合成画面的像素统计）：

| | 修复前 | 修复后 |
| --- | --- | --- |
| 平均亮度 | 0.000（全黑） | 0.255 |
| 白色像素占比 | 0.00% | 13.36% |

### 9. Pass2 的 OverrideOutput 契约

`EPostProcessingPass::Tonemap` 属于"可能是最后一个 Pass"的档位，`Inputs.OverrideOutput`
可能直接指向 backbuffer。当前实现优先写 OverrideOutput，否则用
`FScreenPassRenderTarget::CreateFromInput` 回写 SceneColor，两种情况都返回 Output 交给链继续。

---

## 三、DreamShader 相关

- 调试材质（`M_Debug_WorldNormal` / `M_Debug_SceneDepth`）默认后端 `ThinCustom` 只在内存生成，
  要被后处理体积持久引用必须先落盘：

```
UnrealEditor-Cmd.exe Test_DreamShader.uproject -run=DreamShader compile -Source="DShader/PixelOutline/Debug/M_Debug_WorldNormal.dsm" -Force
```

- `Settings` 里的 `BlendableLocation` 枚举要写 **`SceneColorAfterTonemapping`**
  （去掉 `BL_` 前缀后的完整名，不是 `AfterTonemapping`）。

---

## 四、当前已知问题

### 问题 1：移动视角时描边闪烁

**已排除**：逐帧噪声/抖动。实验（静止相机、纯掩码视图、连拍对比）：

| 条件 | 帧间差异 |
| --- | --- |
| TAA 开启 | 0.00% |
| TAA 关闭（`r.PostProcessAAQuality 0`） | 0.00% |

**结论**：闪烁只发生在相机移动/缩放过程中，是两层叠加：

1. **算法固有放大**：Sobel 响应 → 二值化 → 块归约，会把亚像素级的边缘抖动放大成
   "整块描边开/关"。相机移动时边缘扫过像素块边界，视觉上就是描边块跳动。
2. **参数处在噪声带**：默认 `DepthWeight 1.5` 下掩码仅覆盖 2.5%，阈值 0.08 卡在响应噪声带边缘。

**缓解方向**：`DepthWeight 1.5 → 5`（实验中 20 时掩码升到 28.9%，远离噪声带）、
`EdgeSoftness 0.04 → 0.1+` 让二值化落在 smoothstep 平台区。
根治手段是对边缘响应做轻量时域混合（后续项）。

### 问题 2：拉伸视口时出现异常外围显示

**已排除**：输入/输出尺寸错位。诊断日志实测两者完全一致：

```
PixelOutline extents: source=1096x520 outputTex=1096x520 outputRect=(0,0)-(1096,520) override=0
```

**当前判断**：视口拉伸的过渡帧中 `ViewRect` 扩大超出 RT 已渲染区域，Pass2 沿用
`ENoAction`（与引擎 `FSceneView::GetOverwriteLoadAction()` 非 VR 下的返回值一致，
`SceneView.h:2180`），未写入的外围区域显示上一帧残影或新分配区域的未初始化内容（黑边/彩条）。

**下一步**：插件已加 `PixelOutline extents:` 日志（尺寸变化时输出 source / outputTex /
outputRect / 是否有 override）。复现拉伸时把该日志发来即可判定：

- 拉伸中 source 与 outputTex **持续**不一致 → 分辨率同步问题，改为中间纹理合成再 blit；
- 仅**瞬间**不一致 → resize 瞬态，按"ViewRect ≠ RT 全区时改用 `EClear`"修复。

---

## 五、参数标定流程

Unity 的数值**不能直接照抄**（UE 深度已线性化、法线未归一化、AfterTonemapping 处颜色值域不同）。

1. **只留深度信号**：`r.PixelOutline.NormalWeight 0`（`ColorWeight` 默认已是 0，
   `UseLuminanceEdge` 默认已是 0）。
2. `r.PixelOutline.DebugMode 2`，观察全分辨率掩码：
   - 全黑 → 调小 `EdgeThreshold` 或调大 `DepthWeight`；
   - 全白 → 调大 `EdgeThreshold`。
3. 深度信号稳定后加入法线：`r.PixelOutline.NormalWeight 1`，按同样方式微调。
4. `r.PixelOutline.DebugMode 3` 确认块级掩码与像素块对齐。
5. `r.PixelOutline.DebugMode 0` 看最终效果，按需要调 `PixelSize`（块越大描边越粗）。

参考量级（本机实测，随场景而变）：默认权重下掩码覆盖 2.5%；`DepthWeight 20` 时 28.9%。

---

## 六、验收清单

- [ ] 工程编译通过、编辑器启动无渲染断言
- [ ] 展示关卡出现像素化画面 + 白色块状描边
- [ ] `DebugMode 2/3` 掩码与物体轮廓吻合
- [ ] 描边块位置、块大小、颜色与 Unity 参考截图一致（阶段 4 标定）
- [ ] `r.PixelOutline.Enable 0` 能完全关闭效果
- [ ] 原有关卡（`Content/Cloud/Map/L_Cloud_Base`）功能不受影响
- [ ] 遗留问题 1（闪烁）、问题 2（拉伸外围）按上文方案处理后复验

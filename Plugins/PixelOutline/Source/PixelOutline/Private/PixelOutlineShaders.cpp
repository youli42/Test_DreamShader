// Copyright Epic Games, Inc. All Rights Reserved.

#include "PixelOutlineShaders.h"
#include "PixelOutlineParameters.h"

#include "HAL/IConsoleManager.h"

IMPLEMENT_GLOBAL_SHADER(FPixelOutlinePixelAndDetectPS, "/Plugin/PixelOutline/PixelOutlinePixelAndDetect.usf", "PixelAndDetectPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FPixelOutlineCompositePS, "/Plugin/PixelOutline/PixelOutlineComposite.usf", "CompositePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FPixelOutlineReduceOutlinePS, "/Plugin/PixelOutline/PixelOutlineReduceOutline.usf", "ReduceOutlinePS", SF_Pixel);

// ---------------------------------------------------------------------------
// Console variables -- the runtime knobs of the effect.
// Defaults mirror the Unity ScreenPixel Renderer Feature panel values.
// ---------------------------------------------------------------------------

static TAutoConsoleVariable<int32> CVarPixelOutlineEnable(
	TEXT("r.PixelOutline.Enable"),
	1,
	TEXT("0: disabled, 1: enabled."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarPixelOutlinePixelSize(
	TEXT("r.PixelOutline.PixelSize"),
	13,
	TEXT("Pixel block size in screen pixels. Clamped to [1, 64]. Matches Unity _PixelSize."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPixelOutlineThickness(
	TEXT("r.PixelOutline.OutlineThickness"),
	1.0f,
	TEXT("Sobel sampling radius multiplier, in source pixels. Matches Unity _OutlineThickness."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPixelOutlineDepthWeight(
	TEXT("r.PixelOutline.DepthWeight"),
	1.5f,
	TEXT("Weight of the depth Sobel signal. Matches Unity _DepthWeight."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPixelOutlineNormalWeight(
	TEXT("r.PixelOutline.NormalWeight"),
	1.0f,
	TEXT("Weight of the world normal Sobel signal. Matches Unity _NormalWeight."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarPixelOutlineUseLuminance(
	TEXT("r.PixelOutline.UseLuminanceEdge"),
	0,
	TEXT("0: off, 1: enable the Rec.709 luminance edge signal."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPixelOutlineLuminanceWeight(
	TEXT("r.PixelOutline.LuminanceWeight"),
	1.0f,
	TEXT("Weight of the luminance Sobel signal."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPixelOutlineColorWeight(
	TEXT("r.PixelOutline.ColorWeight"),
	0.0f,
	TEXT("Weight of the RGB Sobel signal. 0 disables it."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPixelOutlineEdgeThreshold(
	TEXT("r.PixelOutline.EdgeThreshold"),
	0.08f,
	TEXT("Edge response threshold before the smoothstep. Unity value 0.08 needs re-calibration on UE."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPixelOutlineEdgeSoftness(
	TEXT("r.PixelOutline.EdgeSoftness"),
	0.04f,
	TEXT("Softness of the edge smoothstep. Unity value 0.04 needs re-calibration on UE."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPixelOutlineColorR(TEXT("r.PixelOutline.OutlineColor.R"), 1.0f, TEXT("Outline color red."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarPixelOutlineColorG(TEXT("r.PixelOutline.OutlineColor.G"), 1.0f, TEXT("Outline color green."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarPixelOutlineColorB(TEXT("r.PixelOutline.OutlineColor.B"), 1.0f, TEXT("Outline color blue."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarPixelOutlineColorA(TEXT("r.PixelOutline.OutlineColor.A"), 1.0f, TEXT("Outline color alpha."), ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarPixelOutlineDebugMode(
	TEXT("r.PixelOutline.DebugMode"),
	0,
	TEXT("0: full pipeline, 1: packed pixelated color, 2: full-res binary edge mask, 3: block grid mask."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarPixelOutlineNormalSource(
	TEXT("r.PixelOutline.NormalSource"),
	0,
	TEXT("0: world normal from GBufferA, 1: rebuild the normal from scene depth derivatives."),
	ECVF_RenderThreadSafe);

FPixelOutlineSettings GetPixelOutlineSettings()
{
	FPixelOutlineSettings Settings;
	Settings.bEnabled = CVarPixelOutlineEnable.GetValueOnAnyThread() != 0;
	Settings.PixelSize = CVarPixelOutlinePixelSize.GetValueOnAnyThread();
	Settings.OutlineThickness = CVarPixelOutlineThickness.GetValueOnAnyThread();
	Settings.DepthWeight = CVarPixelOutlineDepthWeight.GetValueOnAnyThread();
	Settings.NormalWeight = CVarPixelOutlineNormalWeight.GetValueOnAnyThread();
	Settings.bUseLuminanceEdge = CVarPixelOutlineUseLuminance.GetValueOnAnyThread();
	Settings.LuminanceWeight = CVarPixelOutlineLuminanceWeight.GetValueOnAnyThread();
	Settings.ColorWeight = CVarPixelOutlineColorWeight.GetValueOnAnyThread();
	Settings.EdgeThreshold = CVarPixelOutlineEdgeThreshold.GetValueOnAnyThread();
	Settings.EdgeSoftness = CVarPixelOutlineEdgeSoftness.GetValueOnAnyThread();
	Settings.OutlineColor = FVector4f(
		CVarPixelOutlineColorR.GetValueOnAnyThread(),
		CVarPixelOutlineColorG.GetValueOnAnyThread(),
		CVarPixelOutlineColorB.GetValueOnAnyThread(),
		CVarPixelOutlineColorA.GetValueOnAnyThread());
	Settings.DebugMode = CVarPixelOutlineDebugMode.GetValueOnAnyThread();
	Settings.NormalSource = CVarPixelOutlineNormalSource.GetValueOnAnyThread();
	return Settings;
}

FPixelOutlineCommonParameters GetPixelOutlineCommonParameters(
	const FPixelOutlineSettings& Settings,
	FIntPoint SourceExtent,
	float NearClip,
	float FarClip,
	bool bIsOrthographic)
{
	FPixelOutlineCommonParameters Parameters;

	const FIntPoint Source = FIntPoint(FMath::Max(SourceExtent.X, 1), FMath::Max(SourceExtent.Y, 1));
	const int32 BlockSize = FMath::Clamp(FMath::RoundToInt(static_cast<float>(Settings.PixelSize)), 1, 64);
	const FIntPoint BlockSize2D(BlockSize, BlockSize);
	const FIntPoint GridSize(
		FMath::Max(FMath::DivideAndRoundUp(Source.X, BlockSize), 1),
		FMath::Max(FMath::DivideAndRoundUp(Source.Y, BlockSize), 1));

	Parameters.SourceSizeAndInvSize = FVector4f(
		static_cast<float>(Source.X),
		static_cast<float>(Source.Y),
		1.0f / static_cast<float>(Source.X),
		1.0f / static_cast<float>(Source.Y));
	Parameters.GridSizeAndBlockSize = FVector4f(
		static_cast<float>(GridSize.X),
		static_cast<float>(GridSize.Y),
		static_cast<float>(BlockSize2D.X),
		static_cast<float>(BlockSize2D.Y));
	Parameters.EdgeParams = FVector4f(
		Settings.EdgeThreshold,
		FMath::Max(Settings.EdgeSoftness, 1e-5f),
		Settings.OutlineThickness,
		Settings.bUseLuminanceEdge != 0 ? 1.0f : 0.0f);
	Parameters.Weights = FVector4f(
		Settings.DepthWeight,
		Settings.NormalWeight,
		Settings.LuminanceWeight,
		Settings.ColorWeight);
	Parameters.OutlineColor = Settings.OutlineColor;
	Parameters.DepthParams = FVector4f(NearClip, FarClip, bIsOrthographic ? 1.0f : 0.0f, 0.0f);
	Parameters.DebugParams = FVector4f(
		static_cast<float>(Settings.DebugMode),
		static_cast<float>(Settings.NormalSource),
		0.0f,
		0.0f);

	return Parameters;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "PixelOutlineViewExtension.h"
#include "PixelOutlineShaders.h"
#include "PixelOutlineParameters.h"

#include "CommonRenderResources.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ScreenPass.h"

// Diagnostics for the "garbage borders when resizing the viewport" issue: log
// whenever the source (SceneColor) and output (override/backbuffer) extents
// disagree or change between frames.
namespace
{
	FIntPoint GLastSourceExtent = FIntPoint::ZeroValue;
	FIntPoint GLastOutputExtent = FIntPoint::ZeroValue;
	bool GbLastHadOverride = false;

	void LogPassExtentsIfNeeded(const FIntPoint& SourceExtent, const FScreenPassRenderTarget& Output, bool bHadOverride)
	{
		const FIntPoint OutputTextureExtent = Output.IsValid() ? Output.Texture->Desc.Extent : FIntPoint::ZeroValue;
		const FIntRect OutputRect = Output.IsValid() ? Output.ViewRect : FIntRect();

		if (SourceExtent == GLastSourceExtent &&
			OutputTextureExtent == GLastOutputExtent &&
			bHadOverride == GbLastHadOverride)
		{
			return;
		}

		UE_LOG(LogTemp, Warning,
			TEXT("PixelOutline extents: source=%dx%d outputTex=%dx%d outputRect=(%d,%d)-(%d,%d) override=%d"),
			SourceExtent.X, SourceExtent.Y,
			OutputTextureExtent.X, OutputTextureExtent.Y,
			OutputRect.Min.X, OutputRect.Min.Y, OutputRect.Max.X, OutputRect.Max.Y,
			bHadOverride ? 1 : 0);

		GLastSourceExtent = SourceExtent;
		GLastOutputExtent = OutputTextureExtent;
		GbLastHadOverride = bHadOverride;
	}
}

FPixelOutlineViewExtension::FPixelOutlineViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

void FPixelOutlineViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	// Tonemap == BL_SceneColorAfterTonemapping, the UE counterpart of Unity's
	// RenderPassEvent 600 (AfterRenderingPostProcessing).
	if (Pass != EPostProcessingPass::Tonemap)
	{
		return;
	}

	if (!GetPixelOutlineSettings().bEnabled)
	{
		return;
	}

	InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FPixelOutlineViewExtension::AddPixelOutlinePasses));
}

FScreenPassTexture FPixelOutlineViewExtension::AddPixelOutlinePasses(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	const FPixelOutlineSettings Settings = GetPixelOutlineSettings();

	const FScreenPassTextureSlice SceneColor = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
	if (SceneColor.TextureSRV == nullptr)
	{
		return FScreenPassTexture();
	}

	RDG_EVENT_SCOPE(GraphBuilder, "PixelOutline");

	const FIntRect SourceRect = SceneColor.ViewRect;
	const FIntPoint SourceExtent(FMath::Max(SourceRect.Width(), 1), FMath::Max(SourceRect.Height(), 1));

	const bool bIsOrthographic = !View.IsPerspectiveProjection();
	const FPixelOutlineCommonParameters Common = GetPixelOutlineCommonParameters(
		Settings,
		SourceExtent,
		View.NearClippingDistance,
		0.0f,
		bIsOrthographic);

	const FScreenPassTextureViewport InputViewport(SourceRect);

	FRHISamplerState* BilinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	FRHISamplerState* PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	// ---------------------------------------------------------------------
	// Pass 0: pixelate + Sobel + binarise, full resolution.
	// ---------------------------------------------------------------------
	FRDGTextureRef PackedTexture = nullptr;
	{
		const FRDGTextureDesc PackedDesc = FRDGTextureDesc::Create2D(
			SourceExtent,
			PF_A16B16G16R16,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);

		PackedTexture = GraphBuilder.CreateTexture(PackedDesc, TEXT("PixelOutline.PackedColorAndEdge"));

		const FScreenPassTexture Packed(PackedTexture, SourceRect);
		const FScreenPassTextureViewport PackedViewport(Packed);

		FPixelOutlinePixelAndDetectPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPixelOutlinePixelAndDetectPS::FParameters>();
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->Input = GetScreenPassTextureViewportParameters(InputViewport);
		PassParameters->InputTexture = SceneColor.TextureSRV;
		PassParameters->InputSampler = BilinearClampSampler;
		PassParameters->InputPointSampler = PointClampSampler;
		PassParameters->SceneTextures = Inputs.SceneTextures;
		PassParameters->Common = Common;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(PackedTexture, ERenderTargetLoadAction::ENoAction);

		TShaderMapRef<FPixelOutlinePixelAndDetectPS> PixelShader(GlobalShaderMap);

		AddDrawScreenPass(
			GraphBuilder,
			RDG_EVENT_NAME("PixelAndDetect"),
			View,
			PackedViewport,
			InputViewport,
			PixelShader,
			PassParameters);
	}

	// ---------------------------------------------------------------------
	// Pass 1: block max reduction into the low resolution grid.
	// Drawn one pixel per block, exactly like Unity's fullscreen blit reduction.
	// ---------------------------------------------------------------------
	const FIntPoint GridExtent(
		FMath::Max(FMath::RoundToInt(Common.GridSizeAndBlockSize.X), 1),
		FMath::Max(FMath::RoundToInt(Common.GridSizeAndBlockSize.Y), 1));

	FRDGTextureRef GridTexture = nullptr;
	{
		const FRDGTextureDesc GridDesc = FRDGTextureDesc::Create2D(
			GridExtent,
			PF_A16B16G16R16,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);

		GridTexture = GraphBuilder.CreateTexture(GridDesc, TEXT("PixelOutline.OutlineGrid"));

		const FScreenPassTexture Grid(GridTexture, FIntRect(FIntPoint::ZeroValue, GridExtent));
		const FScreenPassTextureViewport GridViewport(Grid);

		FPixelOutlineReduceOutlinePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPixelOutlineReduceOutlinePS::FParameters>();
		PassParameters->PackedTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PackedTexture));
		PassParameters->PackedPointSampler = PointClampSampler;
		PassParameters->Common = Common;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(GridTexture, ERenderTargetLoadAction::ENoAction);

		TShaderMapRef<FPixelOutlineReduceOutlinePS> PixelShader(GlobalShaderMap);

		AddDrawScreenPass(
			GraphBuilder,
			RDG_EVENT_NAME("ReduceOutline %dx%d -> %dx%d", SourceExtent.X, SourceExtent.Y, GridExtent.X, GridExtent.Y),
			View,
			GridViewport,
			GridViewport,
			PixelShader,
			PassParameters);
	}

	// ---------------------------------------------------------------------
	// Pass 2: composite by block index.
	// ---------------------------------------------------------------------
	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(
			GraphBuilder,
			FScreenPassTexture(SceneColor),
			ERenderTargetLoadAction::ENoAction,
			TEXT("PixelOutline.Output"));
	}

	{
		const FScreenPassTextureViewport OutputViewport(Output);

		FPixelOutlineCompositePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPixelOutlineCompositePS::FParameters>();
		PassParameters->Input = GetScreenPassTextureViewportParameters(InputViewport);
		PassParameters->GridTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(GridTexture));
		PassParameters->GridPointSampler = PointClampSampler;
		PassParameters->PackedTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PackedTexture));
		PassParameters->PackedPointSampler = PointClampSampler;
		PassParameters->Common = Common;
		PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();

		TShaderMapRef<FPixelOutlineCompositePS> PixelShader(GlobalShaderMap);

		AddDrawScreenPass(
			GraphBuilder,
			RDG_EVENT_NAME("Composite"),
			View,
			OutputViewport,
			InputViewport,
			PixelShader,
			PassParameters);
	}

	LogPassExtentsIfNeeded(SourceExtent, Output, Inputs.OverrideOutput.IsValid());

	return MoveTemp(Output);
}

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ShaderParameterMacros.h"

/**
 * CPU side mirror of the runtime tunable parameters.
 * Values come from the r.PixelOutline.* console variables and are packed into
 * the PixelOutlineCommon uniform buffer once per frame.
 */
struct FPixelOutlineSettings
{
	int32 bEnabled = 1;

	// Unity: _PixelSize, Range(1, 64), effective value 13.
	int32 PixelSize = 13;

	// Unity: _OutlineThickness, Range(0.5, 4), effective value 1.
	float OutlineThickness = 1.0f;

	float DepthWeight = 1.5f;
	float NormalWeight = 1.0f;

	int32 bUseLuminanceEdge = 0;
	float LuminanceWeight = 1.0f;
	float ColorWeight = 0.0f;

	float EdgeThreshold = 0.08f;
	float EdgeSoftness = 0.04f;

	FVector4f OutlineColor = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);

	// 0 = full pipeline, 1 = show packed color, 2 = show raw edge mask, 3 = show block grid.
	int32 DebugMode = 0;

	// 0 = GBufferA world normal, 1 = disable the normal signal.
	int32 NormalSource = 0;
};

/** Reads the console variables into FPixelOutlineSettings. */
FPixelOutlineSettings GetPixelOutlineSettings();

/**
 * Shader side parameter block shared by all three passes.
 *
 * Every member is a FVector4f on purpose. The block is declared by hand on the
 * HLSL side (Unreal does not emit declarations for loose parameters), and a
 * mixed int2/float4 layout drifted between the CPU packing and the HLSL cbuffer
 * rules -- int2 members were read back as zero. Uniform 16 byte members make the
 * two layouts match by construction.
 *
 * Every pass receives the *same* full-resolution source size so that the block
 * index math stays identical across the chain (Unity's _ScreenPixelSourceSize).
 */
BEGIN_SHADER_PARAMETER_STRUCT(FPixelOutlineCommonParameters, )
	// (W, H, 1/W, 1/H) of the full resolution source, identical in all passes.
	SHADER_PARAMETER(FVector4f, SourceSizeAndInvSize)
	// xy = ceil(W / bs) x ceil(H / bs), zw = block size on both axes.
	SHADER_PARAMETER(FVector4f, GridSizeAndBlockSize)
	// (EdgeThreshold, EdgeSoftness, OutlineThickness, UseLuminanceEdge)
	SHADER_PARAMETER(FVector4f, EdgeParams)
	// (DepthWeight, NormalWeight, LuminanceWeight, ColorWeight)
	SHADER_PARAMETER(FVector4f, Weights)
	// rgb + a, straight from Unity's _OutlineColor.
	SHADER_PARAMETER(FVector4f, OutlineColor)
	// (NearClip, FarClip, bIsOrthographic, unused)
	SHADER_PARAMETER(FVector4f, DepthParams)
	// (DebugMode, NormalSource, unused, unused)
	SHADER_PARAMETER(FVector4f, DebugParams)
END_SHADER_PARAMETER_STRUCT()

/** Packs FPixelOutlineSettings for a given source extent / view. */
FPixelOutlineCommonParameters GetPixelOutlineCommonParameters(
	const FPixelOutlineSettings& Settings,
	FIntPoint SourceExtent,
	float NearClip,
	float FarClip,
	bool bIsOrthographic);

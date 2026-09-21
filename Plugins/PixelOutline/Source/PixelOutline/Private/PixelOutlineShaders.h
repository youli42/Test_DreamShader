// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GlobalShader.h"
#include "ScreenPass.h"
#include "SceneTexturesConfig.h"
#include "SceneView.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "PixelOutlineParameters.h"

/**
 * Pass 0 - full resolution pixelation + Sobel edge detection.
 * RGB = pixelated scene color, A = binary edge mask.
 */
class FPixelOutlinePixelAndDetectPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPixelOutlinePixelAndDetectPS);
	SHADER_USE_PARAMETER_STRUCT(FPixelOutlinePixelAndDetectPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Input)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputPointSampler)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPixelOutlineCommonParameters, Common)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

/**
 * Pass 1 - reduce the full resolution binary mask into one value per pixel block.
 * Drawn into the low resolution grid: one pixel == one block; the reduction is a
 * max, never an average. A pixel shader is used instead of a compute pass because
 * Unity's original reduction is also a fullscreen blit, and the UAV write path
 * proved fragile under ClearUnusedGraphResources.
 */
class FPixelOutlineReduceOutlinePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPixelOutlineReduceOutlinePS);
	SHADER_USE_PARAMETER_STRUCT(FPixelOutlineReduceOutlinePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, PackedTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, PackedPointSampler)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPixelOutlineCommonParameters, Common)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

/**
 * Pass 2 - point sample the block grid by block index and lerp to the outline color.
 */
class FPixelOutlineCompositePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPixelOutlineCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FPixelOutlineCompositePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Input)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, GridTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, GridPointSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, PackedTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, PackedPointSampler)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPixelOutlineCommonParameters, Common)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

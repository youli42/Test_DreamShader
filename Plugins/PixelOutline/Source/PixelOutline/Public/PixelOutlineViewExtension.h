// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "SceneViewExtension.h"

/**
 * Inserts the three pass pixel outline chain right after tone mapping, which is
 * the UE counterpart of Unity's RenderPassEvent 600 / AfterRenderingPostProcessing.
 */
class FPixelOutlineViewExtension : public FSceneViewExtensionBase
{
public:
	FPixelOutlineViewExtension(const FAutoRegister& AutoRegister);

	// ISceneViewExtension
	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass Pass,
		const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;

private:
	FScreenPassTexture AddPixelOutlinePasses(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
};

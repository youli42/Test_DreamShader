// Copyright Epic Games, Inc. All Rights Reserved.

#include "PixelOutlineModule.h"
#include "PixelOutlineViewExtension.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

IMPLEMENT_MODULE(FPixelOutlineModule, PixelOutline)

void FPixelOutlineModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PixelOutline"));
	if (Plugin.IsValid())
	{
		const FString ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/PixelOutline"), ShaderDirectory);
	}

	// The module itself must be loaded at PostConfigInit so that the global shader
	// types register before InitializeShaderTypes(). Creating the view extension is
	// deferred to post engine init, when the scene rendering side is ready.
	FCoreDelegates::OnPostEngineInit.AddLambda([this]()
	{
		ViewExtension = FSceneViewExtensions::NewExtension<FPixelOutlineViewExtension>();
	});
}

void FPixelOutlineModule::ShutdownModule()
{
	ViewExtension.Reset();
}

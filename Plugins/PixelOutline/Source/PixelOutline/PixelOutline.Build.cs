// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class PixelOutline : ModuleRules
{
	public PixelOutline(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"Engine",
			"RenderCore",
			"Renderer",
			"RHI",
			"RHICore",
			"Projects"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject"
		});
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class TiUE : ModuleRules
{
	public TiUE(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"TiUE",
			"TiUE/Variant_Platforming",
			"TiUE/Variant_Platforming/Animation",
			"TiUE/Variant_Combat",
			"TiUE/Variant_Combat/AI",
			"TiUE/Variant_Combat/Animation",
			"TiUE/Variant_Combat/Gameplay",
			"TiUE/Variant_Combat/Interfaces",
			"TiUE/Variant_Combat/UI",
			"TiUE/Variant_SideScrolling",
			"TiUE/Variant_SideScrolling/AI",
			"TiUE/Variant_SideScrolling/Gameplay",
			"TiUE/Variant_SideScrolling/Interfaces",
			"TiUE/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}

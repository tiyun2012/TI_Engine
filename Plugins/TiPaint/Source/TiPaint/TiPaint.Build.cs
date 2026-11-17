using UnrealBuildTool;

public class TiPaint : ModuleRules
{
    public TiPaint(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd", "Slate", "SlateCore",
            "InputCore", "ToolMenus", "DesktopPlatform", "ImageWrapper"
        });
    }
}

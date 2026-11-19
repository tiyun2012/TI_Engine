using UnrealBuildTool;

public class NGon : ModuleRules
{
    public NGon(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "ApplicationCore",
            "CoreUObject",
            "Engine",
            "UnrealEd",
            "Slate",
            "SlateCore",
            "InputCore",
            "ToolMenus",
            "PropertyEditor",
            "MaterialEditor",
            "AssetRegistry",
            "DesktopPlatform",
            "Projects",
            "GeometryCore",
            "GeometryFramework",
            "MeshDescription",
            "StaticMeshDescription",
            "MeshConversion",
            "InteractiveToolsFramework",
            "ModelingComponents",
            "ImageWrapper",
            "Json",
            "JsonUtilities"
        });

        AddEngineThirdPartyPrivateStaticDependencies(Target, "FBX");
    }
}

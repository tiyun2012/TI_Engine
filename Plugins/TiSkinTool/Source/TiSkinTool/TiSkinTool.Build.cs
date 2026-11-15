using System.IO; 
using UnrealBuildTool;

public class TiSkinTool : ModuleRules
{
    public TiSkinTool(ReadOnlyTargetRules Target) : base(Target)
    {
        // Force the compiler to see the EditorOnly headers
        string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
        PublicIncludePaths.Add(Path.Combine(EngineDir, "Plugins/Runtime/MeshModelingToolset/Source/MeshModelingToolsEditorOnly/Public"));
        
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "UnrealEd", 
            "InputCore",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "InteractiveToolsFramework",
            "EditorInteractiveToolsFramework",
            "SkeletalMeshEditor",
            "SkeletalMeshUtilitiesCommon",
            "ModelingComponents",
            "MeshModelingToolsExp",        // <-- Added BACK for UDynamicMeshBrushTool
            "MeshModelingToolsEditorOnly"  // <-- Kept for USkinWeightsPaintTool
        });
    }
}
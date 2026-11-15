#include "TiSkinWeightTool.h"

DEFINE_LOG_CATEGORY(LogTiSkinTool);

void UTiSkinWeightTool::Setup()
{
    Super::Setup();

    OnSelectionChanged.AddUObject(this, &UTiSkinWeightTool::HandleSelectionChanged);
    OnWeightsChanged.AddUObject(this, &UTiSkinWeightTool::HandleWeightsChanged);

    UE_LOG(LogTiSkinTool, Display, TEXT("=== TiSkinWeightTool Successfully Activated ==="));
    DebugDumpState();
}

// REMOVED: UTiSkinWeightTool::Shutdown implementation

void UTiSkinWeightTool::HandleSelectionChanged()
{
    UE_LOG(LogTiSkinTool, Display, TEXT("Selection Changed Event Fired."));
    DebugDumpState();
}

void UTiSkinWeightTool::HandleWeightsChanged()
{
    UE_LOG(LogTiSkinTool, Display, TEXT("Weights Changed Event Fired."));
}

void UTiSkinWeightTool::DebugDumpState()
{
    const int32 LOD = static_cast<int32>(GetEditingLOD());
    const int32 ActiveBoneIndex = GetCurrentBoneIndex();
    const TSet<int32>& LockedBones = GetFinalLockedBoneIndices();
    const auto& ToolWeights = GetWeights(); 
    
    FDynamicMesh3& Mesh = GetMesh();

    UE_LOG(LogTiSkinTool, Display, TEXT("--- Tool State ---"));
    UE_LOG(LogTiSkinTool, Display, TEXT("Target LOD: %d"), LOD);
    UE_LOG(LogTiSkinTool, Display, TEXT("Selected Bone Index: %d"), ActiveBoneIndex);
    UE_LOG(LogTiSkinTool, Display, TEXT("Locked Bones Count: %d"), LockedBones.Num());
    UE_LOG(LogTiSkinTool, Display, TEXT("Dynamic Mesh Vertices: %d"), Mesh.VertexCount());
    UE_LOG(LogTiSkinTool, Display, TEXT("Dynamic Mesh Triangles: %d"), Mesh.TriangleCount());
    UE_LOG(LogTiSkinTool, Display, TEXT("Weight Vertex Entries: %d"), ToolWeights.CurrentWeights.Num());

    if (ActiveBoneIndex >= 0)
    {
        UE_LOG(LogTiSkinTool, Display, TEXT("Selected Bone Name: %s"), *GetBoneNameFromIndex(ActiveBoneIndex).ToString());
    }
}

void UTiSkinWeightTool::DebugDumpVertex(int32 VertexIndex)
{
    const auto& AllWeights = GetWeights().CurrentWeights;

    if (!AllWeights.IsValidIndex(VertexIndex))
    {
        UE_LOG(LogTiSkinTool, Warning, TEXT("Invalid weight vertex index: %d"), VertexIndex);
        return;
    }

    const auto& VertexWeights = AllWeights[VertexIndex];
    float Sum = 0.0f;

    // FIXED: Using 'auto' forces C++ to deduce the exact underlying Epic struct type
    for (const auto& Influence : VertexWeights)
    {
        Sum += Influence.Weight;
        UE_LOG(LogTiSkinTool, Display, TEXT("Vertex %d: Bone=%d (%s), Weight=%.6f"), 
            VertexIndex, 
            Influence.BoneID, 
            *GetBoneNameFromIndex(Influence.BoneID).ToString(), 
            Influence.Weight);
    }

    UE_LOG(LogTiSkinTool, Display, TEXT("Vertex %d Weight Sum = %.8f"), VertexIndex, Sum);
}

void UTiSkinWeightTool::Shutdown(EToolShutdownType ShutdownType)
{
    OnSelectionChanged.RemoveAll(this);
    OnWeightsChanged.RemoveAll(this);

    UE_LOG(LogTiSkinTool, Display, TEXT("=== TiSkinWeightTool Shutting Down ==="));

    // THE FIX: Explicitly bypass the unexported UDynamicMeshBrushTool
    // by calling the fully exported grandparent class from InteractiveToolsFramework.
    UMeshSurfacePointTool::Shutdown(ShutdownType);
}
#pragma once
#include "CoreMinimal.h"
#include "SkeletalMesh/SkinWeightsPaintTool.h"
#include "TiSkinWeightTool.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogTiSkinTool, Log, All);

UCLASS()
class UTiSkinWeightTool : public USkinWeightsPaintTool
{
    GENERATED_BODY()
public:
    virtual void Setup() override;
    
    // ADD THIS BACK
    virtual void Shutdown(EToolShutdownType ShutdownType) override;

    void DebugDumpState();
    void DebugDumpVertex(int32 VertexIndex);

private:
    void HandleSelectionChanged();
    void HandleWeightsChanged();
};
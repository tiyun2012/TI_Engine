#pragma once
#include "CoreMinimal.h"
#include "SkeletalMesh/SkinWeightsPaintTool.h"
#include "TiSkinWeightToolBuilder.generated.h"

UCLASS()
class UTiSkinWeightToolBuilder : public USkinWeightsPaintToolBuilder
{
    GENERATED_BODY()
public:
    // Only keep CreateNewTool. We deleted CanBuildTool!
    virtual UMeshSurfacePointTool* CreateNewTool(const FToolBuilderState& SceneState) const override;
};
#include "TiSkinWeightToolBuilder.h"
#include "TiSkinWeightTool.h"
#include "InteractiveToolManager.h" 

UMeshSurfacePointTool* UTiSkinWeightToolBuilder::CreateNewTool(const FToolBuilderState& SceneState) const
{
    return NewObject<UTiSkinWeightTool>(SceneState.ToolManager);
}
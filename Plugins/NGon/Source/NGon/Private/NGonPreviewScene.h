#pragma once

#include "CoreMinimal.h"
#include "NGonPicking.h"
#include "NGonSoftSelection.h"
#include "NGonTopology.h"
#include "PreviewScene.h"
#include "UObject/StrongObjectPtr.h"

class UDynamicMeshComponent;
class UInteractiveToolManager;
class ULineSetComponent;
class UPointSetComponent;
class UPreviewGeometry;
class UInteractiveGizmoManager;
class UCombinedTransformGizmo;
class UTransformProxy;
class IToolContextTransactionProvider;

class FNGonPreviewScene
{
public:
    FNGonPreviewScene();
    ~FNGonPreviewScene();

    FPreviewScene& GetPreviewScene() { return Scene; }
    const FBox& GetRestBounds() const { return Topology.RestBounds; }
    FBox GetSelectionOrRestBounds() const;

    void SetMesh(FNGonMeshData&& Data);
    void SetOverlayMaterials(UInteractiveToolManager* ToolManager);
    void SetShowEdges(bool bShow);
    void SetShowVertices(bool bShow);
    void SetHeatmapVisible(bool bShow);
    void SetHeatBias(float Value);
    void SetBend(float Value);
    void SetupGizmo(UInteractiveGizmoManager* GizmoManager, IToolContextTransactionProvider* TransactionProvider);
    void TeardownGizmo();
    void UpdateGizmo();
    void SetSoftSelection(const FNGonSoftSelectionSettings& Settings);
    void SetSoftTransformMode(ENGonSoftTransformMode Mode) { SoftTransformMode = Mode; }
    ENGonSoftTransformMode GetSoftTransformMode() const { return SoftTransformMode; }
    const FNGonSoftSelectionSettings& GetSoftSelectionSettings() const { return SoftSettings; }
    const TArray<FNGonVertexInfluence>& GetSoftSelectionInfluences() const { return SoftInfluences; }
    bool BeginSoftTransform(const FTransform& GizmoTransform, ENGonSoftTransformMode Mode);
    bool ApplyGizmoTransform(const FTransform& GizmoTransform);
    void EndSoftTransform();
    void CancelSoftTransform();
    bool IsSoftTransformActive() const { return SoftSession.IsActive(); }
    FTransform GetSelectionGizmoTransform() const;
    int32 Pick(ENGonElement Element, const FSceneView& View, const FVector2D& Cursor, bool bAdd);
    void BoxSelect(ENGonElement Element, const FSceneView& View, const FBox2D& ScreenRect, bool bAdd, bool bFullyContained);
    bool SelectEdgeLoop(int32 StartEdgeId, bool bAdd);
    void ClearSelection();
    FText DescribeSelection() const;
    FText DescribeMesh() const;
    FText DescribeUpdates() const;
    const FNGonTopology& GetTopology() const { return Topology; }

private:
    struct FColorElement
    {
        int32 OverlayId;
        int32 LogicalVertex;
        int32 Face;
    };

    void InitializeColorOverlay(UE::Geometry::FDynamicMesh3& Mesh);
    void WriteColors(UE::Geometry::FDynamicMesh3& Mesh) const;
    void UpdateColors();
    void RebuildOverlays();
    void UpdateOverlayPositions();
    void UpdateSelection(ENGonElement Element, int32 Id, bool bAdd);
    void ApplySelectionSet(ENGonElement Element, const TSet<int32>& Ids, bool bAdd);
    void DrawSelection();
    void DrawSoftSelection();
    void RebuildSoftSelection();

    FPreviewScene Scene;
    TStrongObjectPtr<UPreviewGeometry> Overlay;
    UDynamicMeshComponent* Surface = nullptr;
    ULineSetComponent* EdgeLines = nullptr;
    UPointSetComponent* VertexPoints = nullptr;
    UPointSetComponent* SoftInfluencePoints = nullptr;
    ULineSetComponent* SelectedEdge = nullptr;
    UPointSetComponent* SelectedVertex = nullptr;

    FNGonTopology Topology;
    FNGonPicker Picker;
    ENGonElement SelectedElement = ENGonElement::Face;
    TSet<int32> SelectedIds;
    TArray<FVector> Positions;
    TArray<float> AxisFractions;
    TArray<float> HeatSamples;
    TArray<FColorElement> ColorElements;
    int32 ImportedSampleCount = 0;
    bool bHeatmapVisible = false;
    float HeatBias = 0.0f;
    float Bend = 0.0f;
    int32 TopologyUpdates = 0;
    int32 PositionUpdates = 0;
    int32 ColorUpdates = 0;
    FNGonSoftSelectionSettings SoftSettings{};
    ENGonSoftTransformMode SoftTransformMode = ENGonSoftTransformMode::Fixed;
    FNGonSoftSelectionSolver SoftSolver;
    FNGonSoftTransformSession SoftSession;
    TArray<FNGonVertexInfluence> SoftInfluences;
    TArray<FVector> SoftOriginalPositions;
    FTransform ActiveGizmoTransform = FTransform::Identity;
    UInteractiveGizmoManager* GizmoManager = nullptr;
    UCombinedTransformGizmo* TransformGizmo = nullptr;
    UTransformProxy* TransformProxy = nullptr;
    bool bUpdatingGizmo = false;
};

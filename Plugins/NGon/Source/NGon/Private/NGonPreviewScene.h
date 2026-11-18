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
class UTexture2D;
class IToolContextTransactionProvider;

/** Coordinate frame used by the component gizmo. WeightedAverage matches Maya's
 * multi-component behavior; ActiveElement uses the most recently picked item. */
enum class ENGonGizmoFramePolicy : uint8
{
    WeightedAverage,
    ActiveElement
};

/** High-level W/E/R transform tool. UE owns gizmo drawing and hit testing. */
enum class ENGonGizmoMode : uint8
{
    Combined,
    Translate,
    Rotate,
    Scale,
    None
};

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
    /** Apply the TiPaint composite to the preview surface using each corner's UV. */
    void SetPaintTexture(const TArray<FColor>& Pixels, int32 Width, int32 Height);
    void ClearPaintTexture();
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
    bool HasSoftSelectionInfluence() const { return SoftSettings.bEnabled && !SelectedIds.IsEmpty() && !SoftInfluences.IsEmpty(); }
    bool BeginSoftTransform(const FTransform& GizmoTransform, ENGonSoftTransformMode Mode);
    bool ApplyGizmoTransform(const FTransform& GizmoTransform);
    void EndSoftTransform();
    void CancelSoftTransform();
    bool IsSoftTransformActive() const { return SoftSession.IsActive(); }
    FTransform GetSelectionGizmoTransform() const;
    void SetGizmoFramePolicy(ENGonGizmoFramePolicy InPolicy) { GizmoFramePolicy = InPolicy; UpdateGizmo(); }
    ENGonGizmoFramePolicy GetGizmoFramePolicy() const { return GizmoFramePolicy; }
    void SetGizmoMode(ENGonGizmoMode InMode) { GizmoMode = InMode; UpdateGizmo(); }
    ENGonGizmoMode GetGizmoMode() const { return GizmoMode; }
    int32 Pick(ENGonElement Element, const FSceneView& View, const FVector2D& Cursor, bool bAdd);
    void BoxSelect(ENGonElement Element, const FSceneView& View, const FBox2D& ScreenRect, bool bAdd, bool bFullyContained);
    bool SelectEdgeLoop(int32 StartEdgeId, bool bAdd);
    void ClearSelection();
    bool DeleteSelectedFaces();
    void SelectElement(ENGonElement Element, int32 Id, bool bAdd = false) { UpdateSelection(Element, Id, bAdd); }
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
        FVector2f UV = FVector2f::ZeroVector;
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
    // Build a dense logical-vertex buffer from the sparse solver result. The
    // surface material consumes this buffer so its ramp matches the TiGUI
    // influence display instead of imported FBX vertex colors.
    void RebuildSoftHeatSamples();
    void RebuildGizmoAdjacency();
    FTransform BuildComponentGizmoTransform() const;

    FPreviewScene Scene;
    TStrongObjectPtr<UPreviewGeometry> Overlay;
    TStrongObjectPtr<UTexture2D> PaintTexture;
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
    int32 ActiveSelectedId = INDEX_NONE;
    TArray<FVector> Positions;
    TMap<int32, TArray<int32>> GizmoVertexFaces;
    TMap<FIntPoint, TArray<int32>> GizmoEdgeFaces;
    TArray<float> AxisFractions;
    TArray<float> HeatSamples;
    TBitArray<> ImportedHeatMask;
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
    TArray<float> SoftHeatSamples;
    TArray<FColor> PaintPixels;
    int32 PaintWidth = 0;
    int32 PaintHeight = 0;
    bool bPaintVisible = false;
    TArray<FVector> SoftOriginalPositions;
    FTransform ActiveGizmoTransform = FTransform::Identity;
    ENGonGizmoFramePolicy GizmoFramePolicy = ENGonGizmoFramePolicy::WeightedAverage;
    ENGonGizmoMode GizmoMode = ENGonGizmoMode::Combined;
    UInteractiveGizmoManager* GizmoManager = nullptr;
    UCombinedTransformGizmo* TransformGizmo = nullptr;
    UTransformProxy* TransformProxy = nullptr;
    bool bUpdatingGizmo = false;
};

#pragma once

#include "PreviewScene.h"
#include "CoreMinimal.h"
#include "NGonPicking.h"
#include "NGonSoftSelection.h"
#include "NGonTopology.h"
#include "NGonViewportGizmo.h"
#include "UObject/StrongObjectPtr.h"

class UDynamicMeshComponent;
class UInteractiveToolManager;
class ULineSetComponent;
class UPointSetComponent;
class UPreviewGeometry;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;

/** Coordinate frame used by the component gizmo. WeightedAverage matches Maya's
 * multi-component behavior; ActiveElement uses the most recently picked item. */
enum class ENGonGizmoFramePolicy : uint8
{
    WeightedAverage,
    ActiveElement
};

class FNGonPreviewScene
{
public:
    FNGonPreviewScene();
    ~FNGonPreviewScene();

    FPreviewScene& GetPreviewScene() { return Scene; }
    const FBox& GetRestBounds() const { return Topology.RestBounds; }
    FBox GetMeshBounds() const;
    FBox GetSelectionOrMeshBounds() const;

    void SetMesh(FNGonMeshData&& Data, TArray<UMaterialInterface*> Materials = {});
    void SetPreviewMaterials(UInteractiveToolManager* ToolManager);
    void SetShowEdges(bool bShow);
    void SetShowVertices(bool bShow);
    void SetHeatmapVisible(bool bShow);
    void SetFlatShading(bool bEnable);
    /** Apply the TiPaint composite to the preview surface using each corner's UV. */
    void SetPaintTexture(const TArray<FColor>& Pixels, int32 Width, int32 Height);
    void ClearPaintTexture();
    void RefreshPaintMaterial();
    void SetHeatBias(float Value);
    void SetBend(float Value);
    void RefreshBoneDeformation();
    void UpdateCameraVertexScales(const FVector& CameraOrigin, const FVector& CameraDir);
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
    FTransform GetGizmoDisplayTransform() const;
    void SetGizmoFramePolicy(ENGonGizmoFramePolicy InPolicy) { GizmoFramePolicy = InPolicy; }
    ENGonGizmoFramePolicy GetGizmoFramePolicy() const { return GizmoFramePolicy; }
    void SetGizmoMode(ENGonGizmoMode InMode) { GizmoMode = InMode; }
    ENGonGizmoMode GetGizmoMode() const { return GizmoMode; }
    bool HasGizmoTarget() const;
    int32 Pick(ENGonElement Element, const FSceneView& View, const FVector2D& Cursor, bool bAdd);
    int32 PickHoverVertex(const FSceneView& View, const FVector2D& Cursor, bool bAllowThroughMesh) const;
    void BoxSelect(ENGonElement Element, const FSceneView& View, const FBox2D& ScreenRect, bool bAdd, bool bFullyContained);
    bool SelectEdgeLoop(int32 StartEdgeId, bool bAdd);
    void ClearSelection();
    bool DeleteSelectedFaces();
    void SelectElement(ENGonElement Element, int32 Id, bool bAdd = false) { UpdateSelection(Element, Id, bAdd); }
    FText DescribeSelection() const;
    FText DescribeMesh() const;
    FText DescribeUpdates() const;
    const FNGonTopology& GetTopology() const { return Topology; }
    const TArray<FVector>& GetPositions() const { return Positions; }
    ENGonElement GetSelectedElement() const { return SelectedElement; }
    const TSet<int32>& GetSelectedIds() const { return SelectedIds; }
    void SetSelectionSet(ENGonElement Element, const TSet<int32>& Ids, bool bAdd = false) { ApplySelectionSet(Element, Ids, bAdd); }
    bool ReplacePolygonMesh(const TArray<FVector>& NewPositions, const TArray<TArray<int32>>& NewFaces, FText& OutError);

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FNGonPreviewMaterialTest;
#endif

    struct FColorElement
    {
        int32 OverlayId;
        int32 LogicalVertex;
        FVector4f SourceColor;
        FVector2f UV;
    };

    enum class ESurfaceDisplay : uint8 { Source, Heatmap, PaintColors, PaintTexture };

    void InitializeColorOverlay(UE::Geometry::FDynamicMesh3& Mesh);
    FVector4f GetOverlayColor(const FColorElement& Element) const;
    void WriteColors(UE::Geometry::FDynamicMesh3& Mesh) const;
    void UpdateSurfaceDisplay(bool bColorsChanged);
    bool IsPaintMaterialReady();
    void UpdateFaceHighlight();
    void NotifySurfacePositionsChanged(UE::Geometry::FDynamicMesh3& Mesh);
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
    TStrongObjectPtr<UMaterialInterface> StudioMaterial;
    TStrongObjectPtr<UMaterialInterface> ColorMaterial;
    TStrongObjectPtr<UMaterialInstanceDynamic> PaintMaterial;
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
    bool bSecondaryFaceHighlight = false;
    bool bFlatShading = true;
    bool bSmoothNormalsDirty = false;
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
    ESurfaceDisplay SurfaceDisplay = ESurfaceDisplay::Source;
    FTransform ActiveGizmoTransform = FTransform::Identity;
    ENGonGizmoFramePolicy GizmoFramePolicy = ENGonGizmoFramePolicy::WeightedAverage;
    ENGonGizmoMode GizmoMode = ENGonGizmoMode::Translate;
};

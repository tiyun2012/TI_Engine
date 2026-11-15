#pragma once

#include "CoreMinimal.h"
#include "NGonPicking.h"
#include "NGonTopology.h"
#include "PreviewScene.h"
#include "UObject/StrongObjectPtr.h"

class UDynamicMeshComponent;
class UInteractiveToolManager;
class ULineSetComponent;
class UPointSetComponent;
class UPreviewGeometry;

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
    void Pick(ENGonElement Element, const FSceneView& View, const FVector2D& Cursor, bool bAdd);
    void ClearSelection();
    FText DescribeSelection() const;
    FText DescribeMesh() const;
    FText DescribeUpdates() const;

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
    void DrawSelection();

    FPreviewScene Scene;
    TStrongObjectPtr<UPreviewGeometry> Overlay;
    UDynamicMeshComponent* Surface = nullptr;
    ULineSetComponent* EdgeLines = nullptr;
    UPointSetComponent* VertexPoints = nullptr;
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
};

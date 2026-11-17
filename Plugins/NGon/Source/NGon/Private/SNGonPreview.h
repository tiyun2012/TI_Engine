#pragma once

#include "CoreMinimal.h"
#include "NGonPicking.h"
#include "NGonSoftSelection.h"
#include "SEditorViewport.h"
#include "Widgets/SCompoundWidget.h"

struct FAssetData;
class FNGonPreviewScene;
struct FNGonMeshData;

class SNGonViewport final : public SEditorViewport
{
public:
    SLATE_BEGIN_ARGS(SNGonViewport) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    void SetSelectionMode(ENGonElement Element);
    ENGonElement GetSelectionMode() const { return SelectionMode; }
    void ToggleHeatmap();
    void SetHeatmapVisible(bool bVisible);
    bool IsHeatmapVisible() const;
    void SetMesh(FNGonMeshData&& Data);
    void FrameMesh();
    bool DeleteSelectedFaces();
    int32 Pick(const FSceneView& View, const FVector2D& Cursor, bool bAdd);
    void BoxSelect(const FSceneView& View, const FBox2D& ScreenRect, bool bAdd, bool bFullyContained);
    bool SelectEdgeLoop(int32 StartEdgeId, bool bAdd);
    FNGonPreviewScene& Preview() { return *PreviewScene; }

protected:
    TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
    void OnFocusViewportToSelection() override;

private:
    TSharedPtr<FNGonPreviewScene> PreviewScene;
    ENGonElement SelectionMode = ENGonElement::Face;
    bool bHeatmapVisible = false;
};

class SNGonPreview final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SNGonPreview) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    FReply LoadDemo();
    FReply ReloadDefault();
    FReply OpenFbx();
    bool TryLoadDefault(FText& Error);
    bool TryLoadFbx(const FString& Filename, FText& Error);
    void LoadStaticMesh(const FAssetData& Asset);
    void ShowMesh(FNGonMeshData&& Data, const FText& SourceName);
    void ShowError(const FText& Error);
    void SetSelectionMode(ENGonElement Element);
    void ApplySoftSelectionSettings();

    TSharedRef<SWidget> MakeSelectionMode(ENGonElement Element, const FText& Label);
    TSharedRef<SWidget> MakeControls();
    TSharedRef<SWidget> MakeSliders();
    TSharedRef<SWidget> MakeStatus();
    TSharedRef<SWidget> MakeSoftSelectionControls();

    TSharedPtr<SNGonViewport> Viewport;
    FString AssetPath;
    FText SourceLabel;
    FText ErrorText;
    ENGonElement SelectionMode = ENGonElement::Face;
    bool bShowHeatmap = false;
    float HeatBias = 0.0f;
    float Bend = 0.0f;
    bool bSoftSelection = false;
    float SoftRadius = 100.0f;
    ENGonSoftTransformMode SoftMode = ENGonSoftTransformMode::Fixed;
    ENGonSoftMetric SoftMetric = ENGonSoftMetric::EdgeSurface;
    ENGonSoftConnectivity SoftConnectivity = ENGonSoftConnectivity::SameIsland;
    float SoftSurfaceBlend = 0.5f;
};

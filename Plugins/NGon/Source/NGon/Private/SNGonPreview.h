#pragma once

#include "CoreMinimal.h"
#include "NGonPicking.h"
#include "NGonPreviewScene.h"
#include "NGonSoftSelection.h"
#include "SEditorViewport.h"
#include "Widgets/SCompoundWidget.h"

struct FAssetData;
struct FNGonMeshData;
class UMaterialInterface;
class SVerticalBox;
class STiPaintEditor;
class FNGonUndoSession;
class SNGonUndoTreePanel;

enum class ENGonViewportShading : uint8
{
    Smooth,
    Faceted,
    Wireframe
};

class SNGonViewport final : public SEditorViewport
{
public:
    SLATE_BEGIN_ARGS(SNGonViewport) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    void SetSelectionMode(ENGonElement Element);
    void ToggleSelectionFlag(ENGonSelectionMask Flag);
    void SetSelectionMask(ENGonSelectionMask Mask);
    ENGonSelectionMask GetSelectionMask() const { return SelectionMask; }
    bool IsSelectionFlagEnabled(ENGonSelectionMask Flag) const { return EnumHasAnyFlags(SelectionMask, Flag); }
    bool IsSelectionFrozen() const { return SelectionMask == ENGonSelectionMask::None; }
    void SetGizmoMode(ENGonGizmoMode Mode);
    void SetShadingMode(ENGonViewportShading Mode);
    ENGonGizmoMode GetGizmoMode() const;
    ENGonViewportShading GetShadingMode() const { return ShadingMode; }
    ENGonElement GetSelectionMode() const { return SelectionMode; }
    void ToggleHeatmap();
    void SetHeatmapVisible(bool bVisible);
    bool IsHeatmapVisible() const;
    void SetGridVisible(bool bVisible);
    bool IsGridVisible() const { return bGridVisible; }
    void SetMesh(FNGonMeshData&& Data, TArray<UMaterialInterface*> Materials = {});
    void FrameMesh();
    void FocusViewport();
    bool DeleteSelectedFaces();
    void SetPaintTexture(const TArray<FColor>& Pixels, int32 Width, int32 Height) { PreviewScene->SetPaintTexture(Pixels, Width, Height); }
    void SetPaint3DMode(bool bEnabled);
    bool IsPaint3DMode() const { return bPaint3DMode; }
    void SetPaint3DCallbacks(TFunction<void(const FNGonSurfaceHit&, bool)> InOnPoint, TFunction<void()> InOnEnd);
    void DispatchPaint3DPoint(const FNGonSurfaceHit& Hit, bool bFirstPoint);
    void DispatchPaint3DEnd();
    int32 Pick(const FSceneView& View, const FVector2D& Cursor, bool bAdd, ENGonElement* OutPickedElement = nullptr);
    void BoxSelect(const FSceneView& View, const FBox2D& ScreenRect, bool bAdd, bool bFullyContained);
    bool SelectEdgeLoop(int32 StartEdgeId, bool bAdd);
    FNGonPreviewScene& Preview() { return *PreviewScene; }
    const FNGonPreviewScene& Preview() const { return *PreviewScene; }

protected:
    TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
    void OnFocusViewportToSelection() override;

private:
    TSharedPtr<FNGonPreviewScene> PreviewScene;
    ENGonElement SelectionMode = ENGonElement::Face;
    ENGonSelectionMask SelectionMask = ENGonSelectionMask::Face;
    ENGonViewportShading ShadingMode = ENGonViewportShading::Faceted;
    bool bHeatmapVisible = false;
    bool bGridVisible = false;
    bool bPaint3DMode = false;
    TFunction<void(const FNGonSurfaceHit&, bool)> OnPaint3DPoint;
    TFunction<void()> OnPaint3DEnd;
};

enum class ENGonToolShelfTab : uint8
{
    Select,
    Model,
    Sculpt,
    Deform,
    Skin,
    AIStudio,
    Display
};

enum class ENGonRightUpperTab : uint8
{
    MeshStudio,
    SkeletonTree,
    MorphTargets,
    Topology
};

enum class ENGonRightLowerTab : uint8
{
    AIConsole,
    Details,
    AssetDetails,
    PreviewSettings,
    Painter,
    History
};

struct FNGonBoneEntry
{
    int32 BoneIndex = INDEX_NONE;
    int32 ParentIndex = INDEX_NONE;
    int32 Depth = 0;
    int32 ChildCount = 0;
    FName Name = NAME_None;
    FTransform LocalTransform = FTransform::Identity;
    FTransform ComponentTransform = FTransform::Identity;
};

struct FNGonMorphEntry
{
    FName Name = NAME_None;
    int32 VertexDeltaCount = 0;
};

class SNGonPreview final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SNGonPreview) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    ~SNGonPreview() override;

private:
    FReply LoadDemo();
    FReply ReloadDefault();
    FReply OpenFbx();
    bool TryLoadDefault(FText& Error);
    bool TryLoadFbx(const FString& Filename, FText& Error);
    void LoadStaticMesh(const FAssetData& Asset);
    void LoadSkeletalMesh(const FAssetData& Asset);
    void ShowMesh(FNGonMeshData&& Data, const FText& SourceName, const FText& InSourceType,
        ENGonViewportShading Shading = ENGonViewportShading::Faceted, TArray<UMaterialInterface*> Materials = {});
    void ShowError(const FText& Error);
    void SetSelectionMode(ENGonElement Element);
    void ToggleSelectionMode(ENGonElement Element);
    void SetActiveToolTab(ENGonToolShelfTab Tab);
    void ApplySoftSelectionSettings();
    void RebuildSkeletonTreeList();
    void RebuildMorphTargetList();
    void RebuildMaterialSlotsList();
    FReply SelectVerticesNearSelectedBone();

    TSharedRef<SWidget> MakeTopCommandBar();
    TSharedRef<SWidget> MakeLeftToolSidebar();
    TSharedRef<SWidget> MakeViewportOverlayHeader();
    TSharedRef<SWidget> MakeViewportStatsOverlay();
    TSharedRef<SWidget> MakeViewportHotkeysOverlay();
    TSharedRef<SWidget> MakeRightInspectorPanel();
    TSharedRef<SWidget> MakeSkeletonTreeTab();
    TSharedRef<SWidget> MakeMorphTargetsTab();
    TSharedRef<SWidget> MakeMeshStudioTab();
    TSharedRef<SWidget> MakeAIConsoleTab();
    TSharedRef<SWidget> MakeTopologyTab();
    TSharedRef<SWidget> MakeDetailsTab();
    TSharedRef<SWidget> MakeAssetDetailsTab();
    TSharedRef<SWidget> MakePreviewSettingsTab();
    TSharedRef<SWidget> MakePainterTab();
    TSharedRef<SWidget> MakeHistoryTab();
    TSharedRef<SWidget> MakeStatus();

    TSharedPtr<SNGonViewport> Viewport;
    TSharedPtr<SVerticalBox> SkeletonListBox;
    TSharedPtr<SVerticalBox> MorphListBox;
    TSharedPtr<SVerticalBox> MaterialSlotsListBox;
    TSharedPtr<STiPaintEditor> Painter;
    TSharedPtr<FNGonUndoSession> UndoSession;
    TSharedPtr<SNGonUndoTreePanel> UndoTreePanel;

    FString StaticMeshAssetPath;
    FString SkeletalMeshAssetPath;
    FText SourceLabel;
    FText SourceTypeLabel;
    FText ErrorText;

    TArray<FNGonBoneEntry> SkeletonBones;
    TArray<FNGonMorphEntry> MorphTargets;
    FString BoneSearchFilter;
    int32 SelectedBoneIndex = INDEX_NONE;
    int32 ActiveMaterialSlotIndex = 0;

    ENGonElement SelectionMode = ENGonElement::Face;
    ENGonToolShelfTab ActiveToolTab = ENGonToolShelfTab::Model;
    ENGonRightUpperTab ActiveUpperTab = ENGonRightUpperTab::MeshStudio;
    ENGonRightLowerTab ActiveLowerTab = ENGonRightLowerTab::AIConsole;

    bool bShowEdges = true;
    bool bShowVertices = true;
    bool bShowHeatmap = false;
    bool bShowStatsOverlay = true;
    bool bShowHotkeysOverlay = true;
    float HeatBias = 0.0f;
    float Bend = 0.0f;
    bool bSoftSelection = false;
    float SoftRadius = 100.0f;
    ENGonSoftTransformMode SoftMode = ENGonSoftTransformMode::Fixed;
    ENGonSoftMetric SoftMetric = ENGonSoftMetric::EdgeSurface;
    FVector2f LastPaint3DUV = FVector2f::ZeroVector;
    bool bHasLastPaint3DUV = false;
    ENGonSoftConnectivity SoftConnectivity = ENGonSoftConnectivity::SameIsland;
    float SoftSurfaceBlend = 0.5f;
};

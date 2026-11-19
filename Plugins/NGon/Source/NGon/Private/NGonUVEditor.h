#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SCompoundWidget.h"

struct FNGonMeshData;

enum class ENGonUVSelectionMode : uint8
{
    Corner,
    Edge,
    Face,
    Island
};

enum class ENGonUVGizmoMode : uint8
{
    None,
    Translate,
    Rotate,
    Scale,
    Combined
};

enum class ENGonUVGizmoPart : uint8
{
    None,
    TranslateU,
    TranslateV,
    TranslateUV,
    RotateRing,
    ScaleU,
    ScaleV,
    ScaleUniform
};

/** A UV document backed by the dynamic mesh UV overlay. Overlay element
 * IDs are retained, so a UV seam remains a seam while editing. */
class FNGonUVDocument : public TSharedFromThis<FNGonUVDocument>
{
public:
    struct FTriangle
    {
        FVector2f UV[3];
        int32 Element[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
        int32 FaceId = INDEX_NONE;
        int32 Island = INDEX_NONE;
    };

    ~FNGonUVDocument();

    bool Load(const FNGonMeshData& Data, FText& Error);
    void Clear();
    bool IsEmpty() const { return Triangles.IsEmpty(); }
    const TArray<FTriangle>& GetTriangles() const { return Triangles; }
    const FBox2D& GetBounds() const { return Bounds; }
    const TSet<int32>& GetSelectedElements() const { return SelectedElements; }
    const TSet<int32>& GetSelectedFaces() const { return SelectedFaces; }
    const TMap<int32, FVector2f>& GetElementUVs() const { return ElementUV; }
    int32 GetSelectedFaceCount() const { return SelectedFaces.Num(); }
    int32 GetSelectedIslandCount() const { return SelectedIslands.Num(); }
    int32 GetIslandCount() const { return IslandCount; }
    int32 GetCornerCount() const { return ElementUV.Num(); }
    FVector2f GetElementUV(int32 ElementId) const;
    bool GetSelectionCentroidUV(FVector2f& OutCenter) const;

    void SetSelectionMode(ENGonUVSelectionMode Mode) { SelectionMode = Mode; }
    ENGonUVSelectionMode GetSelectionMode() const { return SelectionMode; }
    void ClearSelection();
    void SetSelectedElements(const TSet<int32>& InElements, bool bAdd = false);
    void SetAllElementUVs(const TMap<int32, FVector2f>& InUVs);
    void SelectAtUV(const FVector2f& UV, bool bAdd, float Radius);
    void SelectRect(const FBox2D& Rect, bool bAdd);

    void TranslateSelected(const FVector2f& Delta);
    void RotateSelected(float Degrees);
    void ScaleSelected(float Factor);
    void ScaleSelectedNonUniform(const FVector2f& ScaleUV);
    void FlipSelected(bool bHorizontal);
    void NormalizeSelected();
    FText Describe() const;

private:
    void RebuildBounds();
    void RebuildIslands();
    void RebuildSelectionMetadata();
    void AddElementSelection(int32 Id, bool bAdd);
    void SelectFaceTriangle(int32 TriangleIndex, bool bAdd);
    void SelectIsland(int32 Island, bool bAdd);

    TArray<FTriangle> Triangles;
    TMap<int32, FVector2f> ElementUV;
    TMap<int32, TArray<int32>> ElementTriangles;
    TSet<int32> SelectedElements;
    TSet<int32> SelectedFaces;
    TSet<int32> SelectedIslands;
    FBox2D Bounds;
    ENGonUVSelectionMode SelectionMode = ENGonUVSelectionMode::Corner;
    int32 IslandCount = 0;
};

class SNGonUVCanvas final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SNGonUVCanvas) {}
    SLATE_END_ARGS()
    void Construct(const FArguments& InArgs, const TSharedRef<FNGonUVDocument>& InDocument);
    void FitToCanvas();
    void SetSelectionMode(ENGonUVSelectionMode Mode);
    void SetGizmoMode(ENGonUVGizmoMode Mode);
    ENGonUVGizmoMode GetGizmoMode() const { return GizmoMode; }
    void SetShowChecker(bool bInShow) { bShowChecker = bInShow; Refresh(); }
    bool IsShowingChecker() const { return bShowChecker; }
    void Refresh();

protected:
    bool SupportsKeyboardFocus() const override { return true; }
    FVector2D ComputeDesiredSize(float) const override { return FVector2D(640.0f, 480.0f); }
    int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
        FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
        bool bParentEnabled) const override;
    FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
    FVector2f ScreenToUV(const FGeometry& Geometry, const FVector2D& Screen) const;
    FVector2D UVToScreen(const FGeometry& Geometry, const FVector2f& UV) const;
    void UpdateViewFromBounds(const FGeometry& Geometry) const;
    ENGonUVGizmoPart HitTestGizmo(const FGeometry& Geometry, const FVector2D& LocalPx) const;
    void DrawGizmo2D(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer) const;
    void ApplyGizmoDrag(const FGeometry& Geo, const FVector2D& CurrentLocalPx);

    TSharedRef<FNGonUVDocument> Document = MakeShared<FNGonUVDocument>();
    mutable FVector2f ViewCenter = FVector2f(0.5f, 0.5f);
    mutable float Zoom = 300.0f;
    mutable bool bNeedsFit = true;

    ENGonUVGizmoMode GizmoMode = ENGonUVGizmoMode::Translate;
    ENGonUVGizmoPart HoveredGizmoPart = ENGonUVGizmoPart::None;
    ENGonUVGizmoPart ActiveGizmoPart = ENGonUVGizmoPart::None;
    bool bShowChecker = true;

    bool bDragging = false;
    bool bGizmoDragging = false;
    bool bPanning = false;
    bool bMarquee = false;

    FVector2D DragStart = FVector2D::ZeroVector;
    FVector2D LastMouse = FVector2D::ZeroVector;
    FVector2f LastUV = FVector2f::ZeroVector;
    FVector2f GizmoDragPivotUV = FVector2f::ZeroVector;
    FVector2D GizmoDragPivotPx = FVector2D::ZeroVector;
    TMap<int32, FVector2f> GizmoDragStartUVs;
    FBox2D Marquee;
};

class SNGonUVEditor final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SNGonUVEditor) {}
    SLATE_END_ARGS()
    void Construct(const FArguments& InArgs);

private:
    FReply Reload();
    void SetMode(ENGonUVSelectionMode Mode);
    void SetGizmoMode(ENGonUVGizmoMode Mode);
    void ApplyCommand(const TCHAR* Command);
    TSharedRef<SWidget> MakeModeButton(ENGonUVSelectionMode Mode, const FText& Label);
    TSharedRef<SWidget> MakeGizmoModeButton(ENGonUVGizmoMode Mode, const FText& Label, const FText& Tooltip);
    TSharedRef<SWidget> MakeToolbar();
    TSharedRef<SWidget> MakeUVStudioPanel();
    TSharedRef<SWidget> MakeUVAIConsolePanel();
    TSharedRef<SWidget> MakeStatus();
    void ShowError(const FText& Error);

    TSharedPtr<FNGonUVDocument> Document;
    TSharedPtr<SNGonUVCanvas> Canvas;
    FText SourceLabel;
    FText ErrorText;
    FString AIJsonInput;
    FString AILastOutput;
    bool bAILastOk = true;
    float PackPadding = 0.02f;
    float SnapStep = 0.0625f;
};
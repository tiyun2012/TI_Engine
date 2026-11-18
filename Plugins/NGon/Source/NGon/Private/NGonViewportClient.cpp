#include "SNGonPreview.h"
#include "NGonSceneSkeleton.h"
#include "NGonUIIcons.h"
#include "NGonViewportGizmo.h"

#include "AssetEditorModeManager.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "InteractiveToolManager.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "Slate/SceneViewport.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "Widgets/SViewport.h"

namespace NGon
{
void UpdateActiveViewportUVMeshFromTopology(const FNGonTopology& Topology);
}

using NGon::FNGonSkeletonTreeUIState;
using NGon::FindUIStateByViewport;

namespace
{
// Keep both objects alive until after FEditorViewportClient is destroyed.
struct FNGonPreviewSceneOwner
{
    explicit FNGonPreviewSceneOwner(const TSharedRef<FNGonPreviewScene>& InPreview)
        : PreviewOwner(InPreview)
        , ModeToolsOwner(MakeShared<FAssetEditorModeManager>())
    {
        ModeToolsOwner->SetPreviewScene(&InPreview->GetPreviewScene());
    }

    TSharedRef<FNGonPreviewScene> PreviewOwner;
    TSharedRef<FAssetEditorModeManager> ModeToolsOwner;
};

class FNGonViewportClient final : private FNGonPreviewSceneOwner, public FEditorViewportClient
{
public:
    FNGonViewportClient(
        const TSharedRef<FNGonPreviewScene>& InPreview,
        const TSharedRef<SNGonViewport>& InWidget)
        : FNGonPreviewSceneOwner(InPreview)
        , FEditorViewportClient(&ModeToolsOwner.Get(), &InPreview->GetPreviewScene(), InWidget)
        , ViewportWidget(InWidget)
    {
        SetViewportType(LVT_Perspective);
        SetViewMode(VMI_Lit);
        SetRealtime(true);
        SetAllowCinematicControl(false);
        EngineShowFlags.SetGrid(true);
        EngineShowFlags.SetSelectionOutline(false);
        EngineShowFlags.SetEyeAdaptation(false);
        EngineShowFlags.SetModeWidgets(true);
        ModeToolsOwner->ActivateDefaultMode();
        ModeToolsOwner->SetWidgetMode(UE::Widget::WM_None);

        if (UModeManagerInteractiveToolsContext* Context = ModeToolsOwner->GetInteractiveToolsContext())
        {
            Context->SetViewportInteractionsEnabled(false);
            PreviewOwner->SetPreviewMaterials(Context->ToolManager.Get());
        }
    }

    void SetGridShowFlag(bool bVisible)
    {
        EngineShowFlags.SetGrid(bVisible);
        Invalidate();
    }

    void FrameBounds(const FBox& Bounds, bool bSetInitialDirection)
    {
        if (!Bounds.IsValid)
        {
            return;
        }
        if (bSetInitialDirection && IsPerspective())
        {
            const FVector Size = Bounds.GetSize();
            FVector FromTarget(-0.32, -0.50, 0.80);
            if (Size.Y < FMath::Min(Size.X, Size.Z) * 0.25)
            {
                FromTarget = FVector(0.25, -1.0, 0.30);
            }
            else if (Size.X < FMath::Min(Size.Y, Size.Z) * 0.25)
            {
                FromTarget = FVector(-1.0, -0.25, 0.30);
            }
            SetViewRotation((-FromTarget).Rotation());
        }
        PendingFrameBounds = Bounds.ExpandBy(FMath::Max(Bounds.GetExtent().GetMax() * 0.05, 1.0));
        ApplyPendingFrame();
    }

    UE::Widget::EWidgetMode GetWidgetMode() const override
    {
        return UE::Widget::WM_None;
    }

    void Tick(float DeltaSeconds) override
    {
        FEditorViewportClient::Tick(DeltaSeconds);
        ApplyPendingFrame();
        PreviewOwner->RefreshPaintMaterial();
        PreviewOwner->UpdateCameraVertexScales(GetViewLocation(), GetViewRotation().Vector());
        if (bPendingGizmoMove)
        {
            UpdateGizmoDrag(PendingGizmoMouse);
        }
    }

    bool InputKey(const FInputKeyEventArgs& EventArgs) override
    {
        FViewport* InViewport = EventArgs.Viewport;
        const FKey Key = EventArgs.Key;
        const bool bPressed = EventArgs.Event == IE_Pressed;

        if (bPressed && Key == EKeys::Escape && PointerAction != EPointerAction::None)
        {
            CancelPointerAction();
            return true;
        }

        const bool bNavigating = IsNavigating(InViewport);
        if (Key == EKeys::LeftMouseButton && InViewport)
        {
            const FIntPoint Mouse(InViewport->GetMouseX(), InViewport->GetMouseY());
            if (EventArgs.Event == IE_Released && PointerAction != EPointerAction::None)
            {
                FinishPointerAction(InViewport, Mouse);
                return true;
            }
            if ((bPressed || EventArgs.Event == IE_DoubleClick) && !bNavigating
                && PointerAction == EPointerAction::None)
            {
                BeginPointerAction(InViewport, Mouse);
                return true;
            }
        }

        if (PointerAction != EPointerAction::None)
        {
            return true;
        }

        if (bPressed && !bNavigating && IsCtrlPressed() && Key == EKeys::I)
        {
            const bool bReset = IsShiftPressed()
                ? NGon::ResetAllSceneBonesTransforms(&PreviewOwner.Get())
                : NGon::ResetSelectedSceneBoneTransform(&PreviewOwner.Get());
            if (bReset)
            {
                NotifyBoneTransformChanged();
                Invalidate();
            }
            return true;
        }

        const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin();
        if (bPressed && Pinned)
        {
            if (IsAltPressed() && !IsCtrlPressed() && Key == EKeys::Nine)
            {
                if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get());
                    UIState && UIState->OnToggleEdgesShortcut)
                {
                    UIState->OnToggleEdgesShortcut();
                }
                return true;
            }
            if (!bNavigating && !IsCtrlPressed() && HandleViewportShortcut(*Pinned, Key))
            {
                return true;
            }
        }
        return FEditorViewportClient::InputKey(EventArgs);
    }

    EMouseCursor::Type GetCursor(FViewport* InViewport, int32 X, int32 Y) override
    {
        if (PointerAction == EPointerAction::Gizmo)
        {
            return EMouseCursor::CardinalCross;
        }
        return FEditorViewportClient::GetCursor(InViewport, X, Y);
    }

    void MouseMove(FViewport* InViewport, int32 X, int32 Y) override
    {
        if (MovePointer(FIntPoint(X, Y)))
        {
            return;
        }
        FEditorViewportClient::MouseMove(InViewport, X, Y);
        HoverPixel.Reset();
        if (InViewport && !IsNavigating(InViewport))
        {
            HoverPixel = FVector2D(X, Y);
        }
        PostInvalidation();
    }

    void CapturedMouseMove(FViewport* InViewport, int32 X, int32 Y) override
    {
        if (!MovePointer(FIntPoint(X, Y)))
        {
            FEditorViewportClient::CapturedMouseMove(InViewport, X, Y);
        }
    }

    void MouseLeave(FViewport* InViewport) override
    {
        HoverPixel.Reset();
        PostInvalidation();
        FEditorViewportClient::MouseLeave(InViewport);
    }

    void LostFocus(FViewport* InViewport) override
    {
        CancelPointerAction();
        FEditorViewportClient::LostFocus(InViewport);
    }

    bool InputAxis(const FInputKeyEventArgs& Args) override
    {
        return PointerAction != EPointerAction::None || FEditorViewportClient::InputAxis(Args);
    }

    void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override
    {
        FEditorViewportClient::Draw(View, PDI);
        if (View && PDI && NGon::HasSceneBones(&PreviewOwner.Get()))
        {
            const FNGonSkeletonTreeUIState* UIState = GetUIState();
            NGon::DrawSceneSkeleton(&PreviewOwner.Get(), View, PDI, UIState ? UIState->BoneDrawMode : 0);
        }
    }

    void Draw(FViewport* InViewport, FCanvas* Canvas) override
    {
        FEditorViewportClient::Draw(InViewport, Canvas);
        if (InViewport && Canvas && !Canvas->IsHitTesting())
        {
            const FNGonSkeletonTreeUIState* UIState = GetUIState();
            const bool bDrawBoneNames = NGon::HasSceneBones(&PreviewOwner.Get())
                && (!UIState || (UIState->bShowBoneNames && UIState->BoneDrawMode != 3));
            const bool bDrawGizmo = PreviewOwner->HasGizmoTarget();
            if (bDrawBoneNames || bDrawGizmo)
            {
                FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
                if (const FSceneView* View = BuildSceneView(InViewport, ViewFamily))
                {
                    if (bDrawBoneNames)
                    {
                        NGon::DrawSceneSkeletonLabels(&PreviewOwner.Get(), View, InViewport, Canvas, true);
                    }
                    if (bDrawGizmo)
                    {
                        BuildGizmo(*View);
                        ForegroundGizmo.Draw(*Canvas, HoverPixel);
                    }
                }
            }
        }

        if (bMarqueeActive && Canvas && !Canvas->IsHitTesting())
        {
            const FBox2D Rect = MakeMarquee();
            const double DPIScale = FMath::Max(static_cast<double>(Canvas->GetDPIScale()), 0.01);
            FCanvasBoxItem Box(Rect.Min / DPIScale, Rect.GetSize() / DPIScale);
            Box.SetColor(FLinearColor(0.15f, 0.65f, 1.0f, 0.85f));
            Box.LineThickness = 2.0f;
            Canvas->DrawItem(Box);
        }
    }

private:
    enum class EPointerAction : uint8 { None, Select, EdgeLoop, Gizmo };

    void PostInvalidation() { Invalidate(false, false); }

    FNGonSkeletonTreeUIState* GetUIState() const
    {
        const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin();
        return FindUIStateByViewport(Pinned.Get());
    }

    void NotifyBoneTransformChanged() const
    {
        if (const FNGonSkeletonTreeUIState* UI = GetUIState(); UI && UI->OnViewportBoneTransformChanged)
        {
            UI->OnViewportBoneTransformChanged();
        }
    }

    void ApplyPendingFrame()
    {
        if (PendingFrameBounds.IsValid && Viewport && Viewport->GetSizeXY().GetMin() > 0)
        {
            FocusViewportOnBox(PendingFrameBounds, true);
            PendingFrameBounds.Init();
            Invalidate();
        }
    }

    bool IsNavigating(FViewport* InViewport) const
    {
        return IsAltPressed()
            || (InViewport && (InViewport->KeyState(EKeys::RightMouseButton) || InViewport->KeyState(EKeys::MiddleMouseButton)));
    }

    bool HandleViewportShortcut(SNGonViewport& PreviewViewport, const FKey& Key)
    {
        if (Key == EKeys::Q || Key == EKeys::W || Key == EKeys::E || Key == EKeys::R)
        {
            ENGonGizmoMode Mode = ENGonGizmoMode::None;
            if (Key == EKeys::W) Mode = ENGonGizmoMode::Translate;
            if (Key == EKeys::E) Mode = ENGonGizmoMode::Rotate;
            if (Key == EKeys::R) Mode = ENGonGizmoMode::Scale;
            PreviewViewport.SetGizmoMode(Mode);
        }
        else if (Key == EKeys::One || Key == EKeys::Two || Key == EKeys::Three)
        {
            PreviewViewport.SetSelectionMode(Key == EKeys::One ? ENGonElement::Vertex
                : Key == EKeys::Two ? ENGonElement::Edge : ENGonElement::Face);
        }
        else if (Key == EKeys::Four)
        {
            NGon::SetBoneGizmoActive(&PreviewOwner.Get(), true);
            Invalidate();
        }
        else if (Key == EKeys::H)
        {
            PreviewViewport.ToggleHeatmap();
        }
        else if (Key == EKeys::F)
        {
            PreviewViewport.FrameMesh();
        }
        else if (Key == EKeys::Delete || Key == EKeys::BackSpace)
        {
            PreviewViewport.DeleteSelectedFaces();
        }
        else
        {
            return false;
        }
        return true;
    }

    FSceneViewFamily::ConstructionValues MakeViewFamily(FViewport* InViewport)
    {
        FSceneViewFamily::ConstructionValues Values(InViewport, GetScene(), EngineShowFlags);
        Values.SetRealtimeUpdate(IsRealtime());
        return Values;
    }

    FSceneView* BuildSceneView(FViewport* InViewport, FSceneViewFamilyContext& ViewFamily)
    {
        if (InViewport->GetSizeXY().GetMin() <= 0)
        {
            return nullptr;
        }
        return CalcSceneView(&ViewFamily);
    }

    void BuildGizmo(const FSceneView& View)
    {
        const FTransform Frame = PreviewOwner->GetGizmoDisplayTransform();
        const FVector Origin = Frame.GetLocation();

        // Extract camera basis directly from ViewMatrices so Alt+LMB orbit camera rotation is always exact
        const FMatrix& ViewMatrix = View.ViewMatrices.GetViewMatrix();
        const FVector CamRight = ViewMatrix.GetColumn(0).GetSafeNormal();
        const FVector CamUp = ViewMatrix.GetColumn(1).GetSafeNormal();
        const FVector CamForward = ViewMatrix.GetColumn(2).GetSafeNormal();
        const FVector CamOrigin = View.ViewMatrices.GetViewOrigin();

        const double ClipW = View.WorldToScreen(Origin).W;
        const double ProjectionWidth = static_cast<double>(View.UnscaledViewRect.Width())
            * FMath::Abs(View.ViewMatrices.GetProjectionMatrix().M[0][0]);
        double UnitsPerPixel = (ClipW > 0.0 && ProjectionWidth > 1e-4)
            ? (2.0 * ClipW / ProjectionWidth)
            : 0.0;
        if (UnitsPerPixel <= 0.0)
        {
            FVector2D OriginPx, OffsetPx;
            if (View.WorldToPixel(Origin, OriginPx) && View.WorldToPixel(Origin + CamRight * 10.0, OffsetPx))
            {
                const double PxDist = FVector2D::Distance(OriginPx, OffsetPx);
                if (PxDist > 1e-4)
                {
                    UnitsPerPixel = 10.0 / PxDist;
                }
            }
        }

        const bool bAxisScale = !NGon::IsBoneGizmoActive(&PreviewOwner.Get())
            || !NGon::IsSceneWorldCoordinateSystem(&PreviewOwner.Get());
        ForegroundGizmo.Build(Frame, PreviewOwner->GetGizmoMode(), UnitsPerPixel, GetDPIScale(),
            CamOrigin, CamForward, CamRight, CamUp, bAxisScale,
            [&View](const FVector& Point, FVector2D& Pixel) { return View.WorldToPixel(Point, Pixel); });
    }

    void BeginPointerAction(FViewport* InViewport, const FIntPoint& Mouse)
    {
        const bool bBoneMode = NGon::IsBoneGizmoActive(&PreviewOwner.Get());
        if (IsCtrlPressed() && !bBoneMode)
        {
            PointerAction = EPointerAction::EdgeLoop;
            return;
        }
        if (PreviewOwner->HasGizmoTarget())
        {
            FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
            if (const FSceneView* View = BuildSceneView(InViewport, ViewFamily))
            {
                BuildGizmo(*View);
                if (ForegroundGizmo.BeginDrag(FVector2D(Mouse.X, Mouse.Y)))
                {
                    const FTransform Start = PreviewOwner->GetSelectionGizmoTransform();
                    if (PreviewOwner->BeginSoftTransform(Start, PreviewOwner->GetSoftTransformMode()))
                    {
                        PointerAction = EPointerAction::Gizmo;
                        LastGizmoMouse = Mouse;
                        PostInvalidation();
                        return;
                    }
                    ForegroundGizmo.EndDrag();
                }
            }
        }

        PointerAction = EPointerAction::Select;
        MarqueeStart = FVector2D(Mouse.X, Mouse.Y);
        MarqueeEnd = MarqueeStart;
    }

    bool MovePointer(const FIntPoint& Mouse)
    {
        if (PointerAction == EPointerAction::Gizmo)
        {
            PendingGizmoMouse = Mouse;
            bPendingGizmoMove = true;
            return true;
        }
        if (PointerAction == EPointerAction::Select)
        {
            if (!NGon::IsBoneGizmoActive(&PreviewOwner.Get()))
            {
                MarqueeEnd = FVector2D(Mouse.X, Mouse.Y);
                bMarqueeActive = bMarqueeActive || FVector2D::DistSquared(MarqueeStart, MarqueeEnd) > 25.0;
                if (bMarqueeActive)
                {
                    PostInvalidation();
                }
            }
            return true;
        }
        return PointerAction == EPointerAction::EdgeLoop;
    }

    void UpdateGizmoDrag(const FIntPoint& Mouse)
    {
        bPendingGizmoMove = false;
        if (Mouse == LastGizmoMouse)
        {
            return;
        }
        PreviewOwner->ApplyGizmoTransform(ForegroundGizmo.DragTo(FVector2D(Mouse.X, Mouse.Y)));
        LastGizmoMouse = Mouse;
        if (NGon::IsBoneTransformActive(&PreviewOwner.Get()))
        {
            NotifyBoneTransformChanged();
        }
        PostInvalidation();
    }

    void FinishPointerAction(FViewport* InViewport, const FIntPoint& Mouse)
    {
        if (PointerAction == EPointerAction::Gizmo)
        {
            UpdateGizmoDrag(Mouse);
            PreviewOwner->EndSoftTransform();
            ForegroundGizmo.EndDrag();
        }
        else
        {
            MovePointer(Mouse);
            FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
            if (const FSceneView* View = BuildSceneView(InViewport, ViewFamily))
            {
                SelectAtPointer(*View, FVector2D(Mouse.X, Mouse.Y));
            }
        }

        PointerAction = EPointerAction::None;
        bMarqueeActive = false;
        PostInvalidation();
    }

    void SelectAtPointer(const FSceneView& View, const FVector2D& Mouse)
    {
        const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin();
        if (!Pinned)
        {
            return;
        }

        FNGonSkeletonTreeUIState* UI = GetUIState();
        const bool bBonesVisible = NGon::HasSceneBones(&PreviewOwner.Get()) && (!UI || UI->BoneDrawMode != 3);
        if (!bMarqueeActive && bBonesVisible)
        {
            const float PickRadius = NGon::IsBoneGizmoActive(&PreviewOwner.Get()) ? 22.0f : 14.0f;
            const int32 HitBone = NGon::PickSceneBone(&PreviewOwner.Get(), View, Mouse, PickRadius, true);
            if (HitBone != INDEX_NONE)
            {
                if (Pinned->GetGizmoMode() == ENGonGizmoMode::None)
                {
                    Pinned->SetGizmoMode(ENGonGizmoMode::Translate);
                }
                if (UI && UI->OnViewportBonePicked)
                {
                    UI->OnViewportBonePicked(HitBone);
                }
                return;
            }
        }

        if (bMarqueeActive)
        {
            Pinned->BoxSelect(View, MakeMarquee(), IsShiftPressed(), MarqueeEnd.X >= MarqueeStart.X);
        }
        else
        {
            const bool bAdd = IsShiftPressed();
            const int32 Hit = Pinned->Pick(View, Mouse, bAdd);
            if (PointerAction == EPointerAction::EdgeLoop && Pinned->GetSelectionMode() == ENGonElement::Edge && Hit != INDEX_NONE)
            {
                Pinned->SelectEdgeLoop(Hit, bAdd);
            }
        }
    }

    void CancelPointerAction()
    {
        ForegroundGizmo.EndDrag();
        HoverPixel.Reset();
        const bool bBoneDrag = NGon::IsBoneTransformActive(&PreviewOwner.Get());
        PreviewOwner->CancelSoftTransform();
        if (bBoneDrag)
        {
            NotifyBoneTransformChanged();
        }
        PointerAction = EPointerAction::None;
        bPendingGizmoMove = false;
        bMarqueeActive = false;
        PostInvalidation();
    }

    FBox2D MakeMarquee() const
    {
        FBox2D Rect(ForceInit);
        Rect += MarqueeStart;
        Rect += MarqueeEnd;
        return Rect;
    }

    TWeakPtr<SNGonViewport> ViewportWidget;
    FNGonViewportGizmo ForegroundGizmo;
    TOptional<FVector2D> HoverPixel;
    FBox PendingFrameBounds{ForceInit};

    EPointerAction PointerAction = EPointerAction::None;
    FIntPoint LastGizmoMouse = FIntPoint::ZeroValue;
    FIntPoint PendingGizmoMouse = FIntPoint::ZeroValue;
    FVector2D MarqueeStart = FVector2D::ZeroVector;
    FVector2D MarqueeEnd = FVector2D::ZeroVector;
    bool bPendingGizmoMove = false;
    bool bMarqueeActive = false;
};
}

void SNGonViewport::Construct(const FArguments& InArgs)
{
    PreviewScene = MakeShared<FNGonPreviewScene>();
    bGridVisible = true;
    SEditorViewport::Construct(SEditorViewport::FArguments());
}

TSharedRef<FEditorViewportClient> SNGonViewport::MakeEditorViewportClient()
{
    return MakeShared<FNGonViewportClient>(PreviewScene.ToSharedRef(), SharedThis(this));
}

void SNGonViewport::SetMesh(FNGonMeshData&& Data, TArray<UMaterialInterface*> Materials)
{
    bHeatmapVisible = false;
    NGon::ClearSceneSkeleton(PreviewScene.Get());
    PreviewScene->SetMesh(MoveTemp(Data), MoveTemp(Materials));
    if (const TSharedPtr<FEditorViewportClient> PreviewClient = GetViewportClient())
    {
        static_cast<FNGonViewportClient*>(PreviewClient.Get())->FrameBounds(PreviewScene->GetMeshBounds(), true);
    }
}

void SNGonViewport::FrameMesh()
{
    if (const TSharedPtr<FEditorViewportClient> PreviewClient = GetViewportClient())
    {
        static_cast<FNGonViewportClient*>(PreviewClient.Get())->FrameBounds(PreviewScene->GetSelectionOrMeshBounds(), false);
    }
}

void SNGonViewport::FocusViewport()
{
    if (const TSharedPtr<FSceneViewport> PreviewSceneViewport = GetSceneViewport())
    {
        if (const TSharedPtr<SViewport> SlateViewport = PreviewSceneViewport->GetViewportWidget().Pin())
        {
            FSlateApplication::Get().SetKeyboardFocus(SlateViewport, EFocusCause::SetDirectly);
        }
    }
}

void SNGonViewport::SetShadingMode(ENGonViewportShading Mode)
{
    ShadingMode = Mode;
    PreviewScene->SetFlatShading(Mode == ENGonViewportShading::Faceted);
    if (const TSharedPtr<FEditorViewportClient> PreviewClient = GetViewportClient())
    {
        PreviewClient->SetViewMode(Mode == ENGonViewportShading::Wireframe ? VMI_Wireframe : VMI_Lit);
        Invalidate();
    }
}

void SNGonViewport::OnFocusViewportToSelection()
{
    FrameMesh();
}

void SNGonViewport::SetSelectionMode(ENGonElement Element)
{
    NGon::SetBoneGizmoActive(PreviewScene.Get(), false);
    if (SelectionMode != Element)
    {
        SelectionMode = Element;
        PreviewScene->ClearSelection();
        SetHeatmapVisible(false);
    }
    Invalidate();
}

void SNGonViewport::SetGizmoMode(ENGonGizmoMode Mode)
{
    PreviewScene->SetGizmoMode(Mode);
    Invalidate();
}

ENGonGizmoMode SNGonViewport::GetGizmoMode() const
{
    return PreviewScene->GetGizmoMode();
}

void SNGonViewport::SetGridVisible(bool bVisible)
{
    bGridVisible = bVisible;
    if (TSharedPtr<FEditorViewportClient> ViewportClient = GetViewportClient())
    {
        static_cast<FNGonViewportClient*>(ViewportClient.Get())->SetGridShowFlag(bVisible);
    }
}

void SNGonViewport::ToggleHeatmap()
{
    if (!PreviewScene->HasSoftSelectionInfluence())
    {
        bHeatmapVisible = false;
        PreviewScene->SetHeatmapVisible(false);
        Invalidate();
        return;
    }
    bHeatmapVisible = !bHeatmapVisible;
    PreviewScene->SetHeatmapVisible(bHeatmapVisible);
    Invalidate();
}

bool SNGonViewport::IsHeatmapVisible() const
{
    return bHeatmapVisible && PreviewScene.IsValid() && PreviewScene->HasSoftSelectionInfluence();
}

void SNGonViewport::SetHeatmapVisible(bool bVisible)
{
    bHeatmapVisible = bVisible && PreviewScene->HasSoftSelectionInfluence();
    PreviewScene->SetHeatmapVisible(bHeatmapVisible);
    Invalidate();
}

int32 SNGonViewport::Pick(const FSceneView& View, const FVector2D& Cursor, bool bAdd)
{
    const int32 Id = PreviewScene->Pick(SelectionMode, View, Cursor, bAdd);
    if (!PreviewScene->HasSoftSelectionInfluence())
    {
        SetHeatmapVisible(false);
    }
    Invalidate();
    return Id;
}

void SNGonViewport::BoxSelect(const FSceneView& View, const FBox2D& ScreenRect, bool bAdd, bool bFullyContained)
{
    PreviewScene->BoxSelect(SelectionMode, View, ScreenRect, bAdd, bFullyContained);
    if (!PreviewScene->HasSoftSelectionInfluence())
    {
        SetHeatmapVisible(false);
    }
    Invalidate();
}

bool SNGonViewport::SelectEdgeLoop(int32 StartEdgeId, bool bAdd)
{
    const bool bSelected = PreviewScene->SelectEdgeLoop(StartEdgeId, bAdd);
    if (!PreviewScene->HasSoftSelectionInfluence())
    {
        SetHeatmapVisible(false);
    }
    Invalidate();
    return bSelected;
}

bool SNGonViewport::DeleteSelectedFaces()
{
    const bool bDeleted = !NGon::IsBoneGizmoActive(PreviewScene.Get()) && PreviewScene->DeleteSelectedFaces();
    if (bDeleted)
    {
        if (!PreviewScene->HasSoftSelectionInfluence())
        {
            SetHeatmapVisible(false);
        }
        NGon::UpdateActiveViewportUVMeshFromTopology(PreviewScene->GetTopology());
        Invalidate();
    }
    return bDeleted;
}
// NGon Unified ABI Sync v5.8.2

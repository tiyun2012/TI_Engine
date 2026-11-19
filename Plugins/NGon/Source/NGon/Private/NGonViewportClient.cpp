#include "SNGonPreview.h"
#include "NGonSceneSkeleton.h"
#include "NGonUIIcons.h"
#include "NGonViewportGizmo.h"
#include "NGonMeshAPI.h"
#include "NGonPieMenu.h"
#include "Framework/Docking/TabManager.h"

#include "AssetEditorModeManager.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
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

        if (PieMenu.IsOpen() && InViewport)
        {
            const double DPI = FMath::Max(static_cast<double>(GetDPIScale()), 0.01);
            const FVector2D ViewSize = FVector2D(InViewport->GetSizeXY()) / DPI;
            const FVector2D MousePos = FVector2D(InViewport->GetMouseX(), InViewport->GetMouseY()) / DPI;

            if (Key == EKeys::LeftMouseButton || Key == EKeys::RightMouseButton)
            {
                if (bPressed)
                {
                    PieMenu.OnMouseDown(ViewSize, MousePos);
                }
                else if (EventArgs.Event == IE_Released)
                {
                    PieMenu.OnMouseUp(ViewSize, MousePos);
                }
                PostInvalidation();
                return true;
            }
            if (Key == EKeys::SpaceBar && EventArgs.Event == IE_Released)
            {
                // Flick-to-activate if user held Space and moved outside the dead zone; otherwise keep open for click navigation
                if (FVector2D::Distance(MousePos, PieOpenPos) >= PieMenu.DeadZoneRadius)
                {
                    PieMenu.OnMouseUp(ViewSize, MousePos);
                    PostInvalidation();
                }
                return true;
            }
            if (Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown)
            {
                PieMenu.OnMouseWheel(ViewSize, MousePos, Key == EKeys::MouseScrollUp ? 1.0f : -1.0f);
                PostInvalidation();
                return true;
            }
            if (bPressed)
            {
                PieMenu.OnKeyDown(ViewSize, Key, (EventArgs.Event == IE_Repeat));
                PostInvalidation();
                return true;
            }
            return true;
        }

        if (Key == EKeys::RightMouseButton && InViewport && !IsAltPressed() && PointerAction == EPointerAction::None)
        {
            const FIntPoint Mouse(InViewport->GetMouseX(), InViewport->GetMouseY());
            if (bPressed && !(EventArgs.Event == IE_Repeat))
            {
                if (IsShiftPressed())
                {
                    OpenViewportPieMenu(InViewport);
                    return true;
                }
                bTrackingRmbTap = true;
                bRmbDragged = false;
                RmbDownMouse = Mouse;
            }
            else if (EventArgs.Event == IE_Released && bTrackingRmbTap)
            {
                const bool bWasTap = !bRmbDragged && FVector2D::Distance(FVector2D(Mouse), FVector2D(RmbDownMouse)) <= 5.0;
                bTrackingRmbTap = false;
                bRmbDragged = false;
                if (bWasTap)
                {
                    // Let base viewport release RMB capture first, then pop up the context Pie Menu
                    FEditorViewportClient::InputKey(EventArgs);
                    OpenViewportPieMenu(InViewport);
                    return true;
                }
            }
        }

        if (bPressed && !(EventArgs.Event == IE_Repeat) && InViewport && PointerAction == EPointerAction::None && !IsAltPressed() && !IsCtrlPressed())
        {
            if (Key == EKeys::SpaceBar)
            {
                OpenViewportPieMenu(InViewport);
                return true;
            }
        }

        if (bPressed && Key == EKeys::Escape && PointerAction != EPointerAction::None)
        {
            CancelPointerAction();
            return true;
        }

        const bool bNavigating = IsNavigating(InViewport);

        // Maya-style 'B' hotkey: tap B to toggle Soft Selection; hold B + LMB drag left/right to adjust radius
        if (Key == EKeys::B && !IsCtrlPressed() && !IsAltPressed())
        {
            if (bPressed && !(EventArgs.Event == IE_Repeat))
            {
                bBKeyHeld = true;
                bBKeyDragged = false;
                return true;
            }
            if (EventArgs.Event == IE_Released)
            {
                if (PointerAction == EPointerAction::SoftRadius)
                {
                    PointerAction = EPointerAction::None;
                    PostInvalidation();
                }
                else if (bBKeyHeld && !bBKeyDragged)
                {
                    ToggleSoftSelectionHotkey();
                }
                bBKeyHeld = false;
                bBKeyDragged = false;
                return true;
            }
            if (bBKeyHeld)
            {
                return true;
            }
        }

        if (Key == EKeys::LeftMouseButton && InViewport)
        {
            const FIntPoint Mouse(InViewport->GetMouseX(), InViewport->GetMouseY());
            if (EventArgs.Event == IE_Released && PointerAction != EPointerAction::None)
            {
                FinishPointerAction(InViewport, Mouse);
                return true;
            }
            const double NowSec = FPlatformTime::Seconds();
            const bool bFastSecondClick = bPressed
                && (NowSec - LastLmbClickTime) <= 0.42
                && FVector2D::DistSquared(FVector2D(Mouse), FVector2D(LastLmbClickPos)) <= 64.0;
            if ((EventArgs.Event == IE_DoubleClick || bFastSecondClick) && !bNavigating && PointerAction == EPointerAction::None && !bBKeyHeld)
            {
                LastLmbClickTime = 0.0;
                if (HandleDoubleClickGesture(InViewport, Mouse))
                {
                    return true;
                }
            }
            if ((bPressed || EventArgs.Event == IE_DoubleClick) && !bNavigating
                && PointerAction == EPointerAction::None)
            {
                if (bBKeyHeld)
                {
                    BeginSoftRadiusDrag(InViewport, Mouse);
                    return true;
                }
                LastLmbClickTime = NowSec;
                LastLmbClickPos = Mouse;
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
        if (PreviewOwner->HasGizmoTarget() && !IsNavigating(InViewport) && ForegroundGizmo.IsHovered(FVector2D(X, Y)))
        {
            return EMouseCursor::CardinalCross;
        }
        return FEditorViewportClient::GetCursor(InViewport, X, Y);
    }

    void MouseMove(FViewport* InViewport, int32 X, int32 Y) override
    {
        if (PieMenu.IsOpen() && InViewport)
        {
            const double DPI = FMath::Max(static_cast<double>(GetDPIScale()), 0.01);
            PieMenu.OnMouseMove(FVector2D(InViewport->GetSizeXY()) / DPI, FVector2D(X, Y) / DPI);
            PostInvalidation();
            return;
        }
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
        if (bTrackingRmbTap && FVector2D::Distance(FVector2D(X, Y), FVector2D(RmbDownMouse)) > 5.0)
        {
            bRmbDragged = true;
        }
        if (PieMenu.IsOpen() && InViewport)
        {
            const double DPI = FMath::Max(static_cast<double>(GetDPIScale()), 0.01);
            PieMenu.OnMouseMove(FVector2D(InViewport->GetSizeXY()) / DPI, FVector2D(X, Y) / DPI);
            PostInvalidation();
            return;
        }
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
        PieMenu.Close();
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
            const bool bDrawBonesOverlay = NGon::HasSceneBones(&PreviewOwner.Get())
                && (!UIState || UIState->BoneDrawMode != 3);
            const bool bShowBoneNames = !UIState || UIState->bShowBoneNames;
            const bool bDrawGizmo = PreviewOwner->HasGizmoTarget();
            const bool bCheckVertexHover = HoverPixel.IsSet()
                && !IsNavigating(InViewport)
                && PointerAction == EPointerAction::None
                && !NGon::IsBoneGizmoActive(&PreviewOwner.Get())
                && !PreviewOwner->GetPositions().IsEmpty();
            if (bDrawBonesOverlay || bDrawGizmo || bCheckVertexHover)
            {
                FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
                if (const FSceneView* View = BuildSceneView(InViewport, ViewFamily))
                {
                    if (bDrawBonesOverlay)
                    {
                        NGon::DrawSceneSkeletonLabels(&PreviewOwner.Get(), View, InViewport, Canvas, bShowBoneNames);
                    }
                    if (bDrawGizmo)
                    {
                        BuildGizmo(*View);
                    }
                    const bool bGizmoHovered = bDrawGizmo && HoverPixel.IsSet() && ForegroundGizmo.IsHovered(HoverPixel.GetValue());
                    if (bCheckVertexHover && !bGizmoHovered)
                    {
                        DrawHoveredVertex(*View, *Canvas, HoverPixel.GetValue());
                    }
                    if (bDrawGizmo)
                    {
                        ForegroundGizmo.Draw(*Canvas, HoverPixel);
                    }
                }
            }
        }

        if (PointerAction == EPointerAction::SoftRadius && Canvas && !Canvas->IsHitTesting())
        {
            const double DPIScale = FMath::Max(static_cast<double>(Canvas->GetDPIScale()), 0.01);
            const FVector2D Center = FVector2D(SoftRadiusStartMouse.X, SoftRadiusStartMouse.Y) / DPIScale;
            const double CurrentRadius = PreviewOwner->GetSoftSelectionSettings().Radius;
            const double RingPx = FMath::Clamp(CurrentRadius / FMath::Max(SoftRadiusUnitsPerPixel * DPIScale, 1e-4), 8.0, 420.0);
            constexpr int32 Segs = 48;
            FVector2D Prev = Center + FVector2D(RingPx, 0.0);
            const FLinearColor RingCol(1.0f, 0.72f, 0.16f, 0.92f);
            for (int32 i = 1; i <= Segs; ++i)
            {
                const double A = (2.0 * UE_DOUBLE_PI * i) / Segs;
                const FVector2D Curr = Center + FVector2D(FMath::Cos(A) * RingPx, FMath::Sin(A) * RingPx);
                FCanvasLineItem SegLine(Prev, Curr);
                SegLine.SetColor(RingCol);
                SegLine.LineThickness = 1.8f;
                Canvas->DrawItem(SegLine);
                Prev = Curr;
            }
            if (GEngine && GEngine->GetSmallFont())
            {
                const FString Label = FString::Printf(TEXT("Soft Radius: %.1f"), CurrentRadius);
                FCanvasTextItem TextItem(Center + FVector2D(8.0, -RingPx - 18.0), FText::FromString(Label), GEngine->GetSmallFont(), RingCol);
                TextItem.EnableShadow(FLinearColor::Black);
                Canvas->DrawItem(TextItem);
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

        if (PieMenu.IsOpen() && InViewport && Canvas && !Canvas->IsHitTesting())
        {
            const double DPIScale = FMath::Max(static_cast<double>(Canvas->GetDPIScale()), 0.01);
            const FVector2D ViewLogical = FVector2D(InViewport->GetSizeXY()) / DPIScale;
            PieMenu.Arrange(ViewLogical);
            PieMenu.Draw(*Canvas, ViewLogical);
        }
    }

private:
    enum class EPointerAction : uint8 { None, Select, EdgeLoop, Gizmo, SoftRadius };

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
        const FMatrix& ViewMatrix = View.ViewMatrices.GetWorldToView();
        const FVector CamRight = ViewMatrix.GetColumn(0).GetSafeNormal();
        const FVector CamUp = ViewMatrix.GetColumn(1).GetSafeNormal();
        const FVector CamForward = ViewMatrix.GetColumn(2).GetSafeNormal();
        const FVector CamOrigin = View.ViewMatrices.GetViewOrigin();

        const double ClipW = View.WorldToScreen(Origin).W;
        const double ProjectionWidth = static_cast<double>(View.UnscaledViewRect.Width())
            * FMath::Abs(View.ViewMatrices.GetViewToClip().M[0][0]);
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
        // Gizmo has highest interaction priority: when the cursor is inside any gizmo handle or rotation area,
        // consume the pointer action for the gizmo and never fall through to mesh/bone picking or marquee selection.
        if (PreviewOwner->HasGizmoTarget())
        {
            FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
            if (const FSceneView* View = BuildSceneView(InViewport, ViewFamily))
            {
                BuildGizmo(*View);
                if (ForegroundGizmo.BeginDrag(FVector2D(Mouse.X, Mouse.Y)))
                {
                    const FTransform Start = PreviewOwner->GetSelectionGizmoTransform();
                    PreviewOwner->BeginSoftTransform(Start, PreviewOwner->GetSoftTransformMode());
                    PointerAction = EPointerAction::Gizmo;
                    LastGizmoMouse = Mouse;
                    PostInvalidation();
                    return;
                }
            }
        }

        const bool bBoneMode = NGon::IsBoneGizmoActive(&PreviewOwner.Get());
        if (IsCtrlPressed() && !bBoneMode)
        {
            PointerAction = EPointerAction::EdgeLoop;
            return;
        }

        PointerAction = EPointerAction::Select;
        MarqueeStart = FVector2D(Mouse.X, Mouse.Y);
        MarqueeEnd = MarqueeStart;
    }

    bool MovePointer(const FIntPoint& Mouse)
    {
        if (PointerAction == EPointerAction::SoftRadius)
        {
            UpdateSoftRadiusDrag(Mouse);
            return true;
        }
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
        if (PointerAction == EPointerAction::SoftRadius)
        {
            UpdateSoftRadiusDrag(Mouse);
        }
        else if (PointerAction == EPointerAction::Gizmo)
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

    bool HandleDoubleClickGesture(FViewport* InViewport, const FIntPoint& Mouse)
    {
        const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin();
        if (!InViewport || !Pinned)
        {
            return false;
        }
        FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
        const FSceneView* View = BuildSceneView(InViewport, ViewFamily);
        if (!View)
        {
            return false;
        }

        const FVector2D Cursor(Mouse.X, Mouse.Y);
        const ENGonElement Mode = Pinned->GetSelectionMode();
        const int32 PrevAnchor = (PreviewOwner->GetSelectedElement() == Mode && !PreviewOwner->GetSelectedIds().IsEmpty())
            ? (PreviewOwner->GetActiveSelectedId() != INDEX_NONE ? PreviewOwner->GetActiveSelectedId() : *PreviewOwner->GetSelectedIds().CreateConstIterator())
            : INDEX_NONE;

        const TSet<int32> SavedSel = PreviewOwner->GetSelectedIds();
        const ENGonElement SavedElem = PreviewOwner->GetSelectedElement();

        const int32 TargetId = Pinned->Pick(*View, Cursor, IsShiftPressed());
        if (TargetId == INDEX_NONE)
        {
            return false;
        }
        int32 EffectiveAnchor = PrevAnchor;
        if ((EffectiveAnchor == INDEX_NONE || EffectiveAnchor == TargetId)
            && PreviousSelectionAnchorElement == Mode
            && PreviousSelectionAnchor != INDEX_NONE
            && PreviousSelectionAnchor != TargetId)
        {
            EffectiveAnchor = PreviousSelectionAnchor;
        }

        // Release any active joint in hierarchy when double-clicking mesh components
        if (NGon::GetSelectedSceneBone(&PreviewOwner.Get()) != INDEX_NONE || NGon::IsBoneGizmoActive(&PreviewOwner.Get()))
        {
            NGon::SelectSceneBone(&PreviewOwner.Get(), INDEX_NONE, false);
            if (FNGonSkeletonTreeUIState* UI = GetUIState(); UI && UI->OnViewportBonePicked)
            {
                UI->OnViewportBonePicked(INDEX_NONE);
            }
        }

        // Restore previous selection if Shift-adding so SelectDoubleClickGesture can merge cleanly
        if (IsShiftPressed() && SavedElem == Mode && !SavedSel.IsEmpty())
        {
            PreviewOwner->SetSelectionSet(Mode, SavedSel, false);
        }

        FNGonMeshAPI::SelectDoubleClickGesture(PreviewOwner.Get(), Mode, EffectiveAnchor, TargetId, IsShiftPressed());
        PreviousSelectionAnchor = INDEX_NONE;
        PostInvalidation();
        return true;
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
        const auto ReleaseSelectedBone = [&]()
        {
            if (NGon::GetSelectedSceneBone(&PreviewOwner.Get()) != INDEX_NONE || NGon::IsBoneGizmoActive(&PreviewOwner.Get()))
            {
                NGon::SelectSceneBone(&PreviewOwner.Get(), INDEX_NONE, false);
                if (UI && UI->OnViewportBonePicked)
                {
                    UI->OnViewportBonePicked(INDEX_NONE);
                }
            }
        };

        if (!bMarqueeActive && bBonesVisible)
        {
            // Keep a tight radius (11px) so clicking mesh components near a bone selects the mesh instead of stealing focus
            const float PickRadius = NGon::IsBoneGizmoActive(&PreviewOwner.Get()) ? 14.0f : 10.0f;
            const int32 HitBone = NGon::PickSceneBone(&PreviewOwner.Get(), View, Mouse, PickRadius, true);
            if (HitBone != INDEX_NONE)
            {
                PreviewOwner->ClearSelection();
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

        // Single click or marquee on mesh/empty space releases any selected joint in hierarchy
        ReleaseSelectedBone();

        if (bMarqueeActive)
        {
            Pinned->BoxSelect(View, MakeMarquee(), IsShiftPressed(), MarqueeEnd.X >= MarqueeStart.X);
        }
        else
        {
            const bool bAdd = IsShiftPressed();
            const ENGonElement CurMode = Pinned->GetSelectionMode();
            const int32 PreClickAnchor = (PreviewOwner->GetSelectedElement() == CurMode && !PreviewOwner->GetSelectedIds().IsEmpty())
                ? (PreviewOwner->GetActiveSelectedId() != INDEX_NONE ? PreviewOwner->GetActiveSelectedId() : *PreviewOwner->GetSelectedIds().CreateConstIterator())
                : INDEX_NONE;
            const int32 Hit = Pinned->Pick(View, Mouse, bAdd);
            if (Hit != INDEX_NONE && PreClickAnchor != INDEX_NONE && PreClickAnchor != Hit)
            {
                PreviousSelectionAnchor = PreClickAnchor;
                PreviousSelectionAnchorElement = CurMode;
            }
            if (PointerAction == EPointerAction::EdgeLoop && CurMode == ENGonElement::Edge && Hit != INDEX_NONE)
            {
                FNGonMeshAPI::SelectEdgeLoop(PreviewOwner.Get(), Hit, bAdd);
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
        if (PointerAction == EPointerAction::SoftRadius)
        {
            FNGonSoftSelectionSettings Settings = PreviewOwner->GetSoftSelectionSettings();
            Settings.Radius = SoftRadiusStartValue;
            PreviewOwner->SetSoftSelection(Settings);
        }
        bBKeyHeld = false;
        bBKeyDragged = false;
        PointerAction = EPointerAction::None;
        bPendingGizmoMove = false;
        bMarqueeActive = false;
        PostInvalidation();
    }

    void DrawHoveredVertex(const FSceneView& View, FCanvas& Canvas, const FVector2D& CursorPx) const
    {
        const TArray<FVector>& Positions = PreviewOwner->GetPositions();
        if (Positions.IsEmpty() || Positions.Num() > 60000)
        {
            return;
        }
        const bool bAllowThroughMesh = NGon::IsXRayEnabled(&PreviewOwner.Get()) || NGon::IsWireframeEdgesOnly(&PreviewOwner.Get()) || (GetViewMode() == VMI_Wireframe);
        const int32 BestVert = PreviewOwner->PickHoverVertex(View, CursorPx, bAllowThroughMesh);
        if (!Positions.IsValidIndex(BestVert))
        {
            return;
        }
        FVector2D BestPx = FVector2D::ZeroVector;
        if (!View.WorldToPixel(Positions[BestVert], BestPx))
        {
            return;
        }
        const double DPIScale = FMath::Max(static_cast<double>(Canvas.GetDPIScale()), 0.01);

        const FVector2D Center = BestPx / DPIScale;
        const bool bAlreadySelected = PreviewOwner->GetSelectedElement() == ENGonElement::Vertex
            && PreviewOwner->GetSelectedIds().Contains(BestVert);
        const FLinearColor DotColor = bAlreadySelected
            ? FLinearColor(1.0f, 0.94f, 0.25f, 1.0f)
            : FLinearColor(0.98f, 0.86f, 0.22f, 1.0f);
        const FLinearColor RingColor(0.82f, 0.45f, 1.0f, 0.95f);

        // Crisp center square (~5px = 5/4 normal vertex size) + subtle outer highlight box
        FCanvasTileItem DotTile(Center - FVector2D(2.5, 2.5), FVector2D(5.0, 5.0), DotColor);
        DotTile.BlendMode = SE_BLEND_Translucent;
        Canvas.DrawItem(DotTile);

        FCanvasBoxItem RingBox(Center - FVector2D(4.5, 4.5), FVector2D(9.0, 9.0));
        RingBox.SetColor(RingColor);
        RingBox.LineThickness = 1.5f;
        Canvas.DrawItem(RingBox);
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
    void ToggleSoftSelectionHotkey()
    {
        FNGonSoftSelectionSettings Settings = PreviewOwner->GetSoftSelectionSettings();
        Settings.bEnabled = !Settings.bEnabled;
        if (Settings.bEnabled && Settings.Radius <= 0.1)
        {
            const FBox Bounds = PreviewOwner->GetMeshBounds();
            Settings.Radius = Bounds.IsValid ? FMath::Max(Bounds.GetExtent().Size() * 0.25, 10.0) : 50.0;
        }
        PreviewOwner->SetSoftSelection(Settings);
        if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
        {
            if (!Settings.bEnabled)
            {
                Pinned->SetHeatmapVisible(false);
            }
        }
        PostInvalidation();
    }

    void BeginSoftRadiusDrag(FViewport* InViewport, const FIntPoint& Mouse)
    {
        bBKeyDragged = true;
        PointerAction = EPointerAction::SoftRadius;
        SoftRadiusStartMouse = Mouse;

        FNGonSoftSelectionSettings Settings = PreviewOwner->GetSoftSelectionSettings();
        if (!Settings.bEnabled)
        {
            Settings.bEnabled = true;
            if (Settings.Radius <= 0.1)
            {
                const FBox Bounds = PreviewOwner->GetMeshBounds();
                Settings.Radius = Bounds.IsValid ? FMath::Max(Bounds.GetExtent().Size() * 0.25, 10.0) : 50.0;
            }
            PreviewOwner->SetSoftSelection(Settings);
        }
        SoftRadiusStartValue = FMath::Max(Settings.Radius, 0.5);

        // Compute world-units-per-pixel at the selection or mesh center so left/right drag feels 1:1 with screen scale
        const FBox Bounds = PreviewOwner->GetMeshBounds();
        const FVector Pivot = PreviewOwner->HasGizmoTarget()
            ? PreviewOwner->GetGizmoDisplayTransform().GetLocation()
            : (Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector);

        SoftRadiusUnitsPerPixel = Bounds.IsValid ? FMath::Max(Bounds.GetExtent().Size() / 300.0, 0.1) : 0.5;
        FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
        if (const FSceneView* View = BuildSceneView(InViewport, ViewFamily))
        {
            const double ClipW = View->WorldToScreen(Pivot).W;
            const double ProjW = static_cast<double>(View->UnscaledViewRect.Width())
                * FMath::Abs(View->ViewMatrices.GetViewToClip().M[0][0]);
            if (ClipW > 0.0 && ProjW > 1e-4)
            {
                SoftRadiusUnitsPerPixel = 2.0 * ClipW / ProjW;
            }
        }


        PostInvalidation();
    }

    void UpdateSoftRadiusDrag(const FIntPoint& Mouse)
    {
        const double DeltaX = static_cast<double>(Mouse.X - SoftRadiusStartMouse.X);
        const FBox Bounds = PreviewOwner->GetMeshBounds();
        const double MaxRadius = Bounds.IsValid ? FMath::Max(Bounds.GetSize().GetMax() * 4.0, 500.0) : 5000.0;
        const double NewRadius = FMath::Clamp(SoftRadiusStartValue + DeltaX * SoftRadiusUnitsPerPixel, 0.5, MaxRadius);

        FNGonSoftSelectionSettings Settings = PreviewOwner->GetSoftSelectionSettings();
        Settings.bEnabled = true;
        Settings.Radius = NewRadius;
        PreviewOwner->SetSoftSelection(Settings);

        PostInvalidation();
    }

    bool bPendingGizmoMove = false;
    bool bMarqueeActive = false;
    bool bBKeyHeld = false;
    bool bBKeyDragged = false;
    FIntPoint SoftRadiusStartMouse = FIntPoint::ZeroValue;
    double SoftRadiusStartValue = 50.0;
    double SoftRadiusUnitsPerPixel = 0.5;
    int32 PreviousSelectionAnchor = INDEX_NONE;
    ENGonElement PreviousSelectionAnchorElement = ENGonElement::Face;
    double LastLmbClickTime = 0.0;
    FIntPoint LastLmbClickPos = FIntPoint::ZeroValue;
    FNGonPieMenu PieMenu;
    FVector2D PieOpenPos = FVector2D::ZeroVector;
    bool bTrackingRmbTap = false;
    bool bRmbDragged = false;
    FIntPoint RmbDownMouse = FIntPoint::ZeroValue;

    void OpenViewportPieMenu(FViewport* InViewport)
    {
        if (!InViewport) return;
        const double DPI = FMath::Max(static_cast<double>(GetDPIScale()), 0.01);
        const FVector2D ViewSize = FVector2D(InViewport->GetSizeXY()) / DPI;
        const FVector2D MousePos = FVector2D(InViewport->GetMouseX(), InViewport->GetMouseY()) / DPI;
        PieOpenPos = MousePos;

        const bool bBoneActive = NGon::IsBoneGizmoActive(&PreviewOwner.Get());
        const bool bHasSelection = !PreviewOwner->GetSelectedIds().IsEmpty();
        const ENGonElement Elem = PreviewOwner->GetSelectedElement();
        const bool bCanUndo = FNGonMeshAPI::CanUndo(&PreviewOwner.Get());
        const bool bCanRedo = FNGonMeshAPI::CanRedo(&PreviewOwner.Get());

        TArray<FNGonPieItem> RootItems;

        if (bBoneActive)
        {
            // Context 1: Skeleton Bone / Joint Pie Menu
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Translate Joint (W)"), TEXT("MOV"), TEXT("gizmo:translate")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Rotate Joint (E)"), TEXT("ROT"), TEXT("gizmo:rotate")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Scale Joint (R)"), TEXT("SCL"), TEXT("gizmo:scale")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Toggle Local/World"), TEXT("SPC"), TEXT("bone:toggle_space")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Reset Bone (Ctrl+I)"), TEXT("RST"), TEXT("bone:reset")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Reset All Bones"), TEXT("RALL"), TEXT("bone:reset_all")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Frame Joint (F)"), TEXT("FRM"), TEXT("view:frame")));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Mesh Modes"), TEXT("MESH"), {
                FNGonPieItem::Leaf(TEXT("Vertex Mode (1)"), TEXT("VTX"), TEXT("mode:vertex")),
                FNGonPieItem::Leaf(TEXT("Edge Mode (2)"), TEXT("EDG"), TEXT("mode:edge")),
                FNGonPieItem::Leaf(TEXT("Face Mode (3)"), TEXT("FAC"), TEXT("mode:face")),
                FNGonPieItem::Leaf(TEXT("Open UV Editor"), TEXT("UV"), TEXT("view:uv_editor"))
            }));
        }
        else if (bHasSelection && Elem == ENGonElement::Vertex)
        {
            // Context 2: Vertex Component Pie Menu
            const int32 SelCount = PreviewOwner->GetSelectedIds().Num();
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Expand / Grow Selection"), TEXT("GROW"), TEXT("sel:grow")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Shrink Selection"), TEXT("SHRK"), TEXT("sel:shrink")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Vertex Loop (2 Shared Edge)"), TEXT("LOOP"), TEXT("sel:vtx_loop"), SelCount >= 2));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Shortest Path (2 Vtx)"), TEXT("PATH"), TEXT("sel:shortest_path"), SelCount >= 2));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Weld Vertices"), TEXT("WELD"), TEXT("mesh:weld"), SelCount >= 2));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Cut Face (2 Vtx)"), TEXT("CUT"), TEXT("mesh:cut_face"), SelCount >= 2));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Select / Edit"), TEXT("SEL"), {
                FNGonPieItem::Leaf(TEXT("Fill Hole (3+ Vtx)"), TEXT("FIL"), TEXT("mesh:fill_hole"), SelCount >= 3),
                FNGonPieItem::Leaf(TEXT("Smooth Vertices"), TEXT("SMT"), TEXT("mesh:smooth")),
                FNGonPieItem::Leaf(TEXT("Linked Shell"), TEXT("SHL"), TEXT("sel:linked")),
                FNGonPieItem::Leaf(TEXT("Invert Selection"), TEXT("INV"), TEXT("sel:invert")),
                FNGonPieItem::Leaf(TEXT("Convert -> Edges"), TEXT("EDG"), TEXT("sel:to_edge")),
                FNGonPieItem::Leaf(TEXT("Convert -> Faces"), TEXT("FAC"), TEXT("sel:to_face")),
                FNGonPieItem::Leaf(TEXT("Clear Selection"), TEXT("CLR"), TEXT("sel:clear"))
            }));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Mode & Tools"), TEXT("TOOL"), {
                FNGonPieItem::Leaf(TEXT("Toggle Soft Select (B)"), TEXT("SFT"), TEXT("soft:toggle")),
                FNGonPieItem::Leaf(TEXT("Edge Mode (2)"), TEXT("EDG"), TEXT("mode:edge")),
                FNGonPieItem::Leaf(TEXT("Face Mode (3)"), TEXT("FAC"), TEXT("mode:face")),
                FNGonPieItem::Leaf(TEXT("Translate (W)"), TEXT("MOV"), TEXT("gizmo:translate")),
                FNGonPieItem::Leaf(TEXT("Rotate (E)"), TEXT("ROT"), TEXT("gizmo:rotate")),
                FNGonPieItem::Leaf(TEXT("Scale (R)"), TEXT("SCL"), TEXT("gizmo:scale")),
                FNGonPieItem::Leaf(TEXT("Undo"), TEXT("UND"), TEXT("mesh:undo"), bCanUndo),
                FNGonPieItem::Leaf(TEXT("Redo"), TEXT("RDO"), TEXT("mesh:redo"), bCanRedo)
            }));
        }
        else if (bHasSelection && Elem == ENGonElement::Edge)
        {
            // Context 3: Edge Component Pie Menu
            const int32 SelCount = PreviewOwner->GetSelectedIds().Num();
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Edge Loop"), TEXT("LOOP"), TEXT("sel:edge_loop")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Edge Ring"), TEXT("RING"), TEXT("sel:edge_ring")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Split Edge"), TEXT("SPLT"), TEXT("mesh:split_edge")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Collapse Edge"), TEXT("CLPS"), TEXT("mesh:collapse_edge")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Bridge Edges (2)"), TEXT("BRDG"), TEXT("mesh:bridge"), SelCount >= 2));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Expand / Select"), TEXT("SEL"), {
                FNGonPieItem::Leaf(TEXT("Shortest Path (2 Edges)"), TEXT("PTH"), TEXT("sel:shortest_path"), SelCount >= 2),
                FNGonPieItem::Leaf(TEXT("Expand / Grow Selection"), TEXT("GRW"), TEXT("sel:grow")),
                FNGonPieItem::Leaf(TEXT("Shrink Selection"), TEXT("SHR"), TEXT("sel:shrink")),
                FNGonPieItem::Leaf(TEXT("Boundary Edges"), TEXT("BND"), TEXT("sel:boundary")),
                FNGonPieItem::Leaf(TEXT("Linked Shell"), TEXT("SHL"), TEXT("sel:linked")),
                FNGonPieItem::Leaf(TEXT("Invert Selection"), TEXT("INV"), TEXT("sel:invert")),
                FNGonPieItem::Leaf(TEXT("Clear Selection"), TEXT("CLR"), TEXT("sel:clear"))
            }));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Convert"), TEXT("CONV"), {
                FNGonPieItem::Leaf(TEXT("Convert -> Vertices"), TEXT("VTX"), TEXT("sel:to_vertex")),
                FNGonPieItem::Leaf(TEXT("Convert -> Faces"), TEXT("FAC"), TEXT("sel:to_face")),
                FNGonPieItem::Leaf(TEXT("Select Face Loop"), TEXT("FLP"), TEXT("sel:face_loop"))
            }));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Mode & Tools"), TEXT("TOOL"), {
                FNGonPieItem::Leaf(TEXT("Toggle Soft Select (B)"), TEXT("SFT"), TEXT("soft:toggle")),
                FNGonPieItem::Leaf(TEXT("Vertex Mode (1)"), TEXT("VTX"), TEXT("mode:vertex")),
                FNGonPieItem::Leaf(TEXT("Face Mode (3)"), TEXT("FAC"), TEXT("mode:face")),
                FNGonPieItem::Leaf(TEXT("Translate (W)"), TEXT("MOV"), TEXT("gizmo:translate")),
                FNGonPieItem::Leaf(TEXT("Rotate (E)"), TEXT("ROT"), TEXT("gizmo:rotate")),
                FNGonPieItem::Leaf(TEXT("Scale (R)"), TEXT("SCL"), TEXT("gizmo:scale")),
                FNGonPieItem::Leaf(TEXT("Undo"), TEXT("UND"), TEXT("mesh:undo"), bCanUndo),
                FNGonPieItem::Leaf(TEXT("Redo"), TEXT("RDO"), TEXT("mesh:redo"), bCanRedo)
            }));
        }
        else if (bHasSelection && Elem == ENGonElement::Face)
        {
            // Context 4: Face Component Pie Menu
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Extrude Faces"), TEXT("EXTR"), TEXT("mesh:extrude")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Inset Faces"), TEXT("INST"), TEXT("mesh:inset")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Bevel Faces"), TEXT("BEVL"), TEXT("mesh:bevel")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Subdivide Faces"), TEXT("SUBD"), TEXT("mesh:subdivide")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Delete Faces"), TEXT("DEL"), TEXT("mesh:delete")));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Face Actions"), TEXT("FACE"), {
                FNGonPieItem::Leaf(TEXT("Flip Normals"), TEXT("FLP"), TEXT("mesh:flip")),
                FNGonPieItem::Leaf(TEXT("Triangulate"), TEXT("TRI"), TEXT("mesh:triangulate")),
                FNGonPieItem::Leaf(TEXT("Smooth Region"), TEXT("SMT"), TEXT("mesh:smooth")),
                FNGonPieItem::Leaf(TEXT("Noise Displace"), TEXT("NSE"), TEXT("mesh:noise"))
            }));
            const int32 FaceSelCount = PreviewOwner->GetSelectedIds().Num();
            RootItems.Add(FNGonPieItem::Branch(TEXT("Expand / Select"), TEXT("SEL"), {
                FNGonPieItem::Leaf(TEXT("Face Loop (2 Shared Edge)"), TEXT("FLP"), TEXT("sel:face_loop")),
                FNGonPieItem::Leaf(TEXT("Shortest Path (2 Faces)"), TEXT("PTH"), TEXT("sel:shortest_path"), FaceSelCount >= 2),
                FNGonPieItem::Leaf(TEXT("Expand / Grow Selection"), TEXT("GRW"), TEXT("sel:grow")),
                FNGonPieItem::Leaf(TEXT("Shrink Selection"), TEXT("SHR"), TEXT("sel:shrink")),
                FNGonPieItem::Leaf(TEXT("Linked Shell"), TEXT("SHL"), TEXT("sel:linked")),
                FNGonPieItem::Leaf(TEXT("Invert Selection"), TEXT("INV"), TEXT("sel:invert")),
                FNGonPieItem::Leaf(TEXT("Convert -> Vertices"), TEXT("VTX"), TEXT("sel:to_vertex")),
                FNGonPieItem::Leaf(TEXT("Convert -> Edges"), TEXT("EDG"), TEXT("sel:to_edge")),
                FNGonPieItem::Leaf(TEXT("Clear Selection"), TEXT("CLR"), TEXT("sel:clear"))
            }));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Mode & Tools"), TEXT("TOOL"), {
                FNGonPieItem::Leaf(TEXT("Toggle Soft Select (B)"), TEXT("SFT"), TEXT("soft:toggle")),
                FNGonPieItem::Leaf(TEXT("Vertex Mode (1)"), TEXT("VTX"), TEXT("mode:vertex")),
                FNGonPieItem::Leaf(TEXT("Edge Mode (2)"), TEXT("EDG"), TEXT("mode:edge")),
                FNGonPieItem::Leaf(TEXT("Translate (W)"), TEXT("MOV"), TEXT("gizmo:translate")),
                FNGonPieItem::Leaf(TEXT("Rotate (E)"), TEXT("ROT"), TEXT("gizmo:rotate")),
                FNGonPieItem::Leaf(TEXT("Scale (R)"), TEXT("SCL"), TEXT("gizmo:scale")),
                FNGonPieItem::Leaf(TEXT("Undo"), TEXT("UND"), TEXT("mesh:undo"), bCanUndo),
                FNGonPieItem::Leaf(TEXT("Redo"), TEXT("RDO"), TEXT("mesh:redo"), bCanRedo)
            }));
        }
        else
        {
            // Context 5: Object / Whole-Mesh Pie Menu (no component selected)
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Vertex Mode (1)"), TEXT("VTX"), TEXT("mode:vertex")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Edge Mode (2)"), TEXT("EDGE"), TEXT("mode:edge")));
            RootItems.Add(FNGonPieItem::Leaf(TEXT("Face Mode (3)"), TEXT("FACE"), TEXT("mode:face")));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Add Primitive"), TEXT("ADD"), {
                FNGonPieItem::Leaf(TEXT("+ Box (100x100)"), TEXT("BOX"), TEXT("prim:box")),
                FNGonPieItem::Leaf(TEXT("+ Plane (4x4)"), TEXT("PLN"), TEXT("prim:plane")),
                FNGonPieItem::Leaf(TEXT("+ Cylinder (16)"), TEXT("CYL"), TEXT("prim:cylinder")),
                FNGonPieItem::Leaf(TEXT("+ UV Sphere"), TEXT("SPH"), TEXT("prim:sphere"))
            }));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Object Modeling"), TEXT("OBJ"), {
                FNGonPieItem::Leaf(TEXT("Subdivide Mesh"), TEXT("SUB"), TEXT("mesh:subdivide")),
                FNGonPieItem::Leaf(TEXT("Triangulate Mesh"), TEXT("TRI"), TEXT("mesh:triangulate")),
                FNGonPieItem::Leaf(TEXT("Flip All Normals"), TEXT("FLP"), TEXT("mesh:flip")),
                FNGonPieItem::Leaf(TEXT("Mirror X"), TEXT("MIR"), TEXT("mesh:mirror_x")),
                FNGonPieItem::Leaf(TEXT("Smooth Mesh"), TEXT("SMT"), TEXT("mesh:smooth")),
                FNGonPieItem::Leaf(TEXT("Taper Z"), TEXT("TPR"), TEXT("mesh:taper")),
                FNGonPieItem::Leaf(TEXT("Twist Z (45 deg)"), TEXT("TWT"), TEXT("mesh:twist")),
                FNGonPieItem::Leaf(TEXT("Noise Displace"), TEXT("NSE"), TEXT("mesh:noise"))
            }));
            RootItems.Add(FNGonPieItem::Branch(TEXT("Quick Select"), TEXT("SEL"), {
                FNGonPieItem::Leaf(TEXT("Select All Faces"), TEXT("ALL"), TEXT("sel:all_faces")),
                FNGonPieItem::Leaf(TEXT("Select N-Gons (5+)"), TEXT("NGN"), TEXT("sel:ngons")),
                FNGonPieItem::Leaf(TEXT("Select Boundary Edges"), TEXT("BND"), TEXT("sel:boundary")),
                FNGonPieItem::Leaf(TEXT("Select Top Faces (+Z)"), TEXT("TOP"), TEXT("sel:top_faces"))
            }));
            RootItems.Add(FNGonPieItem::Branch(TEXT("History & Soft"), TEXT("HIST"), {
                FNGonPieItem::Leaf(TEXT("Undo Mesh Edit"), TEXT("UND"), TEXT("mesh:undo"), bCanUndo),
                FNGonPieItem::Leaf(TEXT("Redo Mesh Edit"), TEXT("RDO"), TEXT("mesh:redo"), bCanRedo),
                FNGonPieItem::Leaf(TEXT("Toggle Soft Select (B)"), TEXT("SFT"), TEXT("soft:toggle"))
            }));
            RootItems.Add(FNGonPieItem::Branch(TEXT("View & UV"), TEXT("VIEW"), {
                FNGonPieItem::Leaf(TEXT("Frame Mesh (F)"), TEXT("FRM"), TEXT("view:frame")),
                FNGonPieItem::Leaf(TEXT("Open UV Editor"), TEXT("UV"), TEXT("view:uv_editor")),
                FNGonPieItem::Leaf(TEXT("Toggle Heatmap (H)"), TEXT("HTM"), TEXT("view:heatmap")),
                FNGonPieItem::Leaf(TEXT("Translate (W)"), TEXT("MOV"), TEXT("gizmo:translate")),
                FNGonPieItem::Leaf(TEXT("Rotate (E)"), TEXT("ROT"), TEXT("gizmo:rotate")),
                FNGonPieItem::Leaf(TEXT("Scale (R)"), TEXT("SCL"), TEXT("gizmo:scale")),
                FNGonPieItem::Leaf(TEXT("Combined Gizmo"), TEXT("ALL"), TEXT("gizmo:combined"))
            }));
        }

        PieMenu.Open(MousePos, MoveTemp(RootItems), [this](const FString& Cmd)
        {
            ExecutePieCommand(Cmd);
        });
        PieMenu.Arrange(ViewSize);
        PostInvalidation();
    }

    void ExecutePieCommand(const FString& Cmd)
    {
        const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin();
        if (Cmd == TEXT("mode:vertex") && Pinned) Pinned->SetSelectionMode(ENGonElement::Vertex);
        else if (Cmd == TEXT("mode:edge") && Pinned) Pinned->SetSelectionMode(ENGonElement::Edge);
        else if (Cmd == TEXT("mode:face") && Pinned) Pinned->SetSelectionMode(ENGonElement::Face);
        else if (Cmd == TEXT("gizmo:translate") && Pinned) Pinned->SetGizmoMode(ENGonGizmoMode::Translate);
        else if (Cmd == TEXT("gizmo:rotate") && Pinned) Pinned->SetGizmoMode(ENGonGizmoMode::Rotate);
        else if (Cmd == TEXT("gizmo:scale") && Pinned) Pinned->SetGizmoMode(ENGonGizmoMode::Scale);
        else if (Cmd == TEXT("gizmo:combined") && Pinned) Pinned->SetGizmoMode(ENGonGizmoMode::Combined);
        else if (Cmd == TEXT("gizmo:none") && Pinned) Pinned->SetGizmoMode(ENGonGizmoMode::None);
        else if (Cmd == TEXT("view:frame") && Pinned) Pinned->FrameMesh();
        else if (Cmd == TEXT("view:heatmap") && Pinned) Pinned->ToggleHeatmap();
        else if (Cmd == TEXT("view:uv_editor")) FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("NGonUVEditor")));
        else if (Cmd == TEXT("soft:toggle")) ToggleSoftSelectionHotkey();
        else if (Cmd == TEXT("bone:toggle_space"))
        {
            const bool bWorld = !NGon::IsSceneWorldCoordinateSystem(&PreviewOwner.Get());
            NGon::SetSceneCoordinateSystem(&PreviewOwner.Get(), bWorld);
        }
        else if (Cmd == TEXT("bone:reset"))
        {
            if (NGon::ResetSelectedSceneBoneTransform(&PreviewOwner.Get())) NotifyBoneTransformChanged();
        }
        else if (Cmd == TEXT("bone:reset_all"))
        {
            if (NGon::ResetAllSceneBonesTransforms(&PreviewOwner.Get())) NotifyBoneTransformChanged();
        }
        else if (Cmd == TEXT("mesh:extrude")) FNGonMeshAPI::ExtrudeFaces(PreviewOwner.Get(), {}, 25.0, 1.0);
        else if (Cmd == TEXT("mesh:inset")) FNGonMeshAPI::InsetFaces(PreviewOwner.Get(), {}, 0.25, 0.0);
        else if (Cmd == TEXT("mesh:bevel")) FNGonMeshAPI::BevelFaces(PreviewOwner.Get(), {}, 0.18, 8.0);
        else if (Cmd == TEXT("mesh:subdivide")) FNGonMeshAPI::SubdivideFaces(PreviewOwner.Get(), {});
        else if (Cmd == TEXT("mesh:triangulate")) FNGonMeshAPI::TriangulateFaces(PreviewOwner.Get(), {});
        else if (Cmd == TEXT("mesh:flip")) FNGonMeshAPI::FlipFaces(PreviewOwner.Get(), {});
        else if (Cmd == TEXT("mesh:delete")) FNGonMeshAPI::DeleteFaces(PreviewOwner.Get(), {});
        else if (Cmd == TEXT("mesh:split_edge")) FNGonMeshAPI::SplitEdge(PreviewOwner.Get(), INDEX_NONE, 0.5);
        else if (Cmd == TEXT("mesh:collapse_edge")) FNGonMeshAPI::CollapseEdge(PreviewOwner.Get(), INDEX_NONE);
        else if (Cmd == TEXT("mesh:cut_face")) FNGonMeshAPI::CutFace(PreviewOwner.Get(), INDEX_NONE, INDEX_NONE, INDEX_NONE);
        else if (Cmd == TEXT("mesh:fill_hole")) FNGonMeshAPI::FillHole(PreviewOwner.Get(), {});
        else if (Cmd == TEXT("mesh:weld")) FNGonMeshAPI::WeldVertices(PreviewOwner.Get(), {}, 0.0);
        else if (Cmd == TEXT("mesh:bridge")) FNGonMeshAPI::BridgeEdges(PreviewOwner.Get(), INDEX_NONE, INDEX_NONE);
        else if (Cmd == TEXT("mesh:mirror_x")) FNGonMeshAPI::MirrorMesh(PreviewOwner.Get(), EAxis::X, true);
        else if (Cmd == TEXT("mesh:smooth")) FNGonMeshAPI::SmoothVertices(PreviewOwner.Get(), {}, 2, 0.5);
        else if (Cmd == TEXT("mesh:taper")) FNGonMeshAPI::ApplyTaper(PreviewOwner.Get(), EAxis::Z, 0.35);
        else if (Cmd == TEXT("mesh:twist")) FNGonMeshAPI::ApplyTwist(PreviewOwner.Get(), EAxis::Z, 45.0);
        else if (Cmd == TEXT("mesh:noise")) FNGonMeshAPI::ApplyNoise(PreviewOwner.Get(), {}, 6.0, 0.05, 1337);
        else if (Cmd == TEXT("mesh:undo")) FNGonMeshAPI::Undo(PreviewOwner.Get());
        else if (Cmd == TEXT("mesh:redo")) FNGonMeshAPI::Redo(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:grow")) FNGonMeshAPI::GrowSelection(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:shrink")) FNGonMeshAPI::ShrinkSelection(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:invert")) FNGonMeshAPI::InvertSelection(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:clear")) PreviewOwner->ClearSelection();
        else if (Cmd == TEXT("sel:vtx_loop")) FNGonMeshAPI::SelectVertexLoop(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:shortest_path")) FNGonMeshAPI::SelectShortestPath(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:edge_loop")) FNGonMeshAPI::SelectEdgeLoop(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:edge_ring")) FNGonMeshAPI::SelectEdgeRing(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:face_loop")) FNGonMeshAPI::SelectFaceLoop(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:linked")) FNGonMeshAPI::SelectLinkedShell(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:boundary")) FNGonMeshAPI::SelectBoundaryEdges(PreviewOwner.Get());
        else if (Cmd == TEXT("sel:ngons")) FNGonMeshAPI::SelectNGons(PreviewOwner.Get(), 5);
        else if (Cmd == TEXT("sel:all_faces")) FNGonMeshAPI::SelectAll(PreviewOwner.Get(), ENGonElement::Face);
        else if (Cmd == TEXT("sel:top_faces")) FNGonMeshAPI::SelectFacesByNormal(PreviewOwner.Get(), FVector::UpVector, 30.0);
        else if (Cmd == TEXT("sel:to_vertex")) FNGonMeshAPI::ConvertSelection(PreviewOwner.Get(), ENGonElement::Vertex);
        else if (Cmd == TEXT("sel:to_edge")) FNGonMeshAPI::ConvertSelection(PreviewOwner.Get(), ENGonElement::Edge);
        else if (Cmd == TEXT("sel:to_face")) FNGonMeshAPI::ConvertSelection(PreviewOwner.Get(), ENGonElement::Face);
        else if (Cmd == TEXT("prim:box")) { FNGonMeshAPI::CreateBox(PreviewOwner.Get()); if (Pinned) Pinned->FrameMesh(); }
        else if (Cmd == TEXT("prim:plane")) { FNGonMeshAPI::CreatePlane(PreviewOwner.Get()); if (Pinned) Pinned->FrameMesh(); }
        else if (Cmd == TEXT("prim:cylinder")) { FNGonMeshAPI::CreateCylinder(PreviewOwner.Get()); if (Pinned) Pinned->FrameMesh(); }
        else if (Cmd == TEXT("prim:sphere")) { FNGonMeshAPI::CreateSphere(PreviewOwner.Get()); if (Pinned) Pinned->FrameMesh(); }
        PostInvalidation();
    }
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
    const bool bEdgesOnly = (Mode == ENGonViewportShading::Wireframe);
    PreviewScene->SetFlatShading(Mode == ENGonViewportShading::Faceted);
    NGon::SetWireframeEdgesOnly(PreviewScene.Get(), bEdgesOnly);
    if (const TSharedPtr<FEditorViewportClient> PreviewClient = GetViewportClient())
    {
        // Keep VMI_Lit so Wireframe renders clean authored N-Gon edges only (no triangulated UE wireframe)
        PreviewClient->SetViewMode(VMI_Lit);
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
    if (Element == ENGonElement::Vertex)
    {
        PreviewScene->SetShowVertices(true);
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

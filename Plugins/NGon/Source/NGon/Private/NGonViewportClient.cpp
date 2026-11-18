#include "SNGonPreview.h"
#include "NGonSceneSkeleton.h"
#include "NGonUIIcons.h"

#include "AssetEditorModeManager.h"
#include "BaseGizmos/GizmoViewContext.h"
#include "BaseGizmos/TransformGizmoUtil.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "ContextObjectStore.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "InputRouter.h"
#include "InputState.h"
#include "InteractiveGizmoManager.h"
#include "InteractiveToolChange.h"
#include "InteractiveToolManager.h"
#include "InteractiveToolsContext.h"
#include "Materials/Material.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "Slate/SceneViewport.h"
#include "ToolContextInterfaces.h"
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

class FNGonGizmoRenderContext final : public IToolsContextRenderAPI
{
public:
    FNGonGizmoRenderContext(const FSceneView* InView, FPrimitiveDrawInterface* InPDI,
        const FViewCameraState& InCamera, EViewInteractionState InInteraction)
        : View(InView), PDI(InPDI), Camera(InCamera), Interaction(InInteraction)
    {
    }

    FPrimitiveDrawInterface* GetPrimitiveDrawInterface() override { return PDI; }
    const FSceneView* GetSceneView() override { return View; }
    FViewCameraState GetCameraState() override { return Camera; }
    EViewInteractionState GetViewInteractionState() override { return Interaction; }

private:
    const FSceneView* View;
    FPrimitiveDrawInterface* PDI;
    const FViewCameraState& Camera;
    EViewInteractionState Interaction;
};

class FNGonViewportClient final : private FNGonPreviewSceneOwner, public FEditorViewportClient,
    private IToolsContextQueriesAPI, private IToolsContextTransactionsAPI
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
        SetViewMode(VMI_Unlit);
        SetRealtime(true);
        SetAllowCinematicControl(false);
        EngineShowFlags.SetGrid(true);
        EngineShowFlags.SetSelectionOutline(false);
        EngineShowFlags.SetEyeAdaptation(false);
        EngineShowFlags.SetPostProcessing(false);
        EngineShowFlags.SetModeWidgets(true);
        ModeToolsOwner->SetWidgetMode(UE::Widget::WM_None);

        // This context uses the preview world and only receives NGon's mouse events.
        ToolsContext.Reset(NewObject<UInteractiveToolsContext>());
        ToolsContext->Initialize(this, this);
        UInteractiveToolManager* ToolManager = ToolsContext->ToolManager.Get();
        UContextObjectStore* ContextStore = ToolManager->GetContextObjectStore();
        UE::TransformGizmoUtil::RegisterTransformGizmoContextObject(ToolsContext.Get());
        GizmoViewContext = ContextStore->FindContext<UGizmoViewContext>();
        if (!GizmoViewContext)
        {
            GizmoViewContext = NewObject<UGizmoViewContext>(ToolsContext.Get());
            ContextStore->AddContextObject(GizmoViewContext);
        }

        PreviewOwner->SetOverlayMaterials(ToolManager);
        PreviewOwner->SetupGizmo(ToolsContext->GizmoManager.Get(), GizmoViewContext);
    }

    ~FNGonViewportClient() override
    {
        ToolsContext->InputRouter->ForceTerminateAll();
        PreviewOwner->TeardownGizmo();
        UE::TransformGizmoUtil::DeregisterTransformGizmoContextObject(ToolsContext.Get());
        ToolsContext->Shutdown();
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
        ToolsContext->GizmoManager->Tick(DeltaSeconds);
        if (bPendingGizmoMove && Viewport)
        {
            UpdateGizmoDrag(Viewport, PendingGizmoMouse);
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
        if (InViewport && !IsNavigating(InViewport) && PreviewOwner->HasGizmoTarget())
        {
            FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
            if (const FSceneView* View = BuildSceneView(InViewport, ViewFamily))
            {
                FInputDeviceState State = MakeMouseInput(*View, FIntPoint(X, Y), FVector2D::ZeroVector);
                ToolsContext->InputRouter->PostHoverInputEvent(State);
            }
        }
    }

    void CapturedMouseMove(FViewport* InViewport, int32 X, int32 Y) override
    {
        if (!MovePointer(FIntPoint(X, Y)))
        {
            FEditorViewportClient::CapturedMouseMove(InViewport, X, Y);
        }
    }

    void MouseEnter(FViewport* InViewport, int32 X, int32 Y) override
    {
        bMouseOverViewport = true;
        FEditorViewportClient::MouseEnter(InViewport, X, Y);
    }

    void MouseLeave(FViewport* InViewport) override
    {
        bMouseOverViewport = false;
        if (PointerAction == EPointerAction::None)
        {
            ToolsContext->InputRouter->ForceTerminateAll();
        }
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
        if (View)
        {
            UpdateViewContext(*View);
        }
        FEditorViewportClient::Draw(View, PDI);
        if (View && PDI)
        {
            if (!PDI->IsHitTesting() && PreviewOwner->HasGizmoTarget())
            {
                const EViewInteractionState Interaction = GetFocusedViewport()
                    ? (bMouseOverViewport ? EViewInteractionState::All : EViewInteractionState::Focused)
                    : (bMouseOverViewport ? EViewInteractionState::Hovered : EViewInteractionState::None);
                FNGonGizmoRenderContext Context(View, PDI, CameraState, Interaction);
                ToolsContext->GizmoManager->Render(&Context);
            }
            if (NGon::HasSceneBones(&PreviewOwner.Get()))
            {
                const FNGonSkeletonTreeUIState* UIState = GetUIState();
                NGon::DrawSceneSkeleton(&PreviewOwner.Get(), View, PDI, UIState ? UIState->BoneDrawMode : 0);
            }
        }
    }

    void Draw(FViewport* InViewport, FCanvas* Canvas) override
    {
        FEditorViewportClient::Draw(InViewport, Canvas);
        if (InViewport && Canvas && NGon::HasSceneBones(&PreviewOwner.Get()))
        {
            const FNGonSkeletonTreeUIState* UIState = GetUIState();
            if (!UIState || (UIState->bShowBoneNames && UIState->BoneDrawMode != 3))
            {
                FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
                if (FSceneView* View = BuildSceneView(InViewport, ViewFamily))
                {
                    NGon::DrawSceneSkeletonLabels(&PreviewOwner.Get(), View, InViewport, Canvas, true);
                }
            }
        }

        if (bMarqueeActive && Canvas)
        {
            const FBox2D Rect = MakeMarquee();
            FCanvasBoxItem Box(Rect.Min, Rect.GetSize());
            Box.SetColor(FLinearColor(0.15f, 0.65f, 1.0f, 0.85f));
            Box.LineThickness = 2.0f;
            Canvas->DrawItem(Box);
        }
    }

private:
    enum class EPointerAction : uint8 { None, Select, EdgeLoop, Gizmo };

    // The preview is transient. Gizmo changes are applied by the preview scene
    // and must not enter the level editor's undo stack as proxy-only changes.
    UWorld* GetCurrentEditingWorld() override { return PreviewOwner->GetPreviewScene().GetWorld(); }
    FViewport* GetFocusedViewport() override { return Viewport && Viewport->HasFocus() ? Viewport : nullptr; }
    FViewport* GetHoveredViewport() override { return bMouseOverViewport ? Viewport : nullptr; }
    void GetCurrentSelectionState(FToolBuilderState& State) const override
    {
        State = FToolBuilderState{};
        State.World = PreviewOwner->GetPreviewScene().GetWorld();
        State.ToolManager = ToolsContext->ToolManager.Get();
        State.GizmoManager = ToolsContext->GizmoManager.Get();
    }
    void GetCurrentViewState(FViewCameraState& State) const override { State = CameraState; }
    UMaterialInterface* GetStandardMaterial(EStandardToolContextMaterials MaterialType) const override
    {
        return UMaterial::GetDefaultMaterial(MD_Surface);
    }
    void DisplayMessage(const FText& Message, EToolMessageLevel Level) override {}
    void PostInvalidation() override { Invalidate(false, false); }
    void BeginUndoTransaction(const FText& Description) override {}
    void EndUndoTransaction() override {}
    void CancelUndoTransaction() override {}
    void AppendChange(UObject* Target, TUniquePtr<FToolCommandChange> Change, const FText& Description) override {}
    bool RequestSelectionChange(const FSelectedObjectsChangeList& Change) override { return false; }

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

    bool HandleViewportShortcut(SNGonViewport& Widget, const FKey& Key)
    {
        if (Key == EKeys::Q || Key == EKeys::W || Key == EKeys::E || Key == EKeys::R)
        {
            ENGonGizmoMode Mode = ENGonGizmoMode::None;
            if (Key == EKeys::W) Mode = ENGonGizmoMode::Translate;
            if (Key == EKeys::E) Mode = ENGonGizmoMode::Rotate;
            if (Key == EKeys::R) Mode = ENGonGizmoMode::Scale;
            Widget.SetGizmoMode(Mode);
        }
        else if (Key == EKeys::One || Key == EKeys::Two || Key == EKeys::Three)
        {
            Widget.SetSelectionMode(Key == EKeys::One ? ENGonElement::Vertex
                : Key == EKeys::Two ? ENGonElement::Edge : ENGonElement::Face);
        }
        else if (Key == EKeys::Four)
        {
            NGon::SetBoneGizmoActive(&PreviewOwner.Get(), true);
            Invalidate();
        }
        else if (Key == EKeys::H)
        {
            Widget.ToggleHeatmap();
        }
        else if (Key == EKeys::F)
        {
            Widget.FrameMesh();
        }
        else if (Key == EKeys::Delete || Key == EKeys::BackSpace)
        {
            Widget.DeleteSelectedFaces();
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
        FSceneView* View = CalcSceneView(&ViewFamily);
        if (View)
        {
            UpdateViewContext(*View);
        }
        return View;
    }

    void UpdateViewContext(const FSceneView& View)
    {
        GizmoViewContext->ResetFromSceneView(View);
        CameraState.DPIScale = GetDPIScale();
        GizmoViewContext->SetDPIScale(CameraState.DPIScale);
        CameraState.Position = View.ViewLocation;
        CameraState.Orientation = View.ViewRotation.Quaternion();
        CameraState.HorizontalFOVDegrees = ViewFOV;
        CameraState.AspectRatio = static_cast<float>(View.UnscaledViewRect.Width()) / FMath::Max(1, View.UnscaledViewRect.Height());
        CameraState.bIsOrthographic = !View.IsPerspectiveProjection();
        CameraState.OrthoWorldCoordinateWidth = CameraState.bIsOrthographic && Viewport
            ? GetOrthoUnitsPerPixel(Viewport) * View.UnscaledViewRect.Width() : 0.0f;
    }

    FInputDeviceState MakeMouseInput(const FSceneView& View, const FIntPoint& Mouse, const FVector2D& Delta) const
    {
        const FVector2D Pixel(Mouse.X, Mouse.Y);
        FVector Origin;
        FVector Direction;
        View.DeprojectFVector2D(Pixel, Origin, Direction);

        FInputDeviceState State{};
        State.InputDevice = EInputDevices::Mouse;
        State.SetModifierKeyStates(IsShiftPressed(), IsAltPressed(), IsCtrlPressed(), false);
        State.Mouse.Position2D = Pixel;
        State.Mouse.Delta2D = Delta;
        State.Mouse.WorldRay = FRay(Origin, Direction);
        return State;
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
                FInputDeviceState State = MakeMouseInput(*View, Mouse, FVector2D::ZeroVector);
                State.Mouse.Left.SetStates(true, true, false);
                ToolsContext->InputRouter->PostInputEvent(State);
                if (ToolsContext->InputRouter->HasActiveMouseCapture())
                {
                    PointerAction = EPointerAction::Gizmo;
                    LastGizmoMouse = Mouse;
                    PostInvalidation();
                    return;
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

    void UpdateGizmoDrag(FViewport* InViewport, const FIntPoint& Mouse)
    {
        bPendingGizmoMove = false;
        if (Mouse == LastGizmoMouse)
        {
            return;
        }

        FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
        if (const FSceneView* View = BuildSceneView(InViewport, ViewFamily))
        {
            const FIntPoint Delta = Mouse - LastGizmoMouse;
            FInputDeviceState State = MakeMouseInput(*View, Mouse, FVector2D(Delta.X, Delta.Y));
            State.Mouse.Left.SetStates(false, true, false);
            ToolsContext->InputRouter->PostInputEvent(State);
            LastGizmoMouse = Mouse;
            if (NGon::IsBoneTransformActive(&PreviewOwner.Get()))
            {
                NotifyBoneTransformChanged();
            }
            PostInvalidation();
        }
    }

    void FinishPointerAction(FViewport* InViewport, const FIntPoint& Mouse)
    {
        if (PointerAction == EPointerAction::Gizmo)
        {
            UpdateGizmoDrag(InViewport, Mouse);
            FSceneViewFamilyContext ViewFamily(MakeViewFamily(InViewport));
            if (const FSceneView* View = BuildSceneView(InViewport, ViewFamily))
            {
                FInputDeviceState State = MakeMouseInput(*View, Mouse, FVector2D::ZeroVector);
                State.Mouse.Left.SetStates(false, false, true);
                ToolsContext->InputRouter->PostInputEvent(State);
                const bool bBoneDrag = NGon::IsBoneTransformActive(&PreviewOwner.Get());
                if (bBoneDrag || PreviewOwner->IsSoftTransformActive())
                {
                    PreviewOwner->EndSoftTransform();
                }
                if (bBoneDrag)
                {
                    NotifyBoneTransformChanged();
                }
            }
            else
            {
                CancelPointerAction();
                return;
            }
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

        if (NGon::IsBoneGizmoActive(&PreviewOwner.Get()))
        {
            FNGonSkeletonTreeUIState* UI = GetUIState();
            if (!UI || UI->BoneDrawMode != 3)
            {
                const int32 HitBone = NGon::PickSceneBone(&PreviewOwner.Get(), View, Mouse, 20.0f, true);
                if (HitBone != INDEX_NONE && UI && UI->OnViewportBonePicked)
                {
                    UI->OnViewportBonePicked(HitBone);
                }
            }
        }
        else if (bMarqueeActive)
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
        ToolsContext->InputRouter->ForceTerminateAll();
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
    TStrongObjectPtr<UInteractiveToolsContext> ToolsContext;
    UGizmoViewContext* GizmoViewContext = nullptr;
    FViewCameraState CameraState{};
    FBox PendingFrameBounds{ForceInit};

    EPointerAction PointerAction = EPointerAction::None;
    FIntPoint LastGizmoMouse = FIntPoint::ZeroValue;
    FIntPoint PendingGizmoMouse = FIntPoint::ZeroValue;
    FVector2D MarqueeStart = FVector2D::ZeroVector;
    FVector2D MarqueeEnd = FVector2D::ZeroVector;
    bool bPendingGizmoMove = false;
    bool bMarqueeActive = false;
    bool bMouseOverViewport = false;
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

void SNGonViewport::SetMesh(FNGonMeshData&& Data)
{
    bHeatmapVisible = false;
    NGon::ClearSceneSkeleton(PreviewScene.Get());
    PreviewScene->SetMesh(MoveTemp(Data));
    if (const TSharedPtr<FEditorViewportClient> Client = GetViewportClient())
    {
        static_cast<FNGonViewportClient*>(Client.Get())->FrameBounds(PreviewScene->GetMeshBounds(), true);
    }
}

void SNGonViewport::FrameMesh()
{
    if (const TSharedPtr<FEditorViewportClient> Client = GetViewportClient())
    {
        static_cast<FNGonViewportClient*>(Client.Get())->FrameBounds(PreviewScene->GetSelectionOrMeshBounds(), false);
    }
}

void SNGonViewport::FocusViewport()
{
    if (const TSharedPtr<FSceneViewport> SceneViewport = GetSceneViewport())
    {
        if (const TSharedPtr<SViewport> Widget = SceneViewport->GetViewportWidget().Pin())
        {
            FSlateApplication::Get().SetKeyboardFocus(Widget, EFocusCause::SetDirectly);
        }
    }
}

void SNGonViewport::SetShadingMode(ENGonViewportShading Mode)
{
    ShadingMode = Mode;
    PreviewScene->SetFlatShading(Mode == ENGonViewportShading::Faceted);
    if (const TSharedPtr<FEditorViewportClient> Client = GetViewportClient())
    {
        Client->SetViewMode(Mode == ENGonViewportShading::Wireframe ? VMI_Wireframe : VMI_Unlit);
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

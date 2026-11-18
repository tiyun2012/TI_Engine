#include "SNGonPreview.h"
#include "NGonSceneSkeleton.h"
#include "NGonUIIcons.h"

#include "AssetEditorModeManager.h"
#include "BaseGizmos/GizmoViewContext.h"
#include "BaseGizmos/TransformGizmoUtil.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "ContextObjectStore.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "UnrealWidget.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "InputRouter.h"
#include "InputState.h"
#include "InteractiveGizmoManager.h"
#include "InteractiveToolManager.h"
#include "SceneView.h"
#include "Tools/EdModeInteractiveToolsContext.h"

namespace NGon
{
void SetActiveViewportUVMesh(const FNGonMeshData& Data, const FText& InSourceName, const FText& InSourceType);
void UpdateActiveViewportUVMeshFromTopology(const FNGonTopology& Topology);
void NotifyHostViewportClosed();
}

using NGon::FNGonSkeletonTreeUIState;
using NGon::GetSkeletonTreeUIState;
using NGon::FindUIStateByViewport;

#define LOCTEXT_NAMESPACE "NGonViewportClient"

namespace
{
struct FNGonPreviewSceneOwner
{
    explicit FNGonPreviewSceneOwner(const TSharedRef<FNGonPreviewScene>& InPreview)
        : PreviewOwner(InPreview)
        , ModeToolsOwner(MakeShared<FAssetEditorModeManager>())
    {
        ModeToolsOwner->SetPreviewScene(&InPreview->GetPreviewScene());
    }

    TSharedRef<FNGonPreviewScene> PreviewOwner;
    TSharedPtr<FAssetEditorModeManager> ModeToolsOwner;
};

class FNGonViewportClient final : private FNGonPreviewSceneOwner, public FEditorViewportClient
{
public:
    FNGonViewportClient(
        const TSharedRef<FNGonPreviewScene>& InPreview,
        const TSharedRef<SNGonViewport>& InWidget)
        : FNGonPreviewSceneOwner(InPreview)
        , FEditorViewportClient(ModeToolsOwner.Get(), &InPreview->GetPreviewScene(), InWidget)
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

        if (FEditorModeTools* ViewportModeTools = GetModeTools())
        {
            ViewportModeTools->ActivateDefaultMode();
            ViewportModeTools->SetWidgetMode(UE::Widget::WM_None);

            if (UModeManagerInteractiveToolsContext* ToolsContext = ViewportModeTools->GetInteractiveToolsContext();
                ToolsContext && ToolsContext->ToolManager && ToolsContext->GizmoManager && ToolsContext->InputRouter)
            {
                InteractiveToolsContext = ToolsContext;
                ToolManager = ToolsContext->ToolManager.Get();
                GizmoManager = ToolsContext->GizmoManager.Get();
                InputRouter = ToolsContext->InputRouter.Get();

                ToolsContext->SetViewportInteractionsEnabled(false);

                // Register UCombinedTransformGizmoContextObject and UGizmoViewContext so
                // ACombinedTransformGizmoActor's UViewAdjustedStaticMeshGizmoComponents
                // and view-based transform adjusters receive a valid UGizmoViewContext.
                UE::TransformGizmoUtil::RegisterTransformGizmoContextObject(ToolsContext);

                if (UContextObjectStore* ContextStore = ToolManager->GetContextObjectStore())
                {
                    GizmoViewContext = ContextStore->FindContext<UGizmoViewContext>();
                    if (!GizmoViewContext)
                    {
                        GizmoViewContext = NewObject<UGizmoViewContext>(ToolManager);
                        ContextStore->AddContextObject(GizmoViewContext);
                    }
                }

                ToolManager->ConfigureChangeTrackingMode(EToolChangeTrackingMode::NoChangeTracking);
                PreviewOwner->SetOverlayMaterials(ToolManager);
                PreviewOwner->SetupGizmo(GizmoManager, nullptr);
            }
        }
    }

    void SetGridShowFlag(bool bVisible)
    {
        EngineShowFlags.SetGrid(bVisible);
        Invalidate();
    }

    UE::Widget::EWidgetMode GetWidgetMode() const override
    {
        if (!NGon::HasActiveSceneGizmoTarget(&PreviewOwner.Get()))
        {
            return UE::Widget::WM_None;
        }
        switch (PreviewOwner->GetGizmoMode())
        {
        case ENGonGizmoMode::Translate:
            return UE::Widget::WM_Translate;
        case ENGonGizmoMode::Rotate:
            return UE::Widget::WM_Rotate;
        case ENGonGizmoMode::Scale:
            return UE::Widget::WM_Scale;
        default:
            return UE::Widget::WM_None;
        }
    }

    FVector GetWidgetLocation() const override
    {
        return PreviewOwner->GetSelectionGizmoTransform().GetLocation();
    }

    FMatrix GetWidgetCoordSystem() const override
    {
        const bool bWorld = NGon::IsSceneWorldCoordinateSystem(&PreviewOwner.Get())
            && PreviewOwner->GetGizmoMode() != ENGonGizmoMode::Scale;
        if (bWorld)
        {
            return FMatrix::Identity;
        }
        return FQuatRotationMatrix(PreviewOwner->GetSelectionGizmoTransform().GetRotation());
    }

    ECoordSystem GetWidgetCoordSystemSpace() const override
    {
        return (NGon::IsSceneWorldCoordinateSystem(&PreviewOwner.Get())
            && PreviewOwner->GetGizmoMode() != ENGonGizmoMode::Scale)
            ? COORD_World
            : COORD_Local;
    }

    void TrackingStarted(const struct FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge) override
    {
        FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);
        if (bIsDraggingWidget && !bDraggingNativeWidget)
        {
            bDraggingNativeWidget = true;
            NativeWidgetStartTransform = PreviewOwner->GetSelectionGizmoTransform();
            NativeWidgetRawLocation = NativeWidgetStartTransform.GetLocation();
            NativeWidgetRawRotation = NativeWidgetStartTransform.GetRotation();
            NativeWidgetRawScale = FVector::OneVector;
            PreviewOwner->BeginSoftTransform(NativeWidgetStartTransform, PreviewOwner->GetSoftTransformMode());
        }
    }

    bool InputWidgetDelta(
        FViewport* InViewport,
        EAxisList::Type InCurrentAxis,
        FVector& Drag,
        FRotator& Rot,
        FVector& Scale) override
    {
        if (!bDraggingNativeWidget || InCurrentAxis == EAxisList::None)
        {
            return false;
        }

        bool bChanged = false;
        if (!Drag.IsNearlyZero())
        {
            NativeWidgetRawLocation += Drag;
            bChanged = true;
        }
        if (!Rot.IsZero())
        {
            NativeWidgetRawRotation = (Rot.Quaternion() * NativeWidgetRawRotation).GetNormalized();
            bChanged = true;
        }
        if (!Scale.IsNearlyZero())
        {
            NativeWidgetRawScale = (NativeWidgetRawScale + Scale * 0.025).ComponentMax(FVector(0.05));
            bChanged = true;
        }

        if (bChanged)
        {
            const FTransform UpdatedTransform(NativeWidgetRawRotation, NativeWidgetRawLocation, NativeWidgetRawScale);
            PreviewOwner->ApplyGizmoTransform(UpdatedTransform);
            if (NGon::IsBoneTransformActive(&PreviewOwner.Get()))
            {
                if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
                {
                    if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get()))
                    {
                        if (UIState->OnViewportBoneTransformChanged)
                        {
                            UIState->OnViewportBoneTransformChanged();
                        }
                    }
                }
            }
            Invalidate();
        }
        return true;
    }

    void TrackingStopped() override
    {
        if (bDraggingNativeWidget)
        {
            bDraggingNativeWidget = false;
            if (PreviewOwner->IsSoftTransformActive() || NGon::IsBoneTransformActive(&PreviewOwner.Get()))
            {
                PreviewOwner->EndSoftTransform();
                NGon::UpdateActiveViewportUVMeshFromTopology(PreviewOwner->GetTopology());
            }
            if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
            {
                if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get()))
                {
                    if (UIState->OnViewportBoneTransformChanged)
                    {
                        UIState->OnViewportBoneTransformChanged();
                    }
                }
            }
            Invalidate();
        }
        FEditorViewportClient::TrackingStopped();
    }

    EMouseCursor::Type GetCursor(FViewport* InViewport, int32 X, int32 Y) override
    {
        if (InViewport && !IsAltPressed())
        {
            if (bConsumedByGizmo || (InputRouter && InputRouter->HasActiveMouseCapture()) || (PreviewOwner->IsSoftTransformActive() || NGon::IsBoneTransformActive(&PreviewOwner.Get())) || NGon::IsBoneTransformActive(&PreviewOwner.Get()))
            {
                return EMouseCursor::CardinalCross;
            }
            return EMouseCursor::Default;
        }
        return FEditorViewportClient::GetCursor(InViewport, X, Y);
    }

    void Tick(float DeltaSeconds) override
    {
        if (Viewport && bHasPendingGizmoDrag)
        {
            FlushPendingGizmoDrag(Viewport);
        }
        FEditorViewportClient::Tick(DeltaSeconds);
        if (ToolManager)
        {
            ToolManager->Tick(DeltaSeconds);
        }
        if (GizmoManager)
        {
            GizmoManager->Tick(DeltaSeconds);
        }
    }

    bool InputKey(const FInputKeyEventArgs& EventArgs) override
    {
        if (EventArgs.Event == IE_Pressed && EventArgs.Key == EKeys::Escape && (PreviewOwner->IsSoftTransformActive() || NGon::IsBoneTransformActive(&PreviewOwner.Get())))
        {
            if (InputRouter)
            {
                InputRouter->ForceTerminateAll();
            }
            PreviewOwner->CancelSoftTransform();
            bLeftMouseButtonDown = false;
            bHasPendingGizmoDrag = false;
            bConsumedByGizmo = false;
            bMarqueeActive = false;
            bMarqueePending = false;
            Invalidate();
            return true;
        }

        if (EventArgs.Key == EKeys::LeftMouseButton && EventArgs.Viewport)
        {
            if (FEditorModeTools* ViewportModeTools = GetModeTools())
            {
                ViewportModeTools->ReceivedFocus(this, EventArgs.Viewport);
            }

            const int32 MouseX = EventArgs.Viewport->GetMouseX();
            const int32 MouseY = EventArgs.Viewport->GetMouseY();
            const FVector2D CurrentPos(MouseX, MouseY);

            if (EventArgs.Event == IE_Pressed && !IsAltPressed() && !IsCtrlPressed())
            {
                // 1. First check if the user clicked on FEditorViewportClient's native Unreal FWidget (HWidgetAxis)
                if (GetWidgetMode() != UE::Widget::WM_None)
                {
                    HHitProxy* HitResult = EventArgs.Viewport->GetHitProxy(MouseX, MouseY);
                    if (HitResult && HitResult->IsA(HWidgetAxis::StaticGetType()))
                    {
                        HWidgetAxis* AxisProxy = static_cast<HWidgetAxis*>(HitResult);
                        if (Widget)
                        {
                            Widget->SetCurrentAxis(AxisProxy->Axis);
                        }
                        bDraggingNativeWidget = true;
                        NativeWidgetStartTransform = PreviewOwner->GetSelectionGizmoTransform();
                        NativeWidgetRawLocation = NativeWidgetStartTransform.GetLocation();
                        NativeWidgetRawRotation = NativeWidgetStartTransform.GetRotation();
                        NativeWidgetRawScale = FVector::OneVector;
                        PreviewOwner->BeginSoftTransform(NativeWidgetStartTransform, PreviewOwner->GetSoftTransformMode());
                        FEditorViewportClient::InputKey(EventArgs);
                        Invalidate();
                        return true;
                    }
                }

                LastMousePosition = CurrentPos;
                bLeftMouseButtonDown = true;
                bHasPendingGizmoDrag = false;
                bConsumedByGizmo = false;

                FSceneViewFamilyContext ViewFamily(
                    FSceneViewFamily::ConstructionValues(EventArgs.Viewport, GetScene(), EngineShowFlags)
                    .SetRealtimeUpdate(IsRealtime()));
                if (FSceneView* View = BuildSceneView(EventArgs.Viewport, ViewFamily))
                {
                    if (GizmoManager)
                    {
                        GizmoManager->Tick(0.0f);
                    }
                    if (InputRouter)
                    {
                        // Use the tools context's canonical button route. It updates
                        // the cached mouse state, performs gizmo hit testing, and
                        // starts capture as one atomic operation. Sending a second
                        // hand-built press here can leave the router and the mesh
                        // picker disagreeing about who owns the gesture.
                        const bool bCapturedOnPress = GetModeTools()
                            ? GetModeTools()->InputKey(this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event, true)
                            : InteractiveToolsContext
                                ? InteractiveToolsContext->InputKey(this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event)
                            : [&]()
                            {
                                FInputDeviceState State = MakeMouseInputState(*View, MouseX, MouseY, FVector2D::ZeroVector);
                                State.Mouse.Left.SetStates(true, true, false);
                                return InputRouter->PostInputEvent(State);
                            }();

                        if (bCapturedOnPress || InputRouter->HasActiveMouseCapture() || (PreviewOwner->IsSoftTransformActive() || NGon::IsBoneTransformActive(&PreviewOwner.Get())))
                        {
                            bConsumedByGizmo = true;
                            bMarqueeActive = false;
                            bMarqueePending = false;
                            Invalidate();
                            return true;
                        }
                    }
                }

                MarqueeStart = CurrentPos;
                MarqueeCurrent = CurrentPos;
                bMarqueeActive = false;
                bMarqueePending = true;
                return true;
            }
            else if (EventArgs.Event == IE_Released && bDraggingNativeWidget)
            {
                bDraggingNativeWidget = false;
                bLeftMouseButtonDown = false;
                if (Widget)
                {
                    Widget->SetCurrentAxis(EAxisList::None);
                }
                FEditorViewportClient::InputKey(EventArgs);
                if (PreviewOwner->IsSoftTransformActive() || NGon::IsBoneTransformActive(&PreviewOwner.Get()))
                {
                    PreviewOwner->EndSoftTransform();
                    NGon::UpdateActiveViewportUVMeshFromTopology(PreviewOwner->GetTopology());
                }
                if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
                {
                    if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get()))
                    {
                        if (UIState->OnViewportBoneTransformChanged)
                        {
                            UIState->OnViewportBoneTransformChanged();
                        }
                    }
                }
                Invalidate();
                return true;
            }
            else if (EventArgs.Event == IE_Released && !IsAltPressed() && !IsCtrlPressed())
            {
                if (bHasPendingGizmoDrag)
                {
                    FlushPendingGizmoDrag(EventArgs.Viewport);
                }
                const FVector2D Delta = CurrentPos - LastMousePosition;
                LastMousePosition = CurrentPos;
                const bool bWasGizmoInteraction = bConsumedByGizmo
                    || (InputRouter && InputRouter->HasActiveMouseCapture())
                    || (PreviewOwner->IsSoftTransformActive() || NGon::IsBoneTransformActive(&PreviewOwner.Get()));
                bLeftMouseButtonDown = false;
                bHasPendingGizmoDrag = false;

                FSceneViewFamilyContext ViewFamily(
                    FSceneViewFamily::ConstructionValues(EventArgs.Viewport, GetScene(), EngineShowFlags)
                    .SetRealtimeUpdate(IsRealtime()));
                FSceneView* View = BuildSceneView(EventArgs.Viewport, ViewFamily);

                // Always deliver the release to the tools context, including a
                // normal mesh click. The press above was routed through
                // InputKey() to perform gizmo hit testing; skipping the matching
                // release leaves CurrentMouseState.Mouse.Left.bDown set and
                // causes later hover/capture tests to reject gizmo interaction.
                if (InteractiveToolsContext)
                {
                    if (FEditorModeTools* ViewportModeTools = GetModeTools())
                    {
                        ViewportModeTools->InputKey(this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event, true);
                    }
                    else
                    {
                        InteractiveToolsContext->InputKey(this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event);
                    }
                }

                if (bWasGizmoInteraction)
                {
                    if (!InteractiveToolsContext && View && InputRouter)
                    {
                        FInputDeviceState State = MakeMouseInputState(*View, MouseX, MouseY, Delta);
                        State.Mouse.Left.SetStates(false, false, true);
                        InputRouter->PostInputEvent(State);
                    }
                    if (GizmoManager)
                    {
                        GizmoManager->Tick(0.0f);
                    }
                    if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
                    {
                        if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get()))
                        {
                            if (UIState->OnViewportBoneTransformChanged)
                            {
                                UIState->OnViewportBoneTransformChanged();
                            }
                        }
                    }
                    bConsumedByGizmo = false;
                    bMarqueeActive = false;
                    bMarqueePending = false;
                    Invalidate();
                    return true;
                }

                if (bMarqueeActive)
                {
                    if (View)
                    {
                        const FBox2D Rect(MarqueeStart, MarqueeCurrent);
                        const bool bFullyContained = MarqueeCurrent.X >= MarqueeStart.X;
                        if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
                        {
                            Pinned->BoxSelect(*View, Rect, IsShiftPressed(), bFullyContained);
                        }
                    }
                    bMarqueeActive = false;
                    bMarqueePending = false;
                    Invalidate();
                    return true;
                }

                if (bMarqueePending)
                {
                    bMarqueePending = false;
                    if (View)
                    {
                        if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
                        {
                            FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get());
                            const bool bBonesVisible = !UIState || UIState->BoneDrawMode != 3;
                            int32 HitBone = INDEX_NONE;
                            if (bBonesVisible && NGon::HasSceneBones(&PreviewOwner.Get()))
                            {
                                const float PickRadius = NGon::IsBoneGizmoActive(&PreviewOwner.Get()) ? 20.0f : 12.0f;
                                HitBone = NGon::PickSceneBone(&PreviewOwner.Get(), *View, CurrentPos, PickRadius, true);
                            }
                            if (HitBone != INDEX_NONE)
                            {
                                if (UIState && UIState->OnViewportBonePicked)
                                {
                                    UIState->OnViewportBonePicked(HitBone);
                                }
                            }
                            else
                            {
                                Pinned->Pick(*View, CurrentPos, IsShiftPressed());
                            }
                        }
                    }
                    Invalidate();
                    return true;
                }
                return true;
            }
            else if (EventArgs.Event == IE_Released)
            {
                if (bHasPendingGizmoDrag)
                {
                    FlushPendingGizmoDrag(EventArgs.Viewport);
                }
                const bool bWasGizmoInteraction = bConsumedByGizmo
                    || (InputRouter && InputRouter->HasActiveMouseCapture())
                    || (PreviewOwner->IsSoftTransformActive() || NGon::IsBoneTransformActive(&PreviewOwner.Get()));
                if (InteractiveToolsContext)
                {
                    // Keep the tools context's button state balanced for
                    // modifier-assisted clicks as well.
                    if (FEditorModeTools* ViewportModeTools = GetModeTools())
                    {
                        ViewportModeTools->InputKey(this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event, true);
                    }
                    else
                    {
                        InteractiveToolsContext->InputKey(this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event);
                    }
                }
                if (bWasGizmoInteraction && !InteractiveToolsContext)
                {
                    FSceneViewFamilyContext ViewFamily(
                        FSceneViewFamily::ConstructionValues(EventArgs.Viewport, GetScene(), EngineShowFlags)
                        .SetRealtimeUpdate(IsRealtime()));
                    if (FSceneView* View = BuildSceneView(EventArgs.Viewport, ViewFamily))
                    {
                        if (InputRouter)
                        {
                            FInputDeviceState State = MakeMouseInputState(*View, MouseX, MouseY, CurrentPos - LastMousePosition);
                            State.Mouse.Left.SetStates(false, false, true);
                            InputRouter->PostInputEvent(State);
                        }
                    }
                }
                bLeftMouseButtonDown = false;
                bHasPendingGizmoDrag = false;
                bConsumedByGizmo = false;
                bMarqueeActive = false;
                bMarqueePending = false;
            }
            else if (EventArgs.Event == IE_Pressed && IsCtrlPressed() && !IsAltPressed())
            {
                if (InteractiveToolsContext)
                {
                    InteractiveToolsContext->UpdateStateWithoutRoutingInputKey(
                        this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event);
                }
                FSceneViewFamilyContext ViewFamily(
                    FSceneViewFamily::ConstructionValues(EventArgs.Viewport, GetScene(), EngineShowFlags)
                    .SetRealtimeUpdate(IsRealtime()));
                if (FSceneView* View = BuildSceneView(EventArgs.Viewport, ViewFamily))
                {
                    if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
                    {
                        const int32 Id = Pinned->Pick(*View, CurrentPos, IsShiftPressed());
                        if (Pinned->GetSelectionMode() == ENGonElement::Edge && Id != INDEX_NONE)
                        {
                            Pinned->SelectEdgeLoop(Id, IsShiftPressed());
                            Invalidate();
                            return true;
                        }
                    }
                }
            }
        }

        // Ctrl+I (Reset Selected Bone Transform) & Ctrl+Shift+I (Reset All Bones Transforms) matching UE Skeletal Mesh Editor
        if (EventArgs.Event == IE_Pressed && IsCtrlPressed() && !IsAltPressed() && EventArgs.Key == EKeys::I)
        {
            const bool bReset = IsShiftPressed()
                ? NGon::ResetAllSceneBonesTransforms(&PreviewOwner.Get())
                : NGon::ResetSelectedSceneBoneTransform(&PreviewOwner.Get());
            if (bReset)
            {
                if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
                {
                    if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get()))
                    {
                        if (UIState->OnViewportBoneTransformChanged)
                        {
                            UIState->OnViewportBoneTransformChanged();
                        }
                    }
                }
                Invalidate();
            }
            return true;
        }

        // Alt+9: Toggle Mesh Edges Rendering (matching UE Skeletal Mesh Editor hotkey)
        if (EventArgs.Event == IE_Pressed && IsAltPressed() && !IsCtrlPressed() && EventArgs.Key == EKeys::Nine)
        {
            if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
            {
                if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get()))
                {
                    if (UIState->OnToggleEdgesShortcut)
                    {
                        UIState->OnToggleEdgesShortcut();
                    }
                }
            }
            Invalidate();
            return true;
        }

        if (EventArgs.Event == IE_Pressed && !IsAltPressed() && !IsCtrlPressed())
        {
            if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
            {
                if (EventArgs.Key == EKeys::Q)
                {
                    Pinned->SetGizmoMode(ENGonGizmoMode::None);
                    return true;
                }
                if (EventArgs.Key == EKeys::Four)
                {
                    NGon::SetBoneGizmoActive(&PreviewOwner.Get(), true);
                    Invalidate();
                    return true;
                }
                if (EventArgs.Key == EKeys::One)
                {
                    NGon::SetBoneGizmoActive(&PreviewOwner.Get(), false);
                    Pinned->SetSelectionMode(ENGonElement::Vertex);
                    return true;
                }
                if (EventArgs.Key == EKeys::Two)
                {
                    NGon::SetBoneGizmoActive(&PreviewOwner.Get(), false);
                    Pinned->SetSelectionMode(ENGonElement::Edge);
                    return true;
                }
                if (EventArgs.Key == EKeys::Three)
                {
                    NGon::SetBoneGizmoActive(&PreviewOwner.Get(), false);
                    Pinned->SetSelectionMode(ENGonElement::Face);
                    return true;
                }
                if (EventArgs.Key == EKeys::W)
                {
                    Pinned->SetGizmoMode(ENGonGizmoMode::Translate);
                    return true;
                }
                if (EventArgs.Key == EKeys::E)
                {
                    Pinned->SetGizmoMode(ENGonGizmoMode::Rotate);
                    return true;
                }
                if (EventArgs.Key == EKeys::R)
                {
                    Pinned->SetGizmoMode(ENGonGizmoMode::Scale);
                    return true;
                }
                if (EventArgs.Key == EKeys::H)
                {
                    Pinned->ToggleHeatmap();
                    return true;
                }
                if (EventArgs.Key == EKeys::F)
                {
                    Pinned->FrameMesh();
                    return true;
                }
                if (EventArgs.Key == EKeys::Delete || EventArgs.Key == EKeys::BackSpace)
                {
                    if (Pinned->DeleteSelectedFaces())
                    {
                        Invalidate();
                        return true;
                    }
                    return false;
                }
            }
        }
        return FEditorViewportClient::InputKey(EventArgs);
    }

    void MouseMove(FViewport* InViewport, int32 X, int32 Y) override
    {
        if (bDraggingNativeWidget)
        {
            FEditorViewportClient::MouseMove(InViewport, X, Y);
            return;
        }
        if (HandleLeftMouseDrag(InViewport, X, Y))
        {
            return;
        }
        // Always let FEditorViewportClient update HWidgetAxis hover state on the native Unreal FWidget
        if (GetWidgetMode() != UE::Widget::WM_None)
        {
            FEditorViewportClient::MouseMove(InViewport, X, Y);
        }

        if (InputRouter && InViewport && !IsAltPressed())
        {
            FSceneViewFamilyContext ViewFamily(
                FSceneViewFamily::ConstructionValues(InViewport, GetScene(), EngineShowFlags)
                .SetRealtimeUpdate(IsRealtime()));
            if (FSceneView* View = BuildSceneView(InViewport, ViewFamily))
            {
                if (GizmoManager)
                {
                    GizmoManager->Tick(0.0f);
                }
                if (InteractiveToolsContext)
                {
                    InteractiveToolsContext->MouseMove(this, InViewport, X, Y);
                }
                const FVector2D CurrentPos(X, Y);
                const FVector2D HoverDelta = CurrentPos - LastMousePosition;
                LastMousePosition = CurrentPos;
                if (!InteractiveToolsContext)
                {
                    FInputDeviceState State = MakeMouseInputState(*View, X, Y, HoverDelta);
                    InputRouter->PostHoverInputEvent(State);
                }
                Invalidate();
            }
            return;
        }

        FEditorViewportClient::MouseMove(InViewport, X, Y);
    }

    void CapturedMouseMove(FViewport* InViewport, int32 InMouseX, int32 InMouseY) override
    {
        if (bDraggingNativeWidget)
        {
            FEditorViewportClient::CapturedMouseMove(InViewport, InMouseX, InMouseY);
            return;
        }
        if (HandleLeftMouseDrag(InViewport, InMouseX, InMouseY))
        {
            return;
        }

        FEditorViewportClient::CapturedMouseMove(InViewport, InMouseX, InMouseY);
    }

    bool InputAxis(const FInputKeyEventArgs& Args) override
    {
        if (bDraggingNativeWidget)
        {
            return FEditorViewportClient::InputAxis(Args);
        }
        // A left-button press by itself is also how mesh picking and marquee
        // selection begin. Do not swallow viewport axis events until a gizmo,
        // soft-transform, or marquee capture actually owns the gesture.
        if (!IsAltPressed() && (bConsumedByGizmo || (PreviewOwner->IsSoftTransformActive() || NGon::IsBoneTransformActive(&PreviewOwner.Get())) || bMarqueeActive))
        {
            return true;
        }
        return FEditorViewportClient::InputAxis(Args);
    }

    void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override
    {
        if (View && GizmoViewContext)
        {
            GizmoViewContext->ResetFromSceneView(*View);
        }
        FEditorViewportClient::Draw(View, PDI);

        if (View && PDI && NGon::HasSceneBones(&PreviewOwner.Get()))
        {
            int32 DrawMode = 0;
            if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
            {
                if (const FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get()))
                {
                    DrawMode = UIState->BoneDrawMode;
                }
            }
            NGon::DrawSceneSkeleton(&PreviewOwner.Get(), View, PDI, DrawMode);
        }
    }

    void Draw(FViewport* InViewport, FCanvas* Canvas) override
    {
        FEditorViewportClient::Draw(InViewport, Canvas);

        if (InViewport && Canvas && NGon::HasSceneBones(&PreviewOwner.Get()))
        {
            bool bShowBoneNames = true;
            int32 DrawMode = 0;
            if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
            {
                if (const FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get()))
                {
                    bShowBoneNames = UIState->bShowBoneNames;
                    DrawMode = UIState->BoneDrawMode;
                }
            }
            if (bShowBoneNames && DrawMode != 3)
            {
                FSceneViewFamilyContext ViewFamily(
                    FSceneViewFamily::ConstructionValues(InViewport, GetScene(), EngineShowFlags)
                    .SetRealtimeUpdate(IsRealtime()));
                if (FSceneView* View = BuildSceneView(InViewport, ViewFamily))
                {
                    NGon::DrawSceneSkeletonLabels(&PreviewOwner.Get(), View, InViewport, Canvas, true);
                }
            }
        }

        if (bMarqueeActive && Canvas)
        {
            const FVector2D Min(FMath::Min(MarqueeStart.X, MarqueeCurrent.X), FMath::Min(MarqueeStart.Y, MarqueeCurrent.Y));
            const FVector2D Size(FMath::Abs(MarqueeCurrent.X - MarqueeStart.X), FMath::Abs(MarqueeCurrent.Y - MarqueeStart.Y));
            FCanvasBoxItem Box(Min, Size);
            Box.SetColor(FLinearColor(0.15f, 0.65f, 1.0f, 0.85f));
            Box.LineThickness = 2.0f;
            Canvas->DrawItem(Box);
        }
    }

    ~FNGonViewportClient() override
    {
        PreviewOwner->TeardownGizmo();
        NGon::NotifyHostViewportClosed();
        if (InteractiveToolsContext)
        {
            UE::TransformGizmoUtil::DeregisterTransformGizmoContextObject(InteractiveToolsContext);
        }
    }

private:
    FSceneView* BuildSceneView(FViewport* InViewport, FSceneViewFamilyContext& ViewFamily)
    {
        if (!InViewport || InViewport->GetSizeXY().GetMin() <= 0)
        {
            return nullptr;
        }
        FSceneView* View = CalcSceneView(&ViewFamily);
        if (View && GizmoViewContext)
        {
            GizmoViewContext->ResetFromSceneView(*View);
        }
        return View;
    }

    FInputDeviceState MakeMouseInputState(const FSceneView& View, int32 MouseX, int32 MouseY, const FVector2D& Delta2D) const
    {
        FVector RayOrigin = FVector::ZeroVector;
        FVector RayDirection = FVector::ForwardVector;
        View.DeprojectFVector2D(FVector2D(MouseX, MouseY), RayOrigin, RayDirection);

        FInputDeviceState State;
        State.InputDevice = EInputDevices::Mouse;
        State.SetModifierKeyStates(IsShiftPressed(), IsAltPressed(), IsCtrlPressed(), false);
        State.Mouse.Position2D = FVector2D(MouseX, MouseY);
        State.Mouse.Delta2D = Delta2D;
        State.Mouse.WorldRay = FRay(RayOrigin, RayDirection);
        return State;
    }

    void FlushPendingGizmoDrag(FViewport* InViewport)
    {
        if (!bHasPendingGizmoDrag || !InViewport)
        {
            return;
        }
        bHasPendingGizmoDrag = false;
        const int32 InMouseX = FMath::RoundToInt(PendingDragPosition.X);
        const int32 InMouseY = FMath::RoundToInt(PendingDragPosition.Y);
        const FVector2D Delta = PendingDragPosition - LastMousePosition;
        if (Delta.IsNearlyZero())
        {
            return;
        }
        LastMousePosition = PendingDragPosition;

        FSceneViewFamilyContext ViewFamily(
            FSceneViewFamily::ConstructionValues(InViewport, GetScene(), EngineShowFlags)
            .SetRealtimeUpdate(IsRealtime()));
        if (FSceneView* View = BuildSceneView(InViewport, ViewFamily))
        {
            if (FEditorModeTools* ViewportModeTools = GetModeTools())
            {
                ViewportModeTools->CapturedMouseMove(this, InViewport, InMouseX, InMouseY);
            }
            else if (InteractiveToolsContext)
            {
                // CapturedMouseMove maintains the tools context's unbounded
                // cursor/ray state and posts exactly one captured event.
                InteractiveToolsContext->CapturedMouseMove(this, InViewport, InMouseX, InMouseY);
            }
            else if (InputRouter)
            {
                FInputDeviceState State = MakeMouseInputState(*View, InMouseX, InMouseY, Delta);
                State.Mouse.Left.SetStates(false, true, false);
                InputRouter->PostInputEvent(State);
                if (NGon::IsBoneTransformActive(&PreviewOwner.Get()))
                {
                    if (const TSharedPtr<SNGonViewport> Pinned = ViewportWidget.Pin())
                    {
                        if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Pinned.Get()))
                        {
                            if (UIState->OnViewportBoneTransformChanged)
                            {
                                UIState->OnViewportBoneTransformChanged();
                            }
                        }
                    }
                }
            }
            if (GizmoManager)
            {
                GizmoManager->Tick(0.0f);
            }
        }
        Invalidate();
    }

    bool HandleLeftMouseDrag(FViewport* InViewport, int32 InMouseX, int32 InMouseY)
    {
        if (!bLeftMouseButtonDown || !InViewport)
        {
            return false;
        }

        const FVector2D CurrentPos(InMouseX, InMouseY);
        if (bConsumedByGizmo || (InputRouter && InputRouter->HasActiveMouseCapture()) || (PreviewOwner->IsSoftTransformActive() || NGon::IsBoneTransformActive(&PreviewOwner.Get())))
        {
            PendingDragPosition = CurrentPos;
            bHasPendingGizmoDrag = true;
            if (LastDragFrameCounter != GFrameCounter)
            {
                LastDragFrameCounter = GFrameCounter;
                FlushPendingGizmoDrag(InViewport);
            }
            else
            {
                Invalidate();
            }
            return true;
        }

        LastMousePosition = CurrentPos;
        if (bMarqueePending && !IsAltPressed())
        {
            if (bMarqueeActive || (MarqueeStart != CurrentPos &&
                (FMath::Abs(CurrentPos.X - MarqueeStart.X) + FMath::Abs(CurrentPos.Y - MarqueeStart.Y) > 5.0)))
            {
                MarqueeCurrent = CurrentPos;
                bMarqueeActive = true;
                Invalidate();
            }
            return true;
        }

        return false;
    }

    TWeakPtr<SNGonViewport> ViewportWidget;
    UModeManagerInteractiveToolsContext* InteractiveToolsContext = nullptr;
    UInteractiveToolManager* ToolManager = nullptr;
    UInteractiveGizmoManager* GizmoManager = nullptr;
    UInputRouter* InputRouter = nullptr;
    UGizmoViewContext* GizmoViewContext = nullptr;

    FVector2D LastMousePosition = FVector2D::ZeroVector;
    FVector2D PendingDragPosition = FVector2D::ZeroVector;
    uint64 LastDragFrameCounter = 0;
    FVector2D MarqueeStart = FVector2D::ZeroVector;
    FVector2D MarqueeCurrent = FVector2D::ZeroVector;
    bool bLeftMouseButtonDown = false;
    bool bHasPendingGizmoDrag = false;
    bool bMarqueeActive = false;
    bool bMarqueePending = false;
    bool bConsumedByGizmo = false;
    bool bDraggingNativeWidget = false;
    FTransform NativeWidgetStartTransform = FTransform::Identity;
    FVector NativeWidgetRawLocation = FVector::ZeroVector;
    FQuat NativeWidgetRawRotation = FQuat::Identity;
    FVector NativeWidgetRawScale = FVector::OneVector;
};
}

void SNGonViewport::Construct(const FArguments& InArgs)
{
    PreviewScene = MakeShared<FNGonPreviewScene>();
    PreviewScene->SetGizmoMode(ENGonGizmoMode::Translate);
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
    PreviewScene->SetMesh(MoveTemp(Data));
    FrameMesh();
}

void SNGonViewport::FrameMesh()
{
    const FBox Bounds = PreviewScene->GetSelectionOrRestBounds();
    if (Bounds.IsValid)
    {
        const FVector Center = Bounds.GetCenter();
        const FVector Size = Bounds.GetSize();
        FVector FromTarget(-0.18, -0.4, 1.0);
        if (Size.Y < FMath::Min(Size.X, Size.Z) * 0.25)
        {
            FromTarget = FVector(0.25, -1.0, 0.30);
        }
        else if (Size.X < FMath::Min(Size.Y, Size.Z) * 0.25)
        {
            FromTarget = FVector(-1.0, -0.25, 0.30);
        }

        const double Distance = FMath::Max(Size.Size() * 1.35, 35.0);
        const FVector Location = Center + FromTarget.GetSafeNormal() * Distance;
        const TSharedPtr<FEditorViewportClient> ViewportClient = GetViewportClient();
        ViewportClient->SetViewLocation(Location);
        ViewportClient->SetViewRotation((Center - Location).Rotation());
        ViewportClient->SetLookAtLocation(Center, false);
        ViewportClient->Invalidate();
    }
}

void SNGonViewport::OnFocusViewportToSelection()
{
    FrameMesh();
}

void SNGonViewport::SetSelectionMode(ENGonElement Element)
{
    if (SelectionMode != Element)
    {
        SelectionMode = Element;
        PreviewScene->ClearSelection();
        SetHeatmapVisible(false);
        Invalidate();
    }
}

void SNGonViewport::SetGizmoMode(ENGonGizmoMode Mode)
{
    PreviewScene->SetGizmoMode(Mode);
    Invalidate();
}

ENGonGizmoMode SNGonViewport::GetGizmoMode() const
{
    return PreviewScene.IsValid() ? PreviewScene->GetGizmoMode() : ENGonGizmoMode::Combined;
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
    const bool bDeleted = PreviewScene->DeleteSelectedFaces();
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



#undef LOCTEXT_NAMESPACE

#include "SNGonPreview.h"

#include "AssetEditorModeManager.h"
#include "AssetRegistry/AssetData.h"
#include "BaseGizmos/GizmoViewContext.h"
#include "BaseGizmos/TransformGizmoUtil.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "ContextObjectStore.h"
#include "DesktopPlatformModule.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "IDesktopPlatform.h"
#include "InputCoreTypes.h"
#include "InputRouter.h"
#include "InputState.h"
#include "InteractiveGizmoManager.h"
#include "InteractiveToolManager.h"
#include "Misc/Paths.h"
#include "NGonFbx.h"
#include "NGonMeshFiles.h"
#include "NGonPreviewScene.h"
#include "PropertyCustomizationHelpers.h"
#include "SceneView.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "NGonPreview"

namespace
{
// Base classes are destroyed in reverse order. Keep both the preview scene and
// the viewport-local ModeTools alive through the FEditorViewportClient destructor.
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
        , Widget(InWidget)
    {
        SetViewportType(LVT_Perspective);
        SetViewMode(VMI_Unlit);
        SetRealtime(true);
        SetAllowCinematicControl(false);
        EngineShowFlags.SetGrid(false);
        EngineShowFlags.SetSelectionOutline(false);
        EngineShowFlags.SetEyeAdaptation(false);
        EngineShowFlags.SetPostProcessing(false);
        EngineShowFlags.SetModeWidgets(true);

        if (FEditorModeTools* ViewportModeTools = GetModeTools())
        {
            ViewportModeTools->ActivateDefaultMode();

            if (UModeManagerInteractiveToolsContext* ToolsContext = ViewportModeTools->GetInteractiveToolsContext();
                ToolsContext && ToolsContext->ToolManager && ToolsContext->GizmoManager && ToolsContext->InputRouter)
            {
                ToolManager = ToolsContext->ToolManager.Get();
                GizmoManager = ToolsContext->GizmoManager.Get();
                InputRouter = ToolsContext->InputRouter.Get();

                if (UContextObjectStore* ContextStore = ToolManager->GetContextObjectStore())
                {
                    GizmoViewContext = ContextStore->FindContext<UGizmoViewContext>();
                    if (!GizmoViewContext)
                    {
                        GizmoViewContext = NewObject<UGizmoViewContext>(ToolManager);
                        ContextStore->AddContextObject(GizmoViewContext);
                    }
                }

                PreviewOwner->SetOverlayMaterials(ToolManager);
                PreviewOwner->SetupGizmo(GizmoManager, ToolManager);
            }
        }
    }

    bool InputKey(const FInputKeyEventArgs& EventArgs) override
    {
        if (EventArgs.Event == IE_Pressed && EventArgs.Key == EKeys::Escape && PreviewOwner->IsSoftTransformActive())
        {
            if (InputRouter)
            {
                InputRouter->ForceTerminateAll();
            }
            PreviewOwner->CancelSoftTransform();
            bLeftMouseButtonDown = false;
            bConsumedByGizmo = false;
            bMarqueeActive = false;
            bMarqueePending = false;
            Invalidate();
            return true;
        }

        if (EventArgs.Key == EKeys::LeftMouseButton && EventArgs.Viewport)
        {
            const int32 MouseX = EventArgs.Viewport->GetMouseX();
            const int32 MouseY = EventArgs.Viewport->GetMouseY();
            const FVector2D CurrentPos(MouseX, MouseY);

            if (EventArgs.Event == IE_Pressed && !IsAltPressed() && !IsCtrlPressed())
            {
                LastMousePosition = CurrentPos;
                bLeftMouseButtonDown = true;
                bConsumedByGizmo = false;

                FSceneViewFamilyContext ViewFamily(
                    FSceneViewFamily::ConstructionValues(EventArgs.Viewport, GetScene(), EngineShowFlags)
                    .SetRealtimeUpdate(IsRealtime()));
                if (FSceneView* View = BuildSceneView(EventArgs.Viewport, ViewFamily))
                {
                    if (InputRouter)
                    {
                        FInputDeviceState State = MakeMouseInputState(*View, MouseX, MouseY, FVector2D::ZeroVector);
                        State.Mouse.Left.SetStates(true, true, false);
                        InputRouter->PostInputEvent(State);

                        if (InputRouter->HasActiveMouseCapture() || PreviewOwner->IsSoftTransformActive())
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
            else if (EventArgs.Event == IE_Released && !IsAltPressed() && !IsCtrlPressed())
            {
                const FVector2D Delta = CurrentPos - LastMousePosition;
                LastMousePosition = CurrentPos;
                bLeftMouseButtonDown = false;

                FSceneViewFamilyContext ViewFamily(
                    FSceneViewFamily::ConstructionValues(EventArgs.Viewport, GetScene(), EngineShowFlags)
                    .SetRealtimeUpdate(IsRealtime()));
                FSceneView* View = BuildSceneView(EventArgs.Viewport, ViewFamily);

                if (bConsumedByGizmo || (InputRouter && InputRouter->HasActiveMouseCapture()) || PreviewOwner->IsSoftTransformActive())
                {
                    if (View && InputRouter)
                    {
                        FInputDeviceState State = MakeMouseInputState(*View, MouseX, MouseY, Delta);
                        State.Mouse.Left.SetStates(false, false, true);
                        InputRouter->PostInputEvent(State);
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
                        if (const TSharedPtr<SNGonViewport> Pinned = Widget.Pin())
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
                        if (const TSharedPtr<SNGonViewport> Pinned = Widget.Pin())
                        {
                            Pinned->Pick(*View, CurrentPos, IsShiftPressed());
                        }
                    }
                    Invalidate();
                    return true;
                }
                return true;
            }
            else if (EventArgs.Event == IE_Released)
            {
                bLeftMouseButtonDown = false;
                bConsumedByGizmo = false;
                bMarqueeActive = false;
                bMarqueePending = false;
            }
            else if (EventArgs.Event == IE_Pressed && IsCtrlPressed() && !IsAltPressed())
            {
                FSceneViewFamilyContext ViewFamily(
                    FSceneViewFamily::ConstructionValues(EventArgs.Viewport, GetScene(), EngineShowFlags)
                    .SetRealtimeUpdate(IsRealtime()));
                if (FSceneView* View = BuildSceneView(EventArgs.Viewport, ViewFamily))
                {
                    if (const TSharedPtr<SNGonViewport> Pinned = Widget.Pin())
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

        if (EventArgs.Event == IE_Pressed && !IsAltPressed() && !IsCtrlPressed())
        {
            if (const TSharedPtr<SNGonViewport> Pinned = Widget.Pin())
            {
                if (EventArgs.Key == EKeys::One)
                {
                    Pinned->SetSelectionMode(ENGonElement::Vertex);
                    return true;
                }
                if (EventArgs.Key == EKeys::Two)
                {
                    Pinned->SetSelectionMode(ENGonElement::Edge);
                    return true;
                }
                if (EventArgs.Key == EKeys::Three)
                {
                    Pinned->SetSelectionMode(ENGonElement::Face);
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
        if (HandleLeftMouseDrag(InViewport, X, Y))
        {
            return;
        }

        if (InputRouter && InViewport && !IsAltPressed())
        {
            FSceneViewFamilyContext ViewFamily(
                FSceneViewFamily::ConstructionValues(InViewport, GetScene(), EngineShowFlags)
                .SetRealtimeUpdate(IsRealtime()));
            if (FSceneView* View = BuildSceneView(InViewport, ViewFamily))
            {
                const FVector2D CurrentPos(X, Y);
                FInputDeviceState State = MakeMouseInputState(*View, X, Y, CurrentPos - LastMousePosition);
                LastMousePosition = CurrentPos;
                InputRouter->PostHoverInputEvent(State);
                Invalidate();
            }
            return;
        }

        FEditorViewportClient::MouseMove(InViewport, X, Y);
    }

    void CapturedMouseMove(FViewport* InViewport, int32 InMouseX, int32 InMouseY) override
    {
        if (HandleLeftMouseDrag(InViewport, InMouseX, InMouseY))
        {
            return;
        }

        FEditorViewportClient::CapturedMouseMove(InViewport, InMouseX, InMouseY);
    }

    bool InputAxis(const FInputKeyEventArgs& Args) override
    {
        if (!IsAltPressed() && (bLeftMouseButtonDown || bConsumedByGizmo || PreviewOwner->IsSoftTransformActive() || bMarqueePending || bMarqueeActive))
        {
            return true;
        }
        return FEditorViewportClient::InputAxis(Args);
    }

    void Draw(FViewport* InViewport, FCanvas* Canvas) override
    {
        if (InViewport && GizmoViewContext)
        {
            FSceneViewFamilyContext ViewFamily(
                FSceneViewFamily::ConstructionValues(InViewport, GetScene(), EngineShowFlags)
                .SetRealtimeUpdate(IsRealtime()));
            BuildSceneView(InViewport, ViewFamily);
        }

        FEditorViewportClient::Draw(InViewport, Canvas);

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

    bool HandleLeftMouseDrag(FViewport* InViewport, int32 InMouseX, int32 InMouseY)
    {
        if (!bLeftMouseButtonDown || !InViewport)
        {
            return false;
        }

        const FVector2D CurrentPos(InMouseX, InMouseY);
        const FVector2D Delta = CurrentPos - LastMousePosition;
        LastMousePosition = CurrentPos;

        if (bConsumedByGizmo || (InputRouter && InputRouter->HasActiveMouseCapture()) || PreviewOwner->IsSoftTransformActive())
        {
            FSceneViewFamilyContext ViewFamily(
                FSceneViewFamily::ConstructionValues(InViewport, GetScene(), EngineShowFlags)
                .SetRealtimeUpdate(IsRealtime()));
            if (FSceneView* View = BuildSceneView(InViewport, ViewFamily))
            {
                if (InputRouter)
                {
                    FInputDeviceState State = MakeMouseInputState(*View, InMouseX, InMouseY, Delta);
                    State.Mouse.Left.SetStates(false, true, false);
                    InputRouter->PostInputEvent(State);
                }
            }
            Invalidate();
            return true;
        }

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

    TWeakPtr<SNGonViewport> Widget;
    UInteractiveToolManager* ToolManager = nullptr;
    UInteractiveGizmoManager* GizmoManager = nullptr;
    UInputRouter* InputRouter = nullptr;
    UGizmoViewContext* GizmoViewContext = nullptr;

    FVector2D LastMousePosition = FVector2D::ZeroVector;
    FVector2D MarqueeStart = FVector2D::ZeroVector;
    FVector2D MarqueeCurrent = FVector2D::ZeroVector;
    bool bLeftMouseButtonDown = false;
    bool bMarqueeActive = false;
    bool bMarqueePending = false;
    bool bConsumedByGizmo = false;
};
}

void SNGonViewport::Construct(const FArguments& InArgs)
{
    PreviewScene = MakeShared<FNGonPreviewScene>();
    SEditorViewport::Construct(SEditorViewport::FArguments());
}

TSharedRef<FEditorViewportClient> SNGonViewport::MakeEditorViewportClient()
{
    return MakeShared<FNGonViewportClient>(PreviewScene.ToSharedRef(), SharedThis(this));
}

void SNGonViewport::SetMesh(FNGonMeshData&& Data)
{
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
        Invalidate();
    }
}

void SNGonViewport::ToggleHeatmap()
{
    bHeatmapVisible = !bHeatmapVisible;
    PreviewScene->SetHeatmapVisible(bHeatmapVisible);
    Invalidate();
}

void SNGonViewport::SetHeatmapVisible(bool bVisible)
{
    bHeatmapVisible = bVisible;
    PreviewScene->SetHeatmapVisible(bVisible);
    Invalidate();
}

int32 SNGonViewport::Pick(const FSceneView& View, const FVector2D& Cursor, bool bAdd)
{
    const int32 Id = PreviewScene->Pick(SelectionMode, View, Cursor, bAdd);
    Invalidate();
    return Id;
}

void SNGonViewport::BoxSelect(const FSceneView& View, const FBox2D& ScreenRect, bool bAdd, bool bFullyContained)
{
    PreviewScene->BoxSelect(SelectionMode, View, ScreenRect, bAdd, bFullyContained);
    Invalidate();
}

bool SNGonViewport::SelectEdgeLoop(int32 StartEdgeId, bool bAdd)
{
    const bool bSelected = PreviewScene->SelectEdgeLoop(StartEdgeId, bAdd);
    Invalidate();
    return bSelected;
}

bool SNGonViewport::DeleteSelectedFaces()
{
    const bool bDeleted = PreviewScene->DeleteSelectedFaces();
    if (bDeleted)
    {
        Invalidate();
    }
    return bDeleted;
}

void SNGonPreview::Construct(const FArguments& InArgs)
{
    SAssignNew(Viewport, SNGonViewport);

    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(8, 5))
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("Default", "Reload Default"))
                .OnClicked(this, &SNGonPreview::ReloadDefault)
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("OpenFbx", "Open FBX"))
                .OnClicked(this, &SNGonPreview::OpenFbx)
            ]
            + SWrapBox::Slot()
            [
                SNew(STextBlock).Text(LOCTEXT("StaticMesh", "Static Mesh"))
            ]
            + SWrapBox::Slot()
            [
                SNew(SBox).WidthOverride(290)
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UStaticMesh::StaticClass())
                    .ObjectPath_Lambda([this] { return AssetPath; })
                    .OnObjectChanged(this, &SNGonPreview::LoadStaticMesh)
                    .DisplayThumbnail(false)
                    .DisplayUseSelected(true)
                    .AllowCreate(false)
                ]
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("Demo", "Load Demo"))
                .OnClicked(this, &SNGonPreview::LoadDemo)
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("Frame", "Frame Mesh"))
                .OnClicked_Lambda([this]
                {
                    Viewport->FrameMesh();
                    return FReply::Handled();
                })
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(8, 3)
        [
            MakeControls()
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(8, 2)
        [
            MakeSoftSelectionControls()
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(8, 2, 8, 7)
        [
            MakeSliders()
        ]
        + SVerticalBox::Slot().FillHeight(1.0f)
        [
            Viewport.ToSharedRef()
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            MakeStatus()
        ]
    ];

    FText Error;
    if (!TryLoadDefault(Error))
    {
        LoadDemo();
        ShowError(Error);
    }
}

TSharedRef<SWidget> SNGonPreview::MakeSelectionMode(ENGonElement Element, const FText& Label)
{
    return SNew(SCheckBox)
        .IsChecked_Lambda([this, Element]
        {
            return Viewport->GetSelectionMode() == Element ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
        })
        .OnCheckStateChanged_Lambda([this, Element](ECheckBoxState)
        {
            SetSelectionMode(Element);
        })
        [
            SNew(STextBlock).Text(Label)
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeControls()
{
    return SNew(SWrapBox)
        .UseAllottedSize(true)
        .InnerSlotPadding(FVector2D(12, 5))
        + SWrapBox::Slot()
        [
            SNew(STextBlock).Text(LOCTEXT("Pick", "Pick"))
        ]
        + SWrapBox::Slot()
        [
            MakeSelectionMode(ENGonElement::Face, LOCTEXT("FaceMode", "Face"))
        ]
        + SWrapBox::Slot()
        [
            MakeSelectionMode(ENGonElement::Edge, LOCTEXT("EdgeMode", "Edge"))
        ]
        + SWrapBox::Slot()
        [
            MakeSelectionMode(ENGonElement::Vertex, LOCTEXT("VertexMode", "Vertex"))
        ]
        + SWrapBox::Slot()
        [
            SNew(SButton)
            .Text(LOCTEXT("DeleteFaces", "Delete Faces"))
            .OnClicked_Lambda([this]
            {
                Viewport->DeleteSelectedFaces();
                return FReply::Handled();
            })
        ]
        + SWrapBox::Slot()
        [
            SNew(SCheckBox).IsChecked(ECheckBoxState::Checked)
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                Viewport->Preview().SetShowEdges(State == ECheckBoxState::Checked);
                Viewport->Invalidate();
            })
            [
                SNew(STextBlock).Text(LOCTEXT("Edges", "Show Edges"))
            ]
        ]
        + SWrapBox::Slot()
        [
            SNew(SCheckBox).IsChecked(ECheckBoxState::Checked)
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                Viewport->Preview().SetShowVertices(State == ECheckBoxState::Checked);
                Viewport->Invalidate();
            })
            [
                SNew(STextBlock).Text(LOCTEXT("Vertices", "Show Vertices"))
            ]
        ]
        + SWrapBox::Slot()
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return Viewport->IsHeatmapVisible() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bShowHeatmap = State == ECheckBoxState::Checked;
                Viewport->SetHeatmapVisible(bShowHeatmap);
            })
            [
                SNew(STextBlock).Text(LOCTEXT("Heatmap", "Heatmap"))
            ]
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeSliders()
{
    return SNew(SWrapBox)
        .UseAllottedSize(true)
        .InnerSlotPadding(FVector2D(10, 4))
        + SWrapBox::Slot()
        [
            SNew(STextBlock).Text(LOCTEXT("HeatBias", "Heat Bias"))
        ]
        + SWrapBox::Slot()
        [
            SNew(SBox).WidthOverride(135)
            [
                SNew(SSlider)
                .MinValue(-0.5f).MaxValue(0.5f)
                .IsEnabled_Lambda([this] { return Viewport->IsHeatmapVisible(); })
                .Value_Lambda([this] { return HeatBias; })
                .OnValueChanged_Lambda([this](float Value)
                {
                    HeatBias = Value;
                    Viewport->Preview().SetHeatBias(Value);
                    Viewport->Invalidate();
                })
            ]
        ]
        + SWrapBox::Slot()
        [
            SNew(STextBlock).Text(LOCTEXT("Bend", "Bend"))
        ]
        + SWrapBox::Slot()
        [
            SNew(SBox).WidthOverride(135)
            [
                SNew(SSlider)
                .Value_Lambda([this] { return Bend; })
                .OnValueChanged_Lambda([this](float Value)
                {
                    Bend = Value;
                    Viewport->Preview().SetBend(Value);
                    Viewport->Invalidate();
                })
            ]
        ]
        + SWrapBox::Slot()
        [
            SNew(STextBlock).Text(LOCTEXT("Controls", "1 vertex     2 edge     3 face     Del delete face     H heatmap     F frame     Shift-click/add marquee     Ctrl-click edge loop     Alt + drag orbit"))
        ];
}

void SNGonPreview::ApplySoftSelectionSettings()
{
    FNGonSoftSelectionSettings Settings;
    Settings.bEnabled = bSoftSelection;
    Settings.Radius = FMath::Max(0.0f, SoftRadius);
    Settings.Metric = SoftMetric;
    Settings.Connectivity = SoftConnectivity;
    Settings.SurfaceBlend = FMath::Clamp(static_cast<double>(SoftSurfaceBlend), 0.0, 1.0);
    Viewport->Preview().SetSoftTransformMode(SoftMode);
    Viewport->Preview().SetSoftSelection(Settings);
    Viewport->Invalidate();
}

TSharedRef<SWidget> SNGonPreview::MakeSoftSelectionControls()
{
    return SNew(SWrapBox)
        .UseAllottedSize(true)
        .InnerSlotPadding(FVector2D(10, 4))
        + SWrapBox::Slot()
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return bSoftSelection ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bSoftSelection = State == ECheckBoxState::Checked;
                ApplySoftSelectionSettings();
            })
            [ SNew(STextBlock).Text(LOCTEXT("SoftSelection", "Soft Selection")) ]
        ]
        + SWrapBox::Slot()
        [ SNew(STextBlock).Text(LOCTEXT("SoftRadius", "Radius")) ]
        + SWrapBox::Slot()
        [
            SNew(SBox).WidthOverride(145)
            [
                SNew(SSlider)
                .MinValue(1.0f).MaxValue(500.0f)
                .Value_Lambda([this] { return SoftRadius; })
                .OnValueChanged_Lambda([this](float Value)
                {
                    SoftRadius = Value;
                    ApplySoftSelectionSettings();
                })
            ]
        ]
        + SWrapBox::Slot()
        [ SNew(STextBlock).Text(LOCTEXT("SoftMetric", "Metric")) ]
        + SWrapBox::Slot()
        [
            SNew(SComboButton)
            .OnGetMenuContent_Lambda([this]
            {
                FMenuBuilder Menu(true, nullptr);
                const auto AddMetric = [this, &Menu](ENGonSoftMetric Metric, const TCHAR* Label)
                {
                    Menu.AddMenuEntry(FText::FromString(Label), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([this, Metric] { SoftMetric = Metric; ApplySoftSelectionSettings(); })));
                };
                AddMetric(ENGonSoftMetric::EdgeSurface, TEXT("Edge Surface"));
                AddMetric(ENGonSoftMetric::FaceSurface, TEXT("Face Surface"));
                AddMetric(ENGonSoftMetric::Volume, TEXT("Volume"));
                AddMetric(ENGonSoftMetric::Hybrid, TEXT("Hybrid"));
                return Menu.MakeWidget();
            })
            .ButtonContent()
            [ SNew(STextBlock).Text_Lambda([this]
                {
                    switch (SoftMetric)
                    {
                    case ENGonSoftMetric::FaceSurface: return LOCTEXT("MetricFace", "Face Surface");
                    case ENGonSoftMetric::Volume: return LOCTEXT("MetricVolume", "Volume");
                    case ENGonSoftMetric::Hybrid: return LOCTEXT("MetricHybrid", "Hybrid");
                    default: return LOCTEXT("MetricEdge", "Edge Surface");
                    }
                }) ]
        ]
        + SWrapBox::Slot()
        [ SNew(STextBlock).Text(LOCTEXT("SoftConnectivity", "Mask")) ]
        + SWrapBox::Slot()
        [
            SNew(SComboButton)
            .OnGetMenuContent_Lambda([this]
            {
                FMenuBuilder Menu(true, nullptr);
                const auto AddMask = [this, &Menu](ENGonSoftConnectivity Mask, const TCHAR* Label)
                {
                    Menu.AddMenuEntry(FText::FromString(Label), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([this, Mask] { SoftConnectivity = Mask; ApplySoftSelectionSettings(); })));
                };
                AddMask(ENGonSoftConnectivity::SameIsland, TEXT("Same Island"));
                AddMask(ENGonSoftConnectivity::Unrestricted, TEXT("Unrestricted"));
                AddMask(ENGonSoftConnectivity::FloodWithinRadius, TEXT("Flood Within Radius"));
                return Menu.MakeWidget();
            })
            .ButtonContent()
            [ SNew(STextBlock).Text_Lambda([this]
                {
                    switch (SoftConnectivity)
                    {
                    case ENGonSoftConnectivity::Unrestricted: return LOCTEXT("MaskNone", "Unrestricted");
                    case ENGonSoftConnectivity::FloodWithinRadius: return LOCTEXT("MaskFlood", "Flood Within Radius");
                    default: return LOCTEXT("MaskIsland", "Same Island");
                    }
                }) ]
        ]
        + SWrapBox::Slot()
        [ SNew(STextBlock).Text(LOCTEXT("SoftBlend", "Blend")) ]
        + SWrapBox::Slot()
        [
            SNew(SBox).WidthOverride(110)
            [
                SNew(SSlider)
                .MinValue(0.0f).MaxValue(1.0f)
                .IsEnabled_Lambda([this] { return bSoftSelection && SoftMetric == ENGonSoftMetric::Hybrid; })
                .Value_Lambda([this] { return SoftSurfaceBlend; })
                .OnValueChanged_Lambda([this](float Value) { SoftSurfaceBlend = Value; ApplySoftSelectionSettings(); })
            ]
        ]
        + SWrapBox::Slot()
        [ SNew(STextBlock).Text(LOCTEXT("SoftMode", "Mode")) ]
        + SWrapBox::Slot()
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return SoftMode == ENGonSoftTransformMode::LiveFalloff ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                SoftMode = State == ECheckBoxState::Checked ? ENGonSoftTransformMode::LiveFalloff : ENGonSoftTransformMode::Fixed;
                ApplySoftSelectionSettings();
            })
            [ SNew(STextBlock).Text(LOCTEXT("LiveFalloff", "Live Falloff")) ]
        ]
        + SWrapBox::Slot()
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return SoftMode == ENGonSoftTransformMode::SlideSculpt ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                SoftMode = State == ECheckBoxState::Checked ? ENGonSoftTransformMode::SlideSculpt : ENGonSoftTransformMode::Fixed;
                ApplySoftSelectionSettings();
            })
            [ SNew(STextBlock).Text(LOCTEXT("SlideSculpt", "Slide Sculpt")) ]
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeStatus()
{
    return SNew(SBorder).Padding(FMargin(8, 5))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Text_Lambda([this]
                {
                    return FText::Format(LOCTEXT("SourceLine", "{0}  |  {1}"),
                        SourceLabel, Viewport->Preview().DescribeMesh());
                })
                .AutoWrapText(true)
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Text_Lambda([this]
                {
                    return FText::Format(LOCTEXT("SelectionLine", "{0}  |  {1}"),
                        Viewport->Preview().DescribeSelection(), Viewport->Preview().DescribeUpdates());
                })
                .AutoWrapText(true)
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Text_Lambda([this] { return ErrorText; })
                .Visibility_Lambda([this]
                {
                    return ErrorText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
                })
                .ColorAndOpacity(FLinearColor(1.0f, 0.58f, 0.25f))
                .AutoWrapText(true)
            ]
        ];
}

FReply SNGonPreview::LoadDemo()
{
    FNGonMeshData Data;
    FText Error;
    if (NGon::MakeDemo(Data, Error))
    {
        AssetPath.Reset();
        ShowMesh(MoveTemp(Data), LOCTEXT("DemoName", "Demo mesh"));
    }
    else
    {
        ShowError(Error);
    }
    return FReply::Handled();
}

void SNGonPreview::LoadStaticMesh(const FAssetData& Asset)
{
    if (!Asset.IsValid())
    {
        FText Error;
        if (!TryLoadDefault(Error))
        {
            LoadDemo();
            ShowError(Error);
        }
        return;
    }

    UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset.GetAsset());
    const FMeshDescription* Source = StaticMesh ? StaticMesh->GetMeshDescription(0) : nullptr;
    if (!Source)
    {
        ShowError(LOCTEXT("NoSourceLOD", "This Static Mesh has no editable LOD 0 mesh description."));
        return;
    }

    FNGonMeshData Data;
    FText Error;
    if (!NGon::ConvertMeshDescription(*Source, Data, Error))
    {
        ShowError(Error);
        return;
    }

    const bool bTriangleOnly = Data.Topology.MaxFaceCorners == 3;
    AssetPath = Asset.GetSoftObjectPath().ToString();
    ShowMesh(MoveTemp(Data), FText::FromName(Asset.AssetName));
    if (bTriangleOnly)
    {
        ShowError(LOCTEXT("TrianglesOnly",
            "LOD 0 contains triangle faces only. Open the untriangulated source FBX to inspect the authored polygons."));
    }
}

FReply SNGonPreview::ReloadDefault()
{
    FText Error;
    if (!TryLoadDefault(Error))
    {
        ShowError(Error);
    }
    return FReply::Handled();
}

bool SNGonPreview::TryLoadDefault(FText& Error)
{
    FString Filename;
    return NGon::FindDefaultFbx(NGon::GetMeshesDirectory(), Filename, Error)
        && TryLoadFbx(Filename, Error);
}

bool SNGonPreview::TryLoadFbx(const FString& Filename, FText& Error)
{
    FNGonMeshData Data;
    const FText Name = FText::FromString(FPaths::GetCleanFilename(Filename));
    if (!NGon::LoadFbx(Filename, Data, Error))
    {
        Error = FText::Format(LOCTEXT("LoadFbxFailed", "Could not load {0}. {1}"), Name, Error);
        return false;
    }

    const bool bTriangleOnly = Data.Topology.MaxFaceCorners == 3;
    AssetPath.Reset();
    ShowMesh(MoveTemp(Data), Name);
    if (bTriangleOnly)
    {
        ShowError(LOCTEXT("FbxIsTriangulated", "This FBX contains triangulated faces only. Export it with Triangulate disabled to preview authored polygons."));
    }
    return true;
}

FReply SNGonPreview::OpenFbx()
{
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    if (!Desktop)
    {
        ShowError(LOCTEXT("NoFileDialog", "The native file dialog is unavailable."));
        return FReply::Handled();
    }

    const FString Meshes = NGon::GetMeshesDirectory();
    const FString Directory = IFileManager::Get().DirectoryExists(*Meshes) ? Meshes : FPaths::ProjectDir();
    TArray<FString> Files;
    const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (Desktop->OpenFileDialog(Parent, LOCTEXT("OpenFbxFile", "Open FBX Mesh").ToString(),
        Directory, FString(), TEXT("FBX (*.fbx)|*.fbx"), 0, Files) && !Files.IsEmpty())
    {
        FText Error;
        if (!TryLoadFbx(Files[0], Error))
        {
            ShowError(Error);
        }
    }
    return FReply::Handled();
}

void SNGonPreview::ShowMesh(FNGonMeshData&& Data, const FText& SourceName)
{
    SourceLabel = SourceName;
    ErrorText = FText::GetEmpty();
    HeatBias = 0.0f;
    Bend = 0.0f;
    Viewport->SetMesh(MoveTemp(Data));
    ApplySoftSelectionSettings();
}

void SNGonPreview::ShowError(const FText& Error)
{
    ErrorText = Error;
}

void SNGonPreview::SetSelectionMode(ENGonElement Element)
{
    SelectionMode = Element;
    Viewport->SetSelectionMode(Element);
}

#undef LOCTEXT_NAMESPACE
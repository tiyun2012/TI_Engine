#include "SNGonPreview.h"

#include "Animation/MorphTarget.h"
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
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "IDesktopPlatform.h"
#include "InputCoreTypes.h"
#include "InputRouter.h"
#include "InputState.h"
#include "InteractiveGizmoManager.h"
#include "InteractiveToolManager.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "NGonFbx.h"
#include "NGonMeshFiles.h"
#include "NGonPreviewScene.h"

namespace NGon
{
    void SetActiveViewportUVMesh(const FNGonMeshData& Data, const FText& InSourceName, const FText& InSourceType);
    void UpdateActiveViewportUVMeshFromTopology(const FNGonTopology& Topology);
    void NotifyHostViewportClosed();
}
#include "PropertyCustomizationHelpers.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SceneView.h"
#include "Styling/CoreStyle.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "NGonPreview"

namespace
{
// Base classes are destroyed in reverse order. Keep both the preview scene and
// the viewport-local ModeTools alive through the FEditorViewportClient destructor.
enum class ENGonTreeIconKind : uint8
{
    Bone,
    Socket,
    TabSkeleton,
    TabMorph,
    TabCurves,
    Plus
};

struct FNGonSocketEntry
{
    int32 SocketIndex = INDEX_NONE;
    int32 ParentBoneIndex = INDEX_NONE;
    FName SocketName = NAME_None;
    FName BoneName = NAME_None;
    FTransform RelativeTransform = FTransform::Identity;
};

struct FNGonSkeletonTreeUIState
{
    TSet<int32> CollapsedBones;
    TArray<FNGonSocketEntry> Sockets;
    int32 SelectedSocketIndex = INDEX_NONE;
};

TMap<const SNGonPreview*, FNGonSkeletonTreeUIState> GSkeletonTreeUIStateMap;

FNGonSkeletonTreeUIState& GetSkeletonTreeUIState(const SNGonPreview* Preview)
{
    return GSkeletonTreeUIStateMap.FindOrAdd(Preview);
}

void DrawBoneIconVector(
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FGeometry& AllottedGeometry,
    const FVector2D& Center,
    const FLinearColor& Color)
{
    // 45-degree bone shaft from bottom-left to top-right
    const FVector2D BL = Center + FVector2D(-3.2f, 3.2f);
    const FVector2D TR = Center + FVector2D(3.2f, -3.2f);
    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        { BL, TR },
        ESlateDrawEffect::None,
        Color,
        true,
        2.1f);

    // Bottom-left and top-right dual condyle knobs (perpendicular to shaft)
    const FVector2D BL_KnobA = Center + FVector2D(-4.8f, 2.1f);
    const FVector2D BL_KnobB = Center + FVector2D(-2.1f, 4.8f);
    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        { BL_KnobA, BL, BL_KnobB },
        ESlateDrawEffect::None,
        Color,
        true,
        2.2f);

    const FVector2D TR_KnobA = Center + FVector2D(2.1f, -4.8f);
    const FVector2D TR_KnobB = Center + FVector2D(4.8f, -2.1f);
    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        { TR_KnobA, TR, TR_KnobB },
        ESlateDrawEffect::None,
        Color,
        true,
        2.2f);
}

void DrawSocketIconVector(
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FGeometry& AllottedGeometry,
    const FVector2D& Center,
    const FLinearColor& Color)
{
    TArray<FVector2D> Ring;
    constexpr int32 NumPts = 12;
    constexpr float Radius = 4.2f;
    Ring.Reserve(NumPts + 1);
    for (int32 i = 0; i <= NumPts; ++i)
    {
        const float Angle = (static_cast<float>(i) / static_cast<float>(NumPts)) * 2.0f * PI;
        Ring.Add(Center + FVector2D(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius));
    }
    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        Ring,
        ESlateDrawEffect::None,
        Color,
        true,
        1.5f);

    // 4 diagonal socket prongs
    for (int32 Corner = 0; Corner < 4; ++Corner)
    {
        const float Angle = (0.25f + 0.5f * static_cast<float>(Corner)) * PI;
        const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
        FSlateDrawElement::MakeLines(
            OutDrawElements,
            LayerId,
            AllottedGeometry.ToPaintGeometry(),
            { Center + Dir * 3.8f, Center + Dir * 6.0f },
            ESlateDrawEffect::None,
            Color,
            true,
            1.4f);
    }

    static FSlateBrush SolidBox;
    SolidBox.DrawAs = ESlateBrushDrawType::Box;
    FSlateDrawElement::MakeBox(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(FVector2D(2.4f, 2.4f), FSlateLayoutTransform(Center - FVector2D(1.2f, 1.2f))),
        &SolidBox,
        ESlateDrawEffect::None,
        Color);
}

class SNGonVectorIcon final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SNGonVectorIcon)
        : _Kind(ENGonTreeIconKind::Bone)
        , _Color(FLinearColor::White)
    {}
        SLATE_ARGUMENT(ENGonTreeIconKind, Kind)
        SLATE_ARGUMENT(FLinearColor, Color)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs)
    {
        Kind = InArgs._Kind;
        IconColor = InArgs._Color;
    }

protected:
    FVector2D ComputeDesiredSize(float) const override
    {
        return FVector2D(14.0f, 14.0f);
    }

    int32 OnPaint(
        const FPaintArgs&,
        const FGeometry& AllottedGeometry,
        const FSlateRect&,
        FSlateWindowElementList& OutDrawElements,
        int32 LayerId,
        const FWidgetStyle&,
        bool) const override
    {
        const FVector2D Center = AllottedGeometry.GetLocalSize() * 0.5f;
        switch (Kind)
        {
        case ENGonTreeIconKind::Bone:
        case ENGonTreeIconKind::TabSkeleton:
            DrawBoneIconVector(OutDrawElements, LayerId, AllottedGeometry, Center, IconColor);
            break;
        case ENGonTreeIconKind::Socket:
            DrawSocketIconVector(OutDrawElements, LayerId, AllottedGeometry, Center, IconColor);
            break;
        case ENGonTreeIconKind::TabMorph:
        {
            TArray<FVector2D> HeadRing;
            for (int32 i = 0; i <= 12; ++i)
            {
                const float A = (static_cast<float>(i) / 12.0f) * 2.0f * PI;
                HeadRing.Add(Center + FVector2D(FMath::Cos(A) * 4.8f, FMath::Sin(A) * 4.8f));
            }
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), HeadRing, ESlateDrawEffect::None, IconColor, true, 1.4f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-2.2f, 1.2f), Center + FVector2D(0.0f, 2.6f), Center + FVector2D(2.2f, 1.2f) },
                ESlateDrawEffect::None, IconColor, true, 1.3f);
            break;
        }
        case ENGonTreeIconKind::TabCurves:
        {
            TArray<FVector2D> CurvePts = {
                Center + FVector2D(-5.0f, 4.0f),
                Center + FVector2D(-2.0f, 3.5f),
                Center + FVector2D(0.0f, 0.0f),
                Center + FVector2D(2.0f, -3.5f),
                Center + FVector2D(5.0f, -4.0f)
            };
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), CurvePts, ESlateDrawEffect::None, IconColor, true, 1.6f);
            break;
        }
        case ENGonTreeIconKind::Plus:
        {
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-4.5f, 0.0f), Center + FVector2D(4.5f, 0.0f) },
                ESlateDrawEffect::None, IconColor, false, 2.0f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(0.0f, -4.5f), Center + FVector2D(0.0f, 4.5f) },
                ESlateDrawEffect::None, IconColor, false, 2.0f);
            break;
        }
        }
        return LayerId + 1;
    }

private:
    ENGonTreeIconKind Kind = ENGonTreeIconKind::Bone;
    FLinearColor IconColor = FLinearColor::White;
};

class SNGonSkeletonTreeRowPrefix final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SNGonSkeletonTreeRowPrefix)
        : _Depth(0)
        , _bIsLastChild(true)
        , _bHasChildren(false)
        , _bIsExpanded(true)
        , _bIsSocket(false)
        , _bFilterActive(false)
        , _bIsSelected(false)
    {}
        SLATE_ARGUMENT(int32, Depth)
        SLATE_ARGUMENT(TArray<bool>, AncestorHasMoreSiblings)
        SLATE_ARGUMENT(bool, bIsLastChild)
        SLATE_ARGUMENT(bool, bHasChildren)
        SLATE_ARGUMENT(bool, bIsExpanded)
        SLATE_ARGUMENT(bool, bIsSocket)
        SLATE_ARGUMENT(bool, bFilterActive)
        SLATE_ARGUMENT(bool, bIsSelected)
        SLATE_EVENT(FSimpleDelegate, OnToggleExpand)
    SLATE_END_ARGS()

    static constexpr float StepX = 14.0f;
    static constexpr float IconSlotWidth = 20.0f;

    void Construct(const FArguments& InArgs)
    {
        Depth = FMath::Max(0, InArgs._Depth);
        AncestorHasMoreSiblings = InArgs._AncestorHasMoreSiblings;
        bIsLastChild = InArgs._bIsLastChild;
        bHasChildren = InArgs._bHasChildren;
        bIsExpanded = InArgs._bIsExpanded;
        bIsSocket = InArgs._bIsSocket;
        bFilterActive = InArgs._bFilterActive;
        bIsSelected = InArgs._bIsSelected;
        OnToggleExpand = InArgs._OnToggleExpand;
    }

protected:
    FVector2D ComputeDesiredSize(float) const override
    {
        const float TreeColumnsWidth = bFilterActive ? 14.0f : static_cast<float>(Depth + 1) * StepX;
        return FVector2D(TreeColumnsWidth + IconSlotWidth, 21.0f);
    }

    int32 OnPaint(
        const FPaintArgs&,
        const FGeometry& AllottedGeometry,
        const FSlateRect&,
        FSlateWindowElementList& OutDrawElements,
        int32 LayerId,
        const FWidgetStyle&,
        bool) const override
    {
        const FVector2D Size = AllottedGeometry.GetLocalSize();
        const float H = Size.Y;
        const float CenterY = H * 0.5f;
        const FLinearColor WireColor = bIsSelected
            ? FLinearColor(0.70f, 0.85f, 1.0f, 0.85f)
            : FLinearColor(0.32f, 0.35f, 0.40f, 0.90f);

        if (!bFilterActive)
        {
            // 1. Continuous vertical guide lines for ancestor columns that still have siblings below
            for (int32 Level = 0; Level < Depth - 1; ++Level)
            {
                if (AncestorHasMoreSiblings.IsValidIndex(Level) && AncestorHasMoreSiblings[Level])
                {
                    const float X = static_cast<float>(Level) * StepX + 7.0f;
                    FSlateDrawElement::MakeLines(
                        OutDrawElements,
                        LayerId,
                        AllottedGeometry.ToPaintGeometry(),
                        { FVector2D(X, 0.0f), FVector2D(X, H) },
                        ESlateDrawEffect::None,
                        WireColor,
                        false,
                        1.0f);
                }
            }

            // 2. Immediate parent vertical & horizontal connector (Depth - 1 -> Depth)
            if (Depth > 0)
            {
                const float ParentX = static_cast<float>(Depth - 1) * StepX + 7.0f;
                const float VertEndY = bIsLastChild ? CenterY : H;
                FSlateDrawElement::MakeLines(
                    OutDrawElements,
                    LayerId,
                    AllottedGeometry.ToPaintGeometry(),
                    { FVector2D(ParentX, 0.0f), FVector2D(ParentX, VertEndY) },
                    ESlateDrawEffect::None,
                    WireColor,
                    false,
                    1.0f);

                const float HorizEndX = bHasChildren
                    ? (static_cast<float>(Depth) * StepX + 2.0f)
                    : (static_cast<float>(Depth + 1) * StepX + 1.0f);
                FSlateDrawElement::MakeLines(
                    OutDrawElements,
                    LayerId,
                    AllottedGeometry.ToPaintGeometry(),
                    { FVector2D(ParentX, CenterY), FVector2D(HorizEndX, CenterY) },
                    ESlateDrawEffect::None,
                    WireColor,
                    false,
                    1.0f);
            }

            // 3. Expander arrow (▼ when expanded, ▶ when collapsed) & downward wire stub to first child
            const float ArrowCenterX = static_cast<float>(Depth) * StepX + 7.0f;
            if (bHasChildren)
            {
                if (bIsExpanded)
                {
                    FSlateDrawElement::MakeLines(
                        OutDrawElements,
                        LayerId,
                        AllottedGeometry.ToPaintGeometry(),
                        { FVector2D(ArrowCenterX, CenterY + 4.0f), FVector2D(ArrowCenterX, H) },
                        ESlateDrawEffect::None,
                        WireColor,
                        false,
                        1.0f);
                }

                const FLinearColor ArrowColor = bIsSelected
                    ? FLinearColor(0.95f, 0.98f, 1.0f, 1.0f)
                    : FLinearColor(0.72f, 0.75f, 0.80f, 1.0f);

                if (bIsExpanded)
                {
                    // Filled downward triangle ▼
                    for (int32 Row = 0; Row < 5; ++Row)
                    {
                        const float HalfW = 3.6f - static_cast<float>(Row) * 0.85f;
                        const float Y = CenterY - 2.0f + static_cast<float>(Row);
                        FSlateDrawElement::MakeLines(
                            OutDrawElements,
                            LayerId + 1,
                            AllottedGeometry.ToPaintGeometry(),
                            { FVector2D(ArrowCenterX - HalfW, Y), FVector2D(ArrowCenterX + HalfW, Y) },
                            ESlateDrawEffect::None,
                            ArrowColor,
                            false,
                            1.2f);
                    }
                }
                else
                {
                    // Filled right-pointing triangle ▶
                    for (int32 Col = 0; Col < 5; ++Col)
                    {
                        const float HalfH = 3.6f - static_cast<float>(Col) * 0.85f;
                        const float X = ArrowCenterX - 2.0f + static_cast<float>(Col);
                        FSlateDrawElement::MakeLines(
                            OutDrawElements,
                            LayerId + 1,
                            AllottedGeometry.ToPaintGeometry(),
                            { FVector2D(X, CenterY - HalfH), FVector2D(X, CenterY + HalfH) },
                            ESlateDrawEffect::None,
                            ArrowColor,
                            false,
                            1.2f);
                    }
                }
            }
        }

        // 4. Bone or Socket icon
        const float IconStartX = bFilterActive ? 14.0f : static_cast<float>(Depth + 1) * StepX;
        const FVector2D IconCenter(IconStartX + IconSlotWidth * 0.5f, CenterY);
        if (bIsSocket)
        {
            const FLinearColor SocketColor = bIsSelected
                ? FLinearColor(1.0f, 0.95f, 0.82f, 1.0f)
                : FLinearColor(0.76f, 0.80f, 0.86f, 1.0f);
            DrawSocketIconVector(OutDrawElements, LayerId + 2, AllottedGeometry, IconCenter, SocketColor);
        }
        else
        {
            const FLinearColor BoneColor = bIsSelected
                ? FLinearColor(1.0f, 1.0f, 1.0f, 1.0f)
                : (bHasChildren
                    ? FLinearColor(0.84f, 0.86f, 0.90f, 1.0f)
                    : FLinearColor(0.68f, 0.71f, 0.76f, 1.0f));
            DrawBoneIconVector(OutDrawElements, LayerId + 2, AllottedGeometry, IconCenter, BoneColor);
        }

        return LayerId + 3;
    }

    FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
    {
        if (!bFilterActive && bHasChildren && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
        {
            const FVector2D LocalPos = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
            const float ArrowMinX = static_cast<float>(Depth) * StepX - 2.0f;
            const float ArrowMaxX = static_cast<float>(Depth + 1) * StepX + 3.0f;
            if (LocalPos.X >= ArrowMinX && LocalPos.X <= ArrowMaxX)
            {
                if (OnToggleExpand.IsBound())
                {
                    OnToggleExpand.Execute();
                }
                return FReply::Handled();
            }
        }
        return FReply::Unhandled();
    }

private:
    int32 Depth = 0;
    TArray<bool> AncestorHasMoreSiblings;
    bool bIsLastChild = true;
    bool bHasChildren = false;
    bool bIsExpanded = true;
    bool bIsSocket = false;
    bool bFilterActive = false;
    bool bIsSelected = false;
    FSimpleDelegate OnToggleExpand;
};

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

                PreviewOwner->SetOverlayMaterials(ToolManager);
                PreviewOwner->SetupGizmo(GizmoManager, ToolManager);
            }
        }
    }

    void SetGridShowFlag(bool bVisible)
    {
        EngineShowFlags.SetGrid(bVisible);
        Invalidate();
    }

    EMouseCursor::Type GetCursor(FViewport* InViewport, int32 X, int32 Y) override
    {
        if (InViewport && !IsAltPressed())
        {
            if (bConsumedByGizmo || (InputRouter && InputRouter->HasActiveMouseCapture()) || PreviewOwner->IsSoftTransformActive())
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
        if (EventArgs.Event == IE_Pressed && EventArgs.Key == EKeys::Escape && PreviewOwner->IsSoftTransformActive())
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
                        if (InteractiveToolsContext)
                        {
                            InteractiveToolsContext->MouseMove(this, EventArgs.Viewport, MouseX, MouseY);
                            InteractiveToolsContext->UpdateStateWithoutRoutingInputKey(
                                this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event);
                        }

                        FInputDeviceState HoverState = MakeMouseInputState(*View, MouseX, MouseY, FVector2D::ZeroVector);
                        InputRouter->PostHoverInputEvent(HoverState);

                        FInputDeviceState State = MakeMouseInputState(*View, MouseX, MouseY, FVector2D::ZeroVector);
                        State.Mouse.Left.SetStates(true, true, false);
                        const bool bCapturedOnPress = InputRouter->PostInputEvent(State);

                        if (bCapturedOnPress || InputRouter->HasActiveMouseCapture() || PreviewOwner->IsSoftTransformActive())
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
                if (bHasPendingGizmoDrag)
                {
                    FlushPendingGizmoDrag(EventArgs.Viewport);
                }
                const FVector2D Delta = CurrentPos - LastMousePosition;
                LastMousePosition = CurrentPos;
                const bool bWasGizmoInteraction = bConsumedByGizmo
                    || (InputRouter && InputRouter->HasActiveMouseCapture())
                    || PreviewOwner->IsSoftTransformActive();
                bLeftMouseButtonDown = false;
                bHasPendingGizmoDrag = false;

                FSceneViewFamilyContext ViewFamily(
                    FSceneViewFamily::ConstructionValues(EventArgs.Viewport, GetScene(), EngineShowFlags)
                    .SetRealtimeUpdate(IsRealtime()));
                FSceneView* View = BuildSceneView(EventArgs.Viewport, ViewFamily);

                if (InteractiveToolsContext)
                {
                    InteractiveToolsContext->UpdateStateWithoutRoutingInputKey(
                        this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event);
                }

                if (bWasGizmoInteraction)
                {
                    if (View && InputRouter)
                    {
                        FInputDeviceState State = MakeMouseInputState(*View, MouseX, MouseY, Delta);
                        State.Mouse.Left.SetStates(false, false, true);
                        InputRouter->PostInputEvent(State);
                    }
                    if (GizmoManager)
                    {
                        GizmoManager->Tick(0.0f);
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
                if (bHasPendingGizmoDrag)
                {
                    FlushPendingGizmoDrag(EventArgs.Viewport);
                }
                if (InteractiveToolsContext)
                {
                    InteractiveToolsContext->UpdateStateWithoutRoutingInputKey(
                        this, EventArgs.Viewport, EventArgs.Key, EventArgs.Event);
                }
                if (bConsumedByGizmo || (InputRouter && InputRouter->HasActiveMouseCapture()) || PreviewOwner->IsSoftTransformActive())
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
                if (GizmoManager)
                {
                    GizmoManager->Tick(0.0f);
                }
                if (InteractiveToolsContext)
                {
                    InteractiveToolsContext->MouseMove(this, InViewport, X, Y);
                }
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

    void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override
    {
        if (View && GizmoViewContext)
        {
            GizmoViewContext->ResetFromSceneView(*View);
        }
        FEditorViewportClient::Draw(View, PDI);
    }

    void Draw(FViewport* InViewport, FCanvas* Canvas) override
    {
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
            if (InteractiveToolsContext)
            {
                InteractiveToolsContext->MouseMove(this, InViewport, InMouseX, InMouseY);
            }
            if (InputRouter)
            {
                FInputDeviceState State = MakeMouseInputState(*View, InMouseX, InMouseY, Delta);
                State.Mouse.Left.SetStates(false, true, false);
                InputRouter->PostInputEvent(State);
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
        if (bConsumedByGizmo || (InputRouter && InputRouter->HasActiveMouseCapture()) || PreviewOwner->IsSoftTransformActive())
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

    TWeakPtr<SNGonViewport> Widget;
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

void SNGonPreview::Construct(const FArguments& InArgs)
{
    SAssignNew(Viewport, SNGonViewport);

    ChildSlot
    [
        SNew(SVerticalBox)
        // 1. Top Asset & Mode Command Bar (Maya/SkeletalMesh Editor Top Header)
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            MakeTopCommandBar()
        ]
        // 2. Main Workspace Body: [Left Vertical Tool Strip] | [Center Viewport + HUD] | [Right Splitter Inspector]
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SNew(SHorizontalBox)
            // Far-Left Vertical Tool Strip (Skin / Deform / Model / Sculpt / Select / Display)
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                MakeLeftToolSidebar()
            ]
            // Center Viewport + Right Docked Inspector Splitter
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SNew(SSplitter)
                .Orientation(Orient_Horizontal)
                + SSplitter::Slot()
                .Value(0.70f)
                [
                    SNew(SVerticalBox)
                    // Viewport Top Floating Toolbar
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        MakeViewportOverlayHeader()
                    ]
                    // 3D Viewport with Top-Left Stats Overlay and Top-Right Hotkeys Overlay
                    + SVerticalBox::Slot()
                    .FillHeight(1.0f)
                    [
                        SNew(SOverlay)
                        + SOverlay::Slot()
                        [
                            Viewport.ToSharedRef()
                        ]
                        + SOverlay::Slot()
                        .HAlign(HAlign_Left)
                        .VAlign(VAlign_Top)
                        .Padding(12.0f, 10.0f)
                        [
                            MakeViewportStatsOverlay()
                        ]
                        + SOverlay::Slot()
                        .HAlign(HAlign_Right)
                        .VAlign(VAlign_Top)
                        .Padding(12.0f, 10.0f)
                        [
                            MakeViewportHotkeysOverlay()
                        ]
                    ]
                    // Bottom Status & Transport Bar
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        MakeStatus()
                    ]
                ]
                + SSplitter::Slot()
                .Value(0.30f)
                [
                    MakeRightInspectorPanel()
                ]
            ]
        ]
    ];

    FText Error;
    if (!TryLoadDefault(Error))
    {
        LoadDemo();
        ShowError(Error);
    }
}

TSharedRef<SWidget> SNGonPreview::MakeTopCommandBar()
{
    return SNew(SBorder)
        .Padding(FMargin(8.0f, 6.0f))
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(8.0f, 4.0f))
            + SWrapBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(SButton)
                .Text(LOCTEXT("OpenFbx", "Open FBX"))
                .ToolTipText(LOCTEXT("OpenFbxTip", "Import an untriangulated FBX mesh with authored N-gon polygons."))
                .OnClicked(this, &SNGonPreview::OpenFbx)
            ]
            + SWrapBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(SButton)
                .Text(LOCTEXT("Default", "Reload Default"))
                .OnClicked(this, &SNGonPreview::ReloadDefault)
            ]
            + SWrapBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(SButton)
                .Text(LOCTEXT("Demo", "Demo Mesh"))
                .OnClicked(this, &SNGonPreview::LoadDemo)
            ]
            + SWrapBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("StaticMeshLabel", "Static Mesh:"))
                .ColorAndOpacity(FLinearColor(0.75f, 0.82f, 0.90f))
            ]
            + SWrapBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(SBox)
                .WidthOverride(220.0f)
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UStaticMesh::StaticClass())
                    .ObjectPath_Lambda([this] { return StaticMeshAssetPath; })
                    .OnObjectChanged(this, &SNGonPreview::LoadStaticMesh)
                    .DisplayThumbnail(false)
                    .DisplayUseSelected(true)
                    .AllowCreate(false)
                ]
            ]
            + SWrapBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("SkeletalMeshLabel", "Skeletal Mesh:"))
                .ColorAndOpacity(FLinearColor(0.55f, 0.88f, 1.0f))
            ]
            + SWrapBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(SBox)
                .WidthOverride(220.0f)
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(USkeletalMesh::StaticClass())
                    .ObjectPath_Lambda([this] { return SkeletalMeshAssetPath; })
                    .OnObjectChanged(this, &SNGonPreview::LoadSkeletalMesh)
                    .DisplayThumbnail(false)
                    .DisplayUseSelected(true)
                    .AllowCreate(false)
                ]
            ]
            + SWrapBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(SButton)
                .Text(LOCTEXT("OpenUVEditorBtn", "UV Editor"))
                .ToolTipText(LOCTEXT("OpenUVEditorBtnTip", "Open the N-Gon UV Editor window."))
                .OnClicked_Lambda([]
                {
                    FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("NGonUVEditor")));
                    return FReply::Handled();
                })
            ]
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeLeftToolSidebar()
{
    auto MakeToolButton = [this](ENGonToolShelfTab Tab, const FText& IconText, const FText& Label, const FText& Tooltip) -> TSharedRef<SWidget>
    {
        return SNew(SBox)
            .WidthOverride(62.0f)
            .HeightOverride(54.0f)
            .Padding(FMargin(2.0f, 2.0f))
            [
                SNew(SCheckBox)
                .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
                .ToolTipText(Tooltip)
                .IsChecked_Lambda([this, Tab]
                {
                    return ActiveToolTab == Tab ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([this, Tab](ECheckBoxState)
                {
                    SetActiveToolTab(Tab);
                })
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .HAlign(HAlign_Center)
                    .Padding(0.0f, 4.0f, 0.0f, 2.0f)
                    [
                        SNew(STextBlock)
                        .Text(IconText)
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
                        .ColorAndOpacity_Lambda([this, Tab]
                        {
                            return ActiveToolTab == Tab
                                ? FLinearColor(0.25f, 0.78f, 1.0f)
                                : FLinearColor(0.75f, 0.78f, 0.82f);
                        })
                    ]
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .HAlign(HAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(Label)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                    ]
                ]
            ];
    };

    return SNew(SBorder)
        .Padding(FMargin(2.0f, 4.0f))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeToolButton(
                    ENGonToolShelfTab::Skin,
                    LOCTEXT("SkinIcon", "SKIN"),
                    LOCTEXT("SkinLabel", "Skin / Rig"),
                    LOCTEXT("SkinTip", "Inspect Skeletal Mesh bone hierarchy, joint transforms, and select vertices near joints."))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeToolButton(
                    ENGonToolShelfTab::Sculpt,
                    LOCTEXT("SculptIcon", "SCULPT"),
                    LOCTEXT("SculptLabel", "Sculpt"),
                    LOCTEXT("SculptTip", "Activate Maya-style Slide Sculpt soft-selection brush workflow."))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeToolButton(
                    ENGonToolShelfTab::Deform,
                    LOCTEXT("DeformIcon", "DEFORM"),
                    LOCTEXT("DeformLabel", "Deform"),
                    LOCTEXT("DeformTip", "Configure Live Falloff soft deformation, Morph Targets, and non-linear Bend deformer."))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeToolButton(
                    ENGonToolShelfTab::Model,
                    LOCTEXT("ModelIcon", "MODEL"),
                    LOCTEXT("ModelLabel", "Model"),
                    LOCTEXT("ModelTip", "N-Gon polygon, edge, and vertex modeling and face deletion tools."))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeToolButton(
                    ENGonToolShelfTab::Select,
                    LOCTEXT("SelectIcon", "SELECT"),
                    LOCTEXT("SelectLabel", "Select"),
                    LOCTEXT("SelectTip", "Component selection modes, edge loops, and gizmo pivot frame policies."))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeToolButton(
                    ENGonToolShelfTab::Display,
                    LOCTEXT("DisplayIcon", "VIEW"),
                    LOCTEXT("DisplayLabel", "Display"),
                    LOCTEXT("DisplayTip", "Viewport overlays, wireframe/point display, grid, and HUD settings."))
            ]
        ];
}

void SNGonPreview::SetActiveToolTab(ENGonToolShelfTab Tab)
{
    ActiveToolTab = Tab;
    ActiveLowerTab = ENGonRightLowerTab::Details;

    if (Tab == ENGonToolShelfTab::Skin)
    {
        ActiveUpperTab = ENGonRightUpperTab::SkeletonTree;
    }
    else if (Tab == ENGonToolShelfTab::Sculpt)
    {
        bSoftSelection = true;
        SoftMode = ENGonSoftTransformMode::SlideSculpt;
        ApplySoftSelectionSettings();
    }
    else if (Tab == ENGonToolShelfTab::Deform)
    {
        ActiveUpperTab = ENGonRightUpperTab::MorphTargets;
        bSoftSelection = true;
        if (SoftMode == ENGonSoftTransformMode::SlideSculpt)
        {
            SoftMode = ENGonSoftTransformMode::LiveFalloff;
        }
        ApplySoftSelectionSettings();
    }
    else if (Tab == ENGonToolShelfTab::Model)
    {
        ActiveUpperTab = ENGonRightUpperTab::Topology;
    }
    else if (Tab == ENGonToolShelfTab::Display)
    {
        ActiveLowerTab = ENGonRightLowerTab::PreviewSettings;
    }
}

TSharedRef<SWidget> SNGonPreview::MakeViewportOverlayHeader()
{
    auto MakeComponentToggle = [this](ENGonElement Element, const FText& Label) -> TSharedRef<SWidget>
    {
        return SNew(SCheckBox)
            .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
            .Padding(FMargin(6.0f, 3.0f))
            .IsChecked_Lambda([this, Element]
            {
                return Viewport->GetSelectionMode() == Element ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this, Element](ECheckBoxState)
            {
                SetSelectionMode(Element);
            })
            [
                SNew(STextBlock)
                .Text(Label)
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            ];
    };

    auto MakeGizmoToggle = [this](ENGonGizmoMode Mode, const FText& Label) -> TSharedRef<SWidget>
    {
        return SNew(SCheckBox)
            .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
            .Padding(FMargin(6.0f, 3.0f))
            .IsChecked_Lambda([this, Mode]
            {
                return Viewport->GetGizmoMode() == Mode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this, Mode](ECheckBoxState)
            {
                Viewport->SetGizmoMode(Mode);
            })
            [
                SNew(STextBlock)
                .Text(Label)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
            ];
    };

    return SNew(SBorder)
        .Padding(FMargin(8.0f, 4.0f))
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(6.0f, 3.0f))
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                MakeComponentToggle(ENGonElement::Vertex, LOCTEXT("VtxBtn", "Vertex (1)"))
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                MakeComponentToggle(ENGonElement::Edge, LOCTEXT("EdgeBtn", "Edge (2)"))
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                MakeComponentToggle(ENGonElement::Face, LOCTEXT("FaceBtn", "Face (3)"))
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                SNew(STextBlock).Text(LOCTEXT("Sep1", "|")).ColorAndOpacity(FLinearColor(0.35f, 0.35f, 0.38f))
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                MakeGizmoToggle(ENGonGizmoMode::Translate, LOCTEXT("MoveBtn", "Move (W)"))
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                MakeGizmoToggle(ENGonGizmoMode::Rotate, LOCTEXT("RotBtn", "Rotate (E)"))
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                MakeGizmoToggle(ENGonGizmoMode::Scale, LOCTEXT("ScaleBtn", "Scale (R)"))
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                MakeGizmoToggle(ENGonGizmoMode::Combined, LOCTEXT("CombBtn", "Combined"))
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this]
                {
                    return Viewport->Preview().GetGizmoFramePolicy() == ENGonGizmoFramePolicy::ActiveElement
                        ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                {
                    Viewport->Preview().SetGizmoFramePolicy(
                        State == ECheckBoxState::Checked
                            ? ENGonGizmoFramePolicy::ActiveElement
                            : ENGonGizmoFramePolicy::WeightedAverage);
                    Viewport->Invalidate();
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("ActivePivot", "Active Pivot"))
                ]
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                SNew(STextBlock).Text(LOCTEXT("Sep2", "|")).ColorAndOpacity(FLinearColor(0.35f, 0.35f, 0.38f))
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return bShowEdges ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                {
                    bShowEdges = (State == ECheckBoxState::Checked);
                    Viewport->Preview().SetShowEdges(bShowEdges);
                    Viewport->Invalidate();
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("ShowEdgesToggle", "Edges"))
                ]
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return bShowVertices ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                {
                    bShowVertices = (State == ECheckBoxState::Checked);
                    Viewport->Preview().SetShowVertices(bShowVertices);
                    Viewport->Invalidate();
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("ShowVertsToggle", "Vertices"))
                ]
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return Viewport->IsHeatmapVisible() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .IsEnabled_Lambda([this] { return Viewport->Preview().HasSoftSelectionInfluence(); })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                {
                    bShowHeatmap = (State == ECheckBoxState::Checked);
                    Viewport->SetHeatmapVisible(bShowHeatmap);
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("HeatmapToggle", "Heatmap (H)"))
                ]
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return Viewport->IsGridVisible() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                {
                    Viewport->SetGridVisible(State == ECheckBoxState::Checked);
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("GridToggle", "Grid"))
                ]
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                SNew(SButton)
                .Text(LOCTEXT("FrameBtn", "Frame (F)"))
                .OnClicked_Lambda([this]
                {
                    Viewport->FrameMesh();
                    return FReply::Handled();
                })
            ]
            + SWrapBox::Slot().VAlign(VAlign_Center)
            [
                SNew(SButton)
                .Text(LOCTEXT("DelFacesBtn", "Delete Faces (Del)"))
                .OnClicked_Lambda([this]
                {
                    Viewport->DeleteSelectedFaces();
                    return FReply::Handled();
                })
            ]
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeViewportStatsOverlay()
{
    return SNew(SBorder)
        .Visibility_Lambda([this] { return bShowStatsOverlay ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
        .Padding(FMargin(8.0f, 6.0f))
        .BorderBackgroundColor(FLinearColor(0.02f, 0.03f, 0.05f, 0.65f))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                .ColorAndOpacity(FLinearColor(0.95f, 0.96f, 0.98f))
                .ShadowOffset(FVector2D(1.0f, 1.0f))
                .Text_Lambda([this]
                {
                    return FText::Format(LOCTEXT("HudPreviewing", "Previewing: {0} ({1})"), SourceLabel, SourceTypeLabel);
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .ColorAndOpacity(FLinearColor(0.88f, 0.90f, 0.94f))
                .ShadowOffset(FVector2D(1.0f, 1.0f))
                .Text_Lambda([this]
                {
                    const FNGonTopology& Topo = Viewport->Preview().GetTopology();
                    const FVector Size = Topo.RestBounds.IsValid ? Topo.RestBounds.GetSize() : FVector::ZeroVector;
                    return FText::Format(
                        LOCTEXT("HudCounts",
                            "LOD: 0\n"
                            "Triangles: {0}\n"
                            "Vertices: {1}  |  Edges: {2}\n"
                            "Polygons: {3} (Max {4} sides)\n"
                            "Bones: {5}  |  Morphs: {6}\n"
                            "Approx Size: {7} x {8} x {9}"),
                        FText::AsNumber(Topo.TriangleCount),
                        FText::AsNumber(Topo.RestPositions.Num()),
                        FText::AsNumber(Topo.Edges.Num()),
                        FText::AsNumber(Topo.Faces.Num()),
                        FText::AsNumber(Topo.MaxFaceCorners),
                        FText::AsNumber(SkeletonBones.Num()),
                        FText::AsNumber(MorphTargets.Num()),
                        FText::AsNumber(FMath::RoundToInt(Size.X)),
                        FText::AsNumber(FMath::RoundToInt(Size.Y)),
                        FText::AsNumber(FMath::RoundToInt(Size.Z)));
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                .ColorAndOpacity(FLinearColor(1.0f, 0.76f, 0.28f))
                .ShadowOffset(FVector2D(1.0f, 1.0f))
                .Text_Lambda([this]
                {
                    return Viewport->Preview().DescribeSelection();
                })
            ]
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeViewportHotkeysOverlay()
{
    auto MakeHotkeyRow = [](const FText& ActionLabel, const FText& KeyLabel) -> TSharedRef<SWidget>
    {
        return SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .ColorAndOpacity(FLinearColor(0.90f, 0.92f, 0.95f))
                .ShadowOffset(FVector2D(1.0f, 1.0f))
                .Text(ActionLabel)
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                .ColorAndOpacity(FLinearColor(0.15f, 0.78f, 1.0f))
                .ShadowOffset(FVector2D(1.0f, 1.0f))
                .Text(KeyLabel)
            ];
    };

    return SNew(SBorder)
        .Visibility_Lambda([this] { return bShowHotkeysOverlay ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
        .Padding(FMargin(8.0f, 6.0f))
        .BorderBackgroundColor(FLinearColor(0.02f, 0.03f, 0.05f, 0.65f))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkMode", "Component Mode:"), LOCTEXT("HkModeKeys", "1 Vtx / 2 Edge / 3 Face"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkGizmo", "Transform Gizmo:"), LOCTEXT("HkGizmoKeys", "W Move / E Rot / R Scale"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkEdgeLoop", "Select Edge Loop:"), LOCTEXT("HkEdgeLoopKey", "Ctrl+LMB"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkHeatmap", "Toggle Soft Heatmap:"), LOCTEXT("HkHeatmapKey", "H"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkFrame", "Frame Selection / Mesh:"), LOCTEXT("HkFrameKey", "F"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkDelete", "Delete Selected Faces:"), LOCTEXT("HkDeleteKey", "Delete"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkCamera", "Maya Camera Orbit:"), LOCTEXT("HkCameraKey", "Alt+LMB / MMB / RMB"))
            ]
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeRightInspectorPanel()
{
    auto MakeUpperTabButton = [this](ENGonRightUpperTab Tab, const FText& Label) -> TSharedRef<SWidget>
    {
        const ENGonTreeIconKind IconKind = (Tab == ENGonRightUpperTab::SkeletonTree)
            ? ENGonTreeIconKind::TabSkeleton
            : (Tab == ENGonRightUpperTab::MorphTargets ? ENGonTreeIconKind::TabMorph : ENGonTreeIconKind::TabCurves);
        return SNew(SCheckBox)
            .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
            .Padding(FMargin(8.0f, 4.0f))
            .IsChecked_Lambda([this, Tab]
            {
                return ActiveUpperTab == Tab ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this, Tab](ECheckBoxState)
            {
                ActiveUpperTab = Tab;
            })
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.0f, 0.0f, 5.0f, 0.0f)
                [
                    SNew(SNGonVectorIcon)
                    .Kind(IconKind)
                    .Color(FLinearColor(0.82f, 0.85f, 0.90f, 1.0f))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(Label)
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                ]
            ];
    };

    auto MakeLowerTabButton = [this](ENGonRightLowerTab Tab, const FText& Label) -> TSharedRef<SWidget>
    {
        return SNew(SCheckBox)
            .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
            .Padding(FMargin(8.0f, 4.0f))
            .IsChecked_Lambda([this, Tab]
            {
                return ActiveLowerTab == Tab ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this, Tab](ECheckBoxState)
            {
                ActiveLowerTab = Tab;
            })
            [
                SNew(STextBlock)
                .Text(Label)
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            ];
    };

    return SNew(SSplitter)
        .Orientation(Orient_Vertical)
        // Upper Right Dock: Skeleton Tree | Morph Targets | Topology
        + SSplitter::Slot()
        .Value(0.48f)
        [
            SNew(SBorder)
            .Padding(FMargin(6.0f))
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 6.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
                    [
                        MakeUpperTabButton(ENGonRightUpperTab::SkeletonTree, LOCTEXT("SkeletonTreeTab", "Skeleton Tree"))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
                    [
                        MakeUpperTabButton(ENGonRightUpperTab::MorphTargets, LOCTEXT("MorphTargetsTab", "Morph Targets"))
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        MakeUpperTabButton(ENGonRightUpperTab::Topology, LOCTEXT("TopologyTab", "N-Gon Topology"))
                    ]
                ]
                + SVerticalBox::Slot()
                .FillHeight(1.0f)
                [
                    SNew(SOverlay)
                    + SOverlay::Slot()
                    [
                        SNew(SBox)
                        .Visibility_Lambda([this]
                        {
                            return ActiveUpperTab == ENGonRightUpperTab::SkeletonTree ? EVisibility::Visible : EVisibility::Collapsed;
                        })
                        [
                            MakeSkeletonTreeTab()
                        ]
                    ]
                    + SOverlay::Slot()
                    [
                        SNew(SBox)
                        .Visibility_Lambda([this]
                        {
                            return ActiveUpperTab == ENGonRightUpperTab::MorphTargets ? EVisibility::Visible : EVisibility::Collapsed;
                        })
                        [
                            MakeMorphTargetsTab()
                        ]
                    ]
                    + SOverlay::Slot()
                    [
                        SNew(SBox)
                        .Visibility_Lambda([this]
                        {
                            return ActiveUpperTab == ENGonRightUpperTab::Topology ? EVisibility::Visible : EVisibility::Collapsed;
                        })
                        [
                            MakeTopologyTab()
                        ]
                    ]
                ]
            ]
        ]
        // Lower Right Dock: Details | Asset Details | Preview Settings
        + SSplitter::Slot()
        .Value(0.52f)
        [
            SNew(SBorder)
            .Padding(FMargin(6.0f))
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 6.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
                    [
                        MakeLowerTabButton(ENGonRightLowerTab::Details, LOCTEXT("DetailsTab", "Details"))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
                    [
                        MakeLowerTabButton(ENGonRightLowerTab::AssetDetails, LOCTEXT("AssetDetailsTab", "Asset Details"))
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        MakeLowerTabButton(ENGonRightLowerTab::PreviewSettings, LOCTEXT("PreviewSettingsTab", "Preview Settings"))
                    ]
                ]
                + SVerticalBox::Slot()
                .FillHeight(1.0f)
                [
                    SNew(SScrollBox)
                    + SScrollBox::Slot()
                    [
                        SNew(SOverlay)
                        + SOverlay::Slot()
                        [
                            SNew(SBox)
                            .Visibility_Lambda([this]
                            {
                                return ActiveLowerTab == ENGonRightLowerTab::Details ? EVisibility::Visible : EVisibility::Collapsed;
                            })
                            [
                                MakeDetailsTab()
                            ]
                        ]
                        + SOverlay::Slot()
                        [
                            SNew(SBox)
                            .Visibility_Lambda([this]
                            {
                                return ActiveLowerTab == ENGonRightLowerTab::AssetDetails ? EVisibility::Visible : EVisibility::Collapsed;
                            })
                            [
                                MakeAssetDetailsTab()
                            ]
                        ]
                        + SOverlay::Slot()
                        [
                            SNew(SBox)
                            .Visibility_Lambda([this]
                            {
                                return ActiveLowerTab == ENGonRightLowerTab::PreviewSettings ? EVisibility::Visible : EVisibility::Collapsed;
                            })
                            [
                                MakePreviewSettingsTab()
                            ]
                        ]
                    ]
                ]
            ]
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeSkeletonTreeTab()
{
    SAssignNew(SkeletonListBox, SVerticalBox);
    RebuildSkeletonTreeList();

    return SNew(SVerticalBox)
        // Top Toolbar: [ + ] button + Search Skeleton Tree...
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 4.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SNew(SButton)
                .ContentPadding(FMargin(8.0f, 3.0f))
                .ToolTipText(LOCTEXT("ExpandCollapseAllTip", "Toggle Expand All / Collapse Subtrees in Skeleton Tree"))
                .OnClicked_Lambda([this]
                {
                    FNGonSkeletonTreeUIState& UIState = GetSkeletonTreeUIState(this);
                    if (!UIState.CollapsedBones.IsEmpty())
                    {
                        UIState.CollapsedBones.Reset();
                    }
                    else
                    {
                        for (const FNGonBoneEntry& Bone : SkeletonBones)
                        {
                            if (Bone.Depth >= 2 && Bone.ChildCount > 0)
                            {
                                UIState.CollapsedBones.Add(Bone.BoneIndex);
                            }
                        }
                    }
                    RebuildSkeletonTreeList();
                    return FReply::Handled();
                })
                [
                    SNew(SNGonVectorIcon)
                    .Kind(ENGonTreeIconKind::Plus)
                    .Color(FLinearColor(0.32f, 0.85f, 0.35f, 1.0f))
                ]
            ]
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            .VAlign(VAlign_Center)
            [
                SNew(SSearchBox)
                .HintText(LOCTEXT("SearchSkeletonHint", "Search Skeleton Tree..."))
                .OnTextChanged_Lambda([this](const FText& NewText)
                {
                    BoneSearchFilter = NewText.ToString();
                    RebuildSkeletonTreeList();
                })
            ]
        ]
        // Column Header Bar ("Name" | Bone & Socket telemetry)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 1.0f)
        [
            SNew(SBorder)
            .Padding(FMargin(6.0f, 4.0f))
            .BorderBackgroundColor(FLinearColor(0.12f, 0.13f, 0.15f, 1.0f))
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("SkeletonColName", "Name"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                    .ColorAndOpacity(FLinearColor(0.75f, 0.78f, 0.82f, 1.0f))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                    .ColorAndOpacity(FLinearColor(0.55f, 0.60f, 0.68f, 1.0f))
                    .Text_Lambda([this]
                    {
                        if (SkeletonBones.IsEmpty())
                        {
                            return LOCTEXT("NoSkeletonHeader", "No Skeleton");
                        }
                        const int32 NumSockets = GetSkeletonTreeUIState(this).Sockets.Num();
                        if (NumSockets > 0)
                        {
                            return FText::Format(LOCTEXT("BoneAndSocketHeaderCount", "{0} bones, {1} sockets"),
                                FText::AsNumber(SkeletonBones.Num()), FText::AsNumber(NumSockets));
                        }
                        return FText::Format(LOCTEXT("BoneHeaderCountShort", "{0} bones"), FText::AsNumber(SkeletonBones.Num()));
                    })
                ]
            ]
        ]
        // Tree Scroll Area
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SNew(SBorder)
            .Padding(FMargin(0.0f))
            .BorderBackgroundColor(FLinearColor(0.055f, 0.058f, 0.065f, 1.0f))
            [
                SNew(SScrollBox)
                + SScrollBox::Slot()
                [
                    SkeletonListBox.ToSharedRef()
                ]
            ]
        ];
}

void SNGonPreview::RebuildSkeletonTreeList()
{
    if (!SkeletonListBox.IsValid())
    {
        return;
    }
    SkeletonListBox->ClearChildren();

    if (SkeletonBones.IsEmpty())
    {
        SkeletonListBox->AddSlot()
            .AutoHeight()
            .Padding(8.0f, 10.0f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("EmptySkeletonMsg",
                    "Currently inspecting a Static Mesh or FBX polygon mesh.\n"
                    "Pick any Skeletal Mesh in the top bar to inspect its Reference Skeleton hierarchy, bones, and sockets."))
                .AutoWrapText(true)
                .ColorAndOpacity(FLinearColor(0.60f, 0.64f, 0.70f))
            ];
        return;
    }

    FNGonSkeletonTreeUIState& UIState = GetSkeletonTreeUIState(this);
    const bool bHasFilter = !BoneSearchFilter.IsEmpty();

    TArray<TArray<int32>> ChildBones;
    ChildBones.SetNum(SkeletonBones.Num());
    TArray<TArray<int32>> BoneSockets;
    BoneSockets.SetNum(SkeletonBones.Num());
    TArray<int32> RootBones;

    for (int32 BoneIdx = 0; BoneIdx < SkeletonBones.Num(); ++BoneIdx)
    {
        const int32 ParentIdx = SkeletonBones[BoneIdx].ParentIndex;
        if (ParentIdx >= 0 && SkeletonBones.IsValidIndex(ParentIdx) && ParentIdx != BoneIdx)
        {
            ChildBones[ParentIdx].Add(BoneIdx);
        }
        else
        {
            RootBones.Add(BoneIdx);
        }
    }

    for (int32 SocketIdx = 0; SocketIdx < UIState.Sockets.Num(); ++SocketIdx)
    {
        const int32 ParentBone = UIState.Sockets[SocketIdx].ParentBoneIndex;
        if (SkeletonBones.IsValidIndex(ParentBone))
        {
            BoneSockets[ParentBone].Add(SocketIdx);
        }
    }

    int32 VisibleRowCounter = 0;

    auto AddTreeRowWidget = [this, &VisibleRowCounter, bHasFilter](
        const FString& DisplayName,
        int32 BoneIdx,
        int32 SocketIdx,
        int32 Depth,
        const TArray<bool>& AncestorHasMoreSiblings,
        bool bIsLastChild,
        bool bHasChildren,
        bool bIsExpanded)
    {
        const int32 RowNumber = VisibleRowCounter++;
        const bool bIsSocket = (SocketIdx != INDEX_NONE);

        auto IsRowSelected = [this, BoneIdx, SocketIdx, bIsSocket]() -> bool
        {
            const FNGonSkeletonTreeUIState& State = GetSkeletonTreeUIState(this);
            if (bIsSocket)
            {
                return State.SelectedSocketIndex == SocketIdx;
            }
            return State.SelectedSocketIndex == INDEX_NONE && SelectedBoneIndex == BoneIdx;
        };

        SkeletonListBox->AddSlot()
            .AutoHeight()
            .Padding(0.0f)
            [
                SNew(SBorder)
                .Padding(FMargin(0.0f))
                .BorderBackgroundColor_Lambda([IsRowSelected, RowNumber]()
                {
                    if (IsRowSelected())
                    {
                        return FLinearColor(0.04f, 0.38f, 0.82f, 1.0f);
                    }
                    return (RowNumber % 2 == 0)
                        ? FLinearColor(0.075f, 0.078f, 0.085f, 1.0f)
                        : FLinearColor(0.058f, 0.062f, 0.068f, 1.0f);
                })
                [
                    SNew(SCheckBox)
                    .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
                    .Padding(FMargin(4.0f, 0.0f, 6.0f, 0.0f))
                    .IsChecked_Lambda([IsRowSelected]()
                    {
                        return IsRowSelected() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([this, BoneIdx, SocketIdx, bIsSocket](ECheckBoxState)
                    {
                        FNGonSkeletonTreeUIState& State = GetSkeletonTreeUIState(this);
                        if (bIsSocket)
                        {
                            State.SelectedSocketIndex = SocketIdx;
                            SelectedBoneIndex = BoneIdx;
                        }
                        else
                        {
                            State.SelectedSocketIndex = INDEX_NONE;
                            SelectedBoneIndex = BoneIdx;
                        }
                        ActiveLowerTab = ENGonRightLowerTab::Details;
                        RebuildSkeletonTreeList();
                    })
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Fill)
                        [
                            SNew(SNGonSkeletonTreeRowPrefix)
                            .Depth(Depth)
                            .AncestorHasMoreSiblings(AncestorHasMoreSiblings)
                            .bIsLastChild(bIsLastChild)
                            .bHasChildren(bHasChildren)
                            .bIsExpanded(bIsExpanded)
                            .bIsSocket(bIsSocket)
                            .bFilterActive(bHasFilter)
                            .bIsSelected(IsRowSelected())
                            .OnToggleExpand_Lambda([this, BoneIdx]()
                            {
                                FNGonSkeletonTreeUIState& State = GetSkeletonTreeUIState(this);
                                if (State.CollapsedBones.Contains(BoneIdx))
                                {
                                    State.CollapsedBones.Remove(BoneIdx);
                                }
                                else
                                {
                                    State.CollapsedBones.Add(BoneIdx);
                                }
                                RebuildSkeletonTreeList();
                            })
                        ]
                        + SHorizontalBox::Slot()
                        .FillWidth(1.0f)
                        .VAlign(VAlign_Center)
                        .Padding(3.0f, 2.0f, 4.0f, 2.0f)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(DisplayName))
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                            .ColorAndOpacity_Lambda([IsRowSelected, bIsSocket, bHasChildren]()
                            {
                                if (IsRowSelected())
                                {
                                    return FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);
                                }
                                if (bIsSocket)
                                {
                                    return FLinearColor(0.82f, 0.85f, 0.90f, 1.0f);
                                }
                                return bHasChildren
                                    ? FLinearColor(0.90f, 0.92f, 0.95f, 1.0f)
                                    : FLinearColor(0.76f, 0.79f, 0.84f, 1.0f);
                            })
                        ]
                    ]
                ]
            ];
    };

    if (bHasFilter)
    {
        for (const FNGonBoneEntry& Bone : SkeletonBones)
        {
            const FString BoneNameStr = Bone.Name.ToString();
            if (BoneNameStr.Contains(BoneSearchFilter, ESearchCase::IgnoreCase))
            {
                const bool bHasChildren = !ChildBones[Bone.BoneIndex].IsEmpty() || !BoneSockets[Bone.BoneIndex].IsEmpty();
                AddTreeRowWidget(BoneNameStr, Bone.BoneIndex, INDEX_NONE, 0, {}, true, bHasChildren, true);
            }
        }
        for (const FNGonSocketEntry& Socket : UIState.Sockets)
        {
            const FString SocketNameStr = Socket.SocketName.ToString();
            if (SocketNameStr.Contains(BoneSearchFilter, ESearchCase::IgnoreCase))
            {
                AddTreeRowWidget(SocketNameStr, Socket.ParentBoneIndex, Socket.SocketIndex, 0, {}, true, false, false);
            }
        }
        return;
    }

    TFunction<void(int32, int32, TArray<bool>&, bool)> TraverseBone;
    TraverseBone = [&](int32 BoneIdx, int32 Depth, TArray<bool>& AncestorLines, bool bIsLastChild)
    {
        const FNGonBoneEntry& Bone = SkeletonBones[BoneIdx];
        const TArray<int32>& AttachedSockets = BoneSockets[BoneIdx];
        const TArray<int32>& Children = ChildBones[BoneIdx];
        const int32 TotalChildren = AttachedSockets.Num() + Children.Num();
        const bool bHasChildren = TotalChildren > 0;
        const bool bIsExpanded = !UIState.CollapsedBones.Contains(BoneIdx);

        AddTreeRowWidget(
            Bone.Name.ToString(),
            BoneIdx,
            INDEX_NONE,
            Depth,
            AncestorLines,
            bIsLastChild,
            bHasChildren,
            bIsExpanded);

        if (!bHasChildren || !bIsExpanded)
        {
            return;
        }

        int32 EmittedChildIdx = 0;
        for (const int32 SocketIdx : AttachedSockets)
        {
            ++EmittedChildIdx;
            const bool bChildIsLast = (EmittedChildIdx == TotalChildren);
            TArray<bool> NextAncestorLines = AncestorLines;
            NextAncestorLines.Add(!bChildIsLast);
            const FNGonSocketEntry& Socket = UIState.Sockets[SocketIdx];
            AddTreeRowWidget(
                Socket.SocketName.ToString(),
                BoneIdx,
                SocketIdx,
                Depth + 1,
                NextAncestorLines,
                bChildIsLast,
                false,
                false);
        }

        for (const int32 ChildBoneIdx : Children)
        {
            ++EmittedChildIdx;
            const bool bChildIsLast = (EmittedChildIdx == TotalChildren);
            TArray<bool> NextAncestorLines = AncestorLines;
            NextAncestorLines.Add(!bChildIsLast);
            TraverseBone(ChildBoneIdx, Depth + 1, NextAncestorLines, bChildIsLast);
        }
    };

    for (int32 RootIdx = 0; RootIdx < RootBones.Num(); ++RootIdx)
    {
        TArray<bool> AncestorLines;
        const bool bRootIsLast = (RootIdx + 1 == RootBones.Num());
        TraverseBone(RootBones[RootIdx], 0, AncestorLines, bRootIsLast);
    }
}

TSharedRef<SWidget> SNGonPreview::MakeMorphTargetsTab()
{
    SAssignNew(MorphListBox, SVerticalBox);
    RebuildMorphTargetList();

    return SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(STextBlock)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            .Text_Lambda([this]
            {
                return FText::Format(LOCTEXT("MorphCountHeader", "Morph Targets ({0}) & Procedural Deformers"),
                    FText::AsNumber(MorphTargets.Num()));
            })
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(STextBlock).Text(LOCTEXT("BendDeformerLabel", "Procedural Bend:"))
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
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
            + SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("ResetBendBtn", "0.0"))
                .OnClicked_Lambda([this]
                {
                    Bend = 0.0f;
                    Viewport->Preview().SetBend(0.0f);
                    Viewport->Invalidate();
                    return FReply::Handled();
                })
            ]
        ]
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            [
                MorphListBox.ToSharedRef()
            ]
        ];
}

void SNGonPreview::RebuildMorphTargetList()
{
    if (!MorphListBox.IsValid())
    {
        return;
    }
    MorphListBox->ClearChildren();

    if (MorphTargets.IsEmpty())
    {
        MorphListBox->AddSlot()
            .AutoHeight()
            .Padding(4.0f, 4.0f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("NoMorphTargetsMsg", "No Morph Targets on active asset. Use the Procedural Bend slider or Soft Selection Sculpt tools."))
                .AutoWrapText(true)
                .ColorAndOpacity(FLinearColor(0.60f, 0.64f, 0.70f))
            ];
        return;
    }

    for (const FNGonMorphEntry& Morph : MorphTargets)
    {
        MorphListBox->AddSlot()
            .AutoHeight()
            .Padding(4.0f, 2.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromName(Morph.Name))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(STextBlock)
                    .Text(FText::Format(LOCTEXT("MorphVerts", "{0} verts"), FText::AsNumber(Morph.VertexDeltaCount)))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                    .ColorAndOpacity(FLinearColor(0.55f, 0.62f, 0.70f))
                ]
            ];
    }
}

TSharedRef<SWidget> SNGonPreview::MakeTopologyTab()
{
    return SNew(SScrollBox)
        + SScrollBox::Slot()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                .Text(LOCTEXT("TopoAuditTitle", "N-Gon Polygon Topology Audit"))
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([this]
                {
                    const FNGonTopology& Topo = Viewport->Preview().GetTopology();
                    int32 TriFaces = 0;
                    int32 QuadFaces = 0;
                    int32 NGonFaces = 0;
                    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
                    {
                        const int32 Sides = Pair.Value.Perimeter.Num();
                        if (Sides == 3) ++TriFaces;
                        else if (Sides == 4) ++QuadFaces;
                        else if (Sides > 4) ++NGonFaces;
                    }
                    return FText::Format(
                        LOCTEXT("TopoBreakdownText",
                            "Total Polygons: {0}\n"
                            "  - Quads (4 sides): {1}\n"
                            "  - Triangles (3 sides): {2}\n"
                            "  - N-Gons (5+ sides): {3} (Max {4} sides)\n"
                            "Render Triangles: {5}\n"
                            "Logical Vertices: {6}  |  Unique Edges: {7}"),
                        FText::AsNumber(Topo.Faces.Num()),
                        FText::AsNumber(QuadFaces),
                        FText::AsNumber(TriFaces),
                        FText::AsNumber(NGonFaces),
                        FText::AsNumber(Topo.MaxFaceCorners),
                        FText::AsNumber(Topo.TriangleCount),
                        FText::AsNumber(Topo.RestPositions.Num()),
                        FText::AsNumber(Topo.Edges.Num()));
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
            [
                SNew(SWrapBox)
                .UseAllottedSize(true)
                .InnerSlotPadding(FVector2D(6.0f, 4.0f))
                + SWrapBox::Slot()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("SelectNGonsBtn", "Select First N-Gon (5+ Sides)"))
                    .OnClicked_Lambda([this]
                    {
                        SetSelectionMode(ENGonElement::Face);
                        Viewport->Preview().ClearSelection();
                        for (const TPair<int32, FNGonFace>& Pair : Viewport->Preview().GetTopology().Faces)
                        {
                            if (Pair.Value.Perimeter.Num() > 4)
                            {
                                Viewport->Preview().SelectElement(ENGonElement::Face, Pair.Key, true);
                            }
                        }
                        Viewport->Invalidate();
                        return FReply::Handled();
                    })
                ]
                + SWrapBox::Slot()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("SelectTrisBtn", "Select Triangle Faces (3 Sides)"))
                    .OnClicked_Lambda([this]
                    {
                        SetSelectionMode(ENGonElement::Face);
                        Viewport->Preview().ClearSelection();
                        for (const TPair<int32, FNGonFace>& Pair : Viewport->Preview().GetTopology().Faces)
                        {
                            if (Pair.Value.Perimeter.Num() == 3)
                            {
                                Viewport->Preview().SelectElement(ENGonElement::Face, Pair.Key, true);
                            }
                        }
                        Viewport->Invalidate();
                        return FReply::Handled();
                    })
                ]
                + SWrapBox::Slot()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("ClearSelBtn", "Clear Selection"))
                    .OnClicked_Lambda([this]
                    {
                        Viewport->Preview().ClearSelection();
                        Viewport->Invalidate();
                        return FReply::Handled();
                    })
                ]
            ]
        ];
}

FReply SNGonPreview::SelectVerticesNearSelectedBone()
{
    if (!SkeletonBones.IsValidIndex(SelectedBoneIndex))
    {
        return FReply::Handled();
    }
    const FVector BonePos = SkeletonBones[SelectedBoneIndex].ComponentTransform.GetTranslation();
    const FNGonTopology& Topo = Viewport->Preview().GetTopology();
    if (Topo.RestPositions.IsEmpty())
    {
        return FReply::Handled();
    }

    SetSelectionMode(ENGonElement::Vertex);
    Viewport->Preview().ClearSelection();

    int32 ClosestVertex = 0;
    double BestDistSq = TNumericLimits<double>::Max();
    const double RadiusSq = FMath::Square(FMath::Max(static_cast<double>(SoftRadius) * 0.35, 10.0));
    int32 SelectedCount = 0;

    for (int32 v = 0; v < Topo.RestPositions.Num(); ++v)
    {
        const double DistSq = FVector::DistSquared(Topo.RestPositions[v], BonePos);
        if (DistSq < BestDistSq)
        {
            BestDistSq = DistSq;
            ClosestVertex = v;
        }
        if (DistSq <= RadiusSq)
        {
            Viewport->Preview().SelectElement(ENGonElement::Vertex, v, true);
            ++SelectedCount;
        }
    }

    if (SelectedCount == 0)
    {
        Viewport->Preview().SelectElement(ENGonElement::Vertex, ClosestVertex, false);
    }
    Viewport->Invalidate();
    return FReply::Handled();
}

TSharedRef<SWidget> SNGonPreview::MakeDetailsTab()
{
    return SNew(SVerticalBox)
        // 1. Selected Bone Inspector (visible when a bone is selected in Skeleton Tree)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            SNew(SBorder)
            .Visibility_Lambda([this]
            {
                return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? EVisibility::Visible : EVisibility::Collapsed;
            })
            .Padding(FMargin(6.0f))
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                    .ColorAndOpacity(FLinearColor(0.25f, 0.85f, 1.0f))
                    .Text(LOCTEXT("SelectedBoneHeader", "Selected Bone Transform (Ref Pose)"))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text_Lambda([this]
                    {
                        if (!SkeletonBones.IsValidIndex(SelectedBoneIndex))
                        {
                            return FText::GetEmpty();
                        }
                        const FNGonBoneEntry& Bone = SkeletonBones[SelectedBoneIndex];
                        const FString ParentName = SkeletonBones.IsValidIndex(Bone.ParentIndex)
                            ? SkeletonBones[Bone.ParentIndex].Name.ToString()
                            : TEXT("None (Root)");
                        const FVector LocalT = Bone.LocalTransform.GetTranslation();
                        const FRotator LocalR = Bone.LocalTransform.GetRotation().Rotator();
                        const FVector CompT = Bone.ComponentTransform.GetTranslation();
                        return FText::Format(
                            LOCTEXT("BoneDetailFmt",
                                "Bone: {0} (Index {1})\n"
                                "Parent: {2}  |  Children: {3}\n"
                                "Local Pos: ({4}, {5}, {6})\n"
                                "Local Rot: ({7}, {8}, {9})\n"
                                "Comp Pos: ({10}, {11}, {12})"),
                            FText::FromName(Bone.Name),
                            FText::AsNumber(Bone.BoneIndex),
                            FText::FromString(ParentName),
                            FText::AsNumber(Bone.ChildCount),
                            FText::AsNumber(FMath::RoundToFloat(LocalT.X * 100.0f) / 100.0f),
                            FText::AsNumber(FMath::RoundToFloat(LocalT.Y * 100.0f) / 100.0f),
                            FText::AsNumber(FMath::RoundToFloat(LocalT.Z * 100.0f) / 100.0f),
                            FText::AsNumber(FMath::RoundToFloat(LocalR.Pitch * 10.0f) / 10.0f),
                            FText::AsNumber(FMath::RoundToFloat(LocalR.Yaw * 10.0f) / 10.0f),
                            FText::AsNumber(FMath::RoundToFloat(LocalR.Roll * 10.0f) / 10.0f),
                            FText::AsNumber(FMath::RoundToFloat(CompT.X * 100.0f) / 100.0f),
                            FText::AsNumber(FMath::RoundToFloat(CompT.Y * 100.0f) / 10.0f),
                            FText::AsNumber(FMath::RoundToFloat(CompT.Z * 100.0f) / 100.0f));
                    })
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("SelectVertsNearBone", "Select Vertices Near Joint"))
                    .OnClicked(this, &SNGonPreview::SelectVerticesNearSelectedBone)
                ]
            ]
        ]
        // 2. Soft Selection & Sculpt Settings (Maya Soft Modification / Sculpt)
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(STextBlock)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            .Text(LOCTEXT("SoftSculptSectionTitle", "Soft Selection & Maya Sculpt Falloff"))
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 4.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return bSoftSelection ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bSoftSelection = (State == ECheckBoxState::Checked);
                ApplySoftSelectionSettings();
            })
            [
                SNew(STextBlock).Text(LOCTEXT("EnableSoftCheck", "Enable Soft Selection Falloff (B)"))
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 4.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(STextBlock).Text(LOCTEXT("FalloffRadiusLabel", "Falloff Radius:"))
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(SSlider)
                .MinValue(1.0f)
                .MaxValue(500.0f)
                .Value_Lambda([this] { return SoftRadius; })
                .OnValueChanged_Lambda([this](float Value)
                {
                    SoftRadius = Value;
                    ApplySoftSelectionSettings();
                })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text_Lambda([this] { return FText::AsNumber(FMath::RoundToInt(SoftRadius)); })
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 4.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(STextBlock).Text(LOCTEXT("DistanceMetricLabel", "Distance Metric:"))
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f)
            [
                SNew(SComboButton)
                .OnGetMenuContent_Lambda([this]
                {
                    FMenuBuilder Menu(true, nullptr);
                    const auto AddMetric = [this, &Menu](ENGonSoftMetric Metric, const TCHAR* Label)
                    {
                        Menu.AddMenuEntry(
                            FText::FromString(Label),
                            FText(),
                            FSlateIcon(),
                            FUIAction(FExecuteAction::CreateLambda([this, Metric]
                            {
                                SoftMetric = Metric;
                                ApplySoftSelectionSettings();
                            })));
                    };
                    AddMetric(ENGonSoftMetric::EdgeSurface, TEXT("Edge Surface (Geodesic)"));
                    AddMetric(ENGonSoftMetric::FaceSurface, TEXT("Face Surface (Polygon Centers)"));
                    AddMetric(ENGonSoftMetric::Volume, TEXT("Volume (Euclidean Sphere)"));
                    AddMetric(ENGonSoftMetric::Hybrid, TEXT("Hybrid (Surface + Volume Blend)"));
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]
                    {
                        switch (SoftMetric)
                        {
                        case ENGonSoftMetric::FaceSurface: return LOCTEXT("MetricFace", "Face Surface");
                        case ENGonSoftMetric::Volume: return LOCTEXT("MetricVolume", "Volume");
                        case ENGonSoftMetric::Hybrid: return LOCTEXT("MetricHybrid", "Hybrid");
                        default: return LOCTEXT("MetricEdge", "Edge Surface");
                        }
                    })
                ]
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 4.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(STextBlock).Text(LOCTEXT("ConnectivityMaskLabel", "Connectivity Mask:"))
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f)
            [
                SNew(SComboButton)
                .OnGetMenuContent_Lambda([this]
                {
                    FMenuBuilder Menu(true, nullptr);
                    const auto AddMask = [this, &Menu](ENGonSoftConnectivity Mask, const TCHAR* Label)
                    {
                        Menu.AddMenuEntry(
                            FText::FromString(Label),
                            FText(),
                            FSlateIcon(),
                            FUIAction(FExecuteAction::CreateLambda([this, Mask]
                            {
                                SoftConnectivity = Mask;
                                ApplySoftSelectionSettings();
                            })));
                    };
                    AddMask(ENGonSoftConnectivity::SameIsland, TEXT("Same Connected Island"));
                    AddMask(ENGonSoftConnectivity::Unrestricted, TEXT("Unrestricted (Global)"));
                    AddMask(ENGonSoftConnectivity::FloodWithinRadius, TEXT("Flood Within Radius"));
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]
                    {
                        switch (SoftConnectivity)
                        {
                        case ENGonSoftConnectivity::Unrestricted: return LOCTEXT("MaskNone", "Unrestricted");
                        case ENGonSoftConnectivity::FloodWithinRadius: return LOCTEXT("MaskFlood", "Flood Within Radius");
                        default: return LOCTEXT("MaskIsland", "Same Island");
                        }
                    })
                ]
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 4.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(STextBlock).Text(LOCTEXT("HybridBlendLabel", "Hybrid Surface Blend:"))
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(SSlider)
                .MinValue(0.0f)
                .MaxValue(1.0f)
                .IsEnabled_Lambda([this] { return bSoftSelection && SoftMetric == ENGonSoftMetric::Hybrid; })
                .Value_Lambda([this] { return SoftSurfaceBlend; })
                .OnValueChanged_Lambda([this](float Value)
                {
                    SoftSurfaceBlend = Value;
                    ApplySoftSelectionSettings();
                })
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 6.0f, 0.0f, 2.0f)
        [
            SNew(STextBlock)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            .Text(LOCTEXT("DeformSolverModeLabel", "Deformation / Sculpt Solver Mode:"))
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 2.0f)
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(8.0f, 4.0f))
            + SWrapBox::Slot()
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return SoftMode == ENGonSoftTransformMode::Fixed ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                {
                    if (State == ECheckBoxState::Checked)
                    {
                        SoftMode = ENGonSoftTransformMode::Fixed;
                        ApplySoftSelectionSettings();
                    }
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("FixedModeRadio", "Fixed Weights"))
                ]
            ]
            + SWrapBox::Slot()
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return SoftMode == ENGonSoftTransformMode::LiveFalloff ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                {
                    SoftMode = (State == ECheckBoxState::Checked) ? ENGonSoftTransformMode::LiveFalloff : ENGonSoftTransformMode::Fixed;
                    ApplySoftSelectionSettings();
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("LiveFalloffRadio", "Live Falloff"))
                ]
            ]
            + SWrapBox::Slot()
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return SoftMode == ENGonSoftTransformMode::SlideSculpt ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                {
                    SoftMode = (State == ECheckBoxState::Checked) ? ENGonSoftTransformMode::SlideSculpt : ENGonSoftTransformMode::Fixed;
                    ApplySoftSelectionSettings();
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("SlideSculptRadio", "Slide Sculpt"))
                ]
            ]
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeAssetDetailsTab()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(STextBlock)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            .Text(LOCTEXT("AssetMetadataHeader", "Active Asset & Scene Telemetry"))
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text_Lambda([this]
            {
                const FNGonTopology& Topo = Viewport->Preview().GetTopology();
                const FVector Min = Topo.RestBounds.IsValid ? Topo.RestBounds.Min : FVector::ZeroVector;
                const FVector Max = Topo.RestBounds.IsValid ? Topo.RestBounds.Max : FVector::ZeroVector;
                const FString ActivePath = !SkeletalMeshAssetPath.IsEmpty()
                    ? SkeletalMeshAssetPath
                    : (!StaticMeshAssetPath.IsEmpty() ? StaticMeshAssetPath : TEXT("(Local FBX / Procedural Demo)"));

                return FText::Format(
                    LOCTEXT("AssetTelemetryText",
                        "Source Name: {0}\n"
                        "Asset Type: {1}\n"
                        "Object Path: {2}\n\n"
                        "{3}\n"
                        "Bounds Min: ({4}, {5}, {6})\n"
                        "Bounds Max: ({7}, {8}, {9})\n"
                        "Reference Bones: {10}\n"
                        "Morph Targets: {11}\n\n"
                        "{12}"),
                    SourceLabel,
                    SourceTypeLabel,
                    FText::FromString(ActivePath),
                    Viewport->Preview().DescribeMesh(),
                    FText::AsNumber(FMath::RoundToInt(Min.X)),
                    FText::AsNumber(FMath::RoundToInt(Min.Y)),
                    FText::AsNumber(FMath::RoundToInt(Min.Z)),
                    FText::AsNumber(FMath::RoundToInt(Max.X)),
                    FText::AsNumber(FMath::RoundToInt(Max.Y)),
                    FText::AsNumber(FMath::RoundToInt(Max.Z)),
                    FText::AsNumber(SkeletonBones.Num()),
                    FText::AsNumber(MorphTargets.Num()),
                    Viewport->Preview().DescribeUpdates());
            })
        ];
}

TSharedRef<SWidget> SNGonPreview::MakePreviewSettingsTab()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(STextBlock)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            .Text(LOCTEXT("PreviewViewportSettingsHeader", "Viewport & Overlay Settings"))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return bShowEdges ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bShowEdges = (State == ECheckBoxState::Checked);
                Viewport->Preview().SetShowEdges(bShowEdges);
                Viewport->Invalidate();
            })
            [
                SNew(STextBlock).Text(LOCTEXT("PrefShowEdges", "Show Authored N-Gon Edges Overlay"))
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return bShowVertices ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bShowVertices = (State == ECheckBoxState::Checked);
                Viewport->Preview().SetShowVertices(bShowVertices);
                Viewport->Invalidate();
            })
            [
                SNew(STextBlock).Text(LOCTEXT("PrefShowVertices", "Show Logical Vertices Overlay"))
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return Viewport->IsGridVisible() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                Viewport->SetGridVisible(State == ECheckBoxState::Checked);
            })
            [
                SNew(STextBlock).Text(LOCTEXT("PrefShowGrid", "Show Viewport Ground Grid"))
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return bShowStatsOverlay ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bShowStatsOverlay = (State == ECheckBoxState::Checked);
            })
            [
                SNew(STextBlock).Text(LOCTEXT("PrefShowStatsHud", "Show Top-Left Mesh & Skeleton HUD Stats"))
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return bShowHotkeysOverlay ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bShowHotkeysOverlay = (State == ECheckBoxState::Checked);
            })
            [
                SNew(STextBlock).Text(LOCTEXT("PrefShowHotkeysHud", "Show Top-Right Hotkeys Legend"))
            ]
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
    if (!Viewport->Preview().HasSoftSelectionInfluence())
    {
        bShowHeatmap = false;
        Viewport->SetHeatmapVisible(false);
    }
    Viewport->Invalidate();
}

TSharedRef<SWidget> SNGonPreview::MakeStatus()
{
    return SNew(SBorder)
        .Padding(FMargin(8.0f, 4.0f))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]
                    {
                        return FText::Format(LOCTEXT("BottomStatusMain", "{0} ({1})  |  {2}"),
                            SourceLabel, SourceTypeLabel, Viewport->Preview().DescribeMesh());
                    })
                    .AutoWrapText(true)
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(12.0f, 0.0f, 0.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]
                    {
                        return Viewport->Preview().DescribeUpdates();
                    })
                    .ColorAndOpacity(FLinearColor(0.62f, 0.68f, 0.75f))
                ]
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
        StaticMeshAssetPath.Reset();
        SkeletalMeshAssetPath.Reset();
        SkeletonBones.Reset();
        MorphTargets.Reset();
        SelectedBoneIndex = INDEX_NONE;
        GetSkeletonTreeUIState(this) = FNGonSkeletonTreeUIState{};
        RebuildSkeletonTreeList();
        RebuildMorphTargetList();
        ShowMesh(MoveTemp(Data), LOCTEXT("DemoName", "Demo Mesh"), LOCTEXT("DemoType", "Procedural N-Gon"));
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
        StaticMeshAssetPath.Reset();
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
    StaticMeshAssetPath = Asset.GetSoftObjectPath().ToString();
    SkeletalMeshAssetPath.Reset();
    SkeletonBones.Reset();
    MorphTargets.Reset();
    SelectedBoneIndex = INDEX_NONE;
    RebuildSkeletonTreeList();
    RebuildMorphTargetList();

    ShowMesh(MoveTemp(Data), FText::FromName(Asset.AssetName), LOCTEXT("StaticMeshType", "Static Mesh"));
    if (bTriangleOnly)
    {
        ShowError(LOCTEXT("TrianglesOnly",
            "LOD 0 contains triangle faces only. Open the untriangulated source FBX to inspect the authored polygons."));
    }
}

void SNGonPreview::LoadSkeletalMesh(const FAssetData& Asset)
{
    if (!Asset.IsValid())
    {
        SkeletalMeshAssetPath.Reset();
        return;
    }

    USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset.GetAsset());
    if (!SkeletalMesh)
    {
        ShowError(LOCTEXT("InvalidSkelMesh", "Could not load the selected Skeletal Mesh asset."));
        return;
    }

    FMeshDescription SourceMeshDesc;
    bool bHasMeshDesc = SkeletalMesh->CloneMeshDescription(0, SourceMeshDesc);
    if (!bHasMeshDesc)
    {
        if (const FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel())
        {
            if (ImportedModel->LODModels.IsValidIndex(0))
            {
                ImportedModel->LODModels[0].GetMeshDescription(SkeletalMesh, 0, SourceMeshDesc);
                bHasMeshDesc = (SourceMeshDesc.Triangles().Num() > 0);
            }
        }
    }

    if (!bHasMeshDesc)
    {
        ShowError(LOCTEXT("NoSkelSourceLOD", "This Skeletal Mesh has no accessible LOD 0 MeshDescription or ImportedModel."));
        return;
    }

    FNGonMeshData Data;
    FText Error;
    if (!NGon::ConvertMeshDescription(SourceMeshDesc, Data, Error))
    {
        ShowError(Error);
        return;
    }

    // Extract Reference Skeleton hierarchy
    SkeletonBones.Reset();
    SelectedBoneIndex = INDEX_NONE;
    const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
    const int32 NumBones = RefSkeleton.GetNum();
    const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();
    SkeletonBones.Reserve(NumBones);

    for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
    {
        FNGonBoneEntry Entry;
        Entry.BoneIndex = BoneIdx;
        Entry.ParentIndex = RefSkeleton.GetParentIndex(BoneIdx);
        Entry.Name = RefSkeleton.GetBoneName(BoneIdx);
        Entry.LocalTransform = RefBonePose.IsValidIndex(BoneIdx) ? RefBonePose[BoneIdx] : FTransform::Identity;
        if (Entry.ParentIndex >= 0 && SkeletonBones.IsValidIndex(Entry.ParentIndex))
        {
            Entry.Depth = SkeletonBones[Entry.ParentIndex].Depth + 1;
            Entry.ComponentTransform = Entry.LocalTransform * SkeletonBones[Entry.ParentIndex].ComponentTransform;
            ++SkeletonBones[Entry.ParentIndex].ChildCount;
        }
        else
        {
            Entry.Depth = 0;
            Entry.ComponentTransform = Entry.LocalTransform;
        }
        SkeletonBones.Add(Entry);
    }
    if (!SkeletonBones.IsEmpty())
    {
        SelectedBoneIndex = 0;
    }

    // Extract Skeletal Mesh Sockets and attach them to their parent bones in the Skeleton Tree
    FNGonSkeletonTreeUIState& SkelUIState = GetSkeletonTreeUIState(this);
    SkelUIState.CollapsedBones.Reset();
    SkelUIState.Sockets.Reset();
    SkelUIState.SelectedSocketIndex = INDEX_NONE;
    const int32 NumMeshSockets = SkeletalMesh->NumSockets();
    for (int32 SocketIdx = 0; SocketIdx < NumMeshSockets; ++SocketIdx)
    {
        if (const USkeletalMeshSocket* Socket = SkeletalMesh->GetSocketByIndex(SocketIdx))
        {
            FNGonSocketEntry SocketEntry;
            SocketEntry.SocketIndex = SkelUIState.Sockets.Num();
            SocketEntry.SocketName = Socket->SocketName;
            SocketEntry.BoneName = Socket->BoneName;
            SocketEntry.ParentBoneIndex = RefSkeleton.FindBoneIndex(Socket->BoneName);
            SocketEntry.RelativeTransform = FTransform(Socket->RelativeRotation, Socket->RelativeLocation, Socket->RelativeScale);
            if (SkeletonBones.IsValidIndex(SocketEntry.ParentBoneIndex))
            {
                ++SkeletonBones[SocketEntry.ParentBoneIndex].ChildCount;
            }
            SkelUIState.Sockets.Add(SocketEntry);
        }
    }

    // Extract Morph Targets
    MorphTargets.Reset();
    for (const TObjectPtr<UMorphTarget>& Morph : SkeletalMesh->GetMorphTargets())
    {
        if (Morph)
        {
            FNGonMorphEntry MorphEntry;
            MorphEntry.Name = Morph->GetFName();
            MorphEntry.VertexDeltaCount = Morph->GetMorphLODModels().IsValidIndex(0)
                ? Morph->GetMorphLODModels()[0].Vertices.Num()
                : 0;
            MorphTargets.Add(MorphEntry);
        }
    }

    StaticMeshAssetPath.Reset();
    SkeletalMeshAssetPath = Asset.GetSoftObjectPath().ToString();
    ActiveUpperTab = ENGonRightUpperTab::SkeletonTree;
    RebuildSkeletonTreeList();
    RebuildMorphTargetList();

    ShowMesh(MoveTemp(Data), FText::FromName(Asset.AssetName), LOCTEXT("SkeletalMeshType", "Skeletal Mesh"));
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
    StaticMeshAssetPath.Reset();
    SkeletalMeshAssetPath.Reset();
    SkeletonBones.Reset();
    MorphTargets.Reset();
    SelectedBoneIndex = INDEX_NONE;
    RebuildSkeletonTreeList();
    RebuildMorphTargetList();

    ShowMesh(MoveTemp(Data), Name, LOCTEXT("FbxType", "FBX Polygon Mesh"));
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

void SNGonPreview::ShowMesh(FNGonMeshData&& Data, const FText& SourceName, const FText& InSourceType)
{
    SourceLabel = SourceName;
    SourceTypeLabel = InSourceType;
    ErrorText = FText::GetEmpty();
    bShowHeatmap = false;
    HeatBias = 0.0f;
    Bend = 0.0f;
    NGon::SetActiveViewportUVMesh(Data, SourceName, InSourceType);
    Viewport->SetMesh(MoveTemp(Data));
    Viewport->Preview().SetShowEdges(bShowEdges);
    Viewport->Preview().SetShowVertices(bShowVertices);
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
#pragma once

#include "CoreMinimal.h"
#include "Rendering/DrawElements.h"
#include "SNGonPreview.h"
#include "UnrealClient.h"
#include "Widgets/SLeafWidget.h"

namespace NGon
{
enum class ENGonTreeIconKind : uint8
{
    Bone,
    Socket,
    TabSkeleton,
    TabMorph,
    TabCurves,
    Plus,
    ToolSelect,
    ToolTranslate,
    ToolRotate,
    ToolScale,
    CoordSpace,
    MoreDots,
    CompVertex,
    CompEdge,
    CompFace,
    OcclusionCube,
    OverlayEdges,
    OverlayVerts,
    OverlayHeatmap,
    SnapMagnet,
    SnapSurface,
    SnapGrid,
    SnapRotate,
    SnapScale,
    ViewCamera,
    ViewCamSpeed,
    ViewLit,
    ViewEye,
    ViewSkull
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

    // Viewport <-> Preview bridge & Toolbar settings
    SNGonViewport* ViewportPtr = nullptr;
    TFunction<void(int32)> OnViewportBonePicked;
    TFunction<void()> OnViewportBoneTransformChanged;
    TFunction<void()> OnToggleEdgesShortcut;

    int32 BoneDrawMode = 2; // 0 = All Hierarchy, 1 = Selected & Subtree, 2 = Selected Only (UE5 default), 3 = Hidden
    bool bShowOnlyWeightedBones = false;
    bool bHideSocketsInTree = false;
    bool bShowBoneNames = true;
    bool bBackfaceSelect = true;
    float SurfaceSnapOffset = 0.0f;
    bool bSurfaceSnapEnabled = false;
    float CameraSpeedScalar = 1.0f;
    ELevelViewportType ViewportType = LVT_Perspective;
    int32 ShadingMode = 0; // 0 = Lit Smooth, 1 = Lit Flat, 2 = Unlit, 3 = Wireframe
};

FNGonSkeletonTreeUIState& GetSkeletonTreeUIState(const SNGonPreview* Preview);
FNGonSkeletonTreeUIState* FindUIStateByViewport(const SNGonViewport* InViewport);

void DrawBoneIconVector(
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FGeometry& AllottedGeometry,
    const FVector2D& Center,
    const FLinearColor& Color);

void DrawSocketIconVector(
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FGeometry& AllottedGeometry,
    const FVector2D& Center,
    const FLinearColor& Color);

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

    void Construct(const FArguments& InArgs);

protected:
    FVector2D ComputeDesiredSize(float) const override;
    int32 OnPaint(
        const FPaintArgs& Args,
        const FGeometry& AllottedGeometry,
        const FSlateRect& MyCullingRect,
        FSlateWindowElementList& OutDrawElements,
        int32 LayerId,
        const FWidgetStyle& InWidgetStyle,
        bool bParentEnabled) const override;

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
        , _bIsWeightedBone(true)
    {}
        SLATE_ARGUMENT(int32, Depth)
        SLATE_ARGUMENT(TArray<bool>, AncestorHasMoreSiblings)
        SLATE_ARGUMENT(bool, bIsLastChild)
        SLATE_ARGUMENT(bool, bHasChildren)
        SLATE_ARGUMENT(bool, bIsExpanded)
        SLATE_ARGUMENT(bool, bIsSocket)
        SLATE_ARGUMENT(bool, bFilterActive)
        SLATE_ARGUMENT(bool, bIsSelected)
        SLATE_ARGUMENT(bool, bIsWeightedBone)
        SLATE_EVENT(FSimpleDelegate, OnToggleExpand)
    SLATE_END_ARGS()

    static constexpr float StepX = 14.0f;
    static constexpr float IconSlotWidth = 20.0f;

    void Construct(const FArguments& InArgs);

protected:
    FVector2D ComputeDesiredSize(float) const override;
    int32 OnPaint(
        const FPaintArgs& Args,
        const FGeometry& AllottedGeometry,
        const FSlateRect& MyCullingRect,
        FSlateWindowElementList& OutDrawElements,
        int32 LayerId,
        const FWidgetStyle& InWidgetStyle,
        bool bParentEnabled) const override;
    FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
    int32 Depth = 0;
    TArray<bool> AncestorHasMoreSiblings;
    bool bIsLastChild = true;
    bool bHasChildren = false;
    bool bIsExpanded = true;
    bool bIsSocket = false;
    bool bFilterActive = false;
    bool bIsSelected = false;
    bool bIsWeightedBone = true;
    FSimpleDelegate OnToggleExpand;
};
} // namespace NGon

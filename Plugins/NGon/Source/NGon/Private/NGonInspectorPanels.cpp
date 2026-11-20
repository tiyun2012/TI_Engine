#include "SNGonPreview.h"
#include "NGonSceneSkeleton.h"
#include "NGonUIIcons.h"
#include "NGonUVEditor.h"

#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "PropertyCustomizationHelpers.h"
#include "NGonUndoTree.h"
#include "Materials/MaterialInterface.h"
#include "STiPaintEditor.h"
#include "SNGonUndoTreePanel.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace NGon
{
void UpdateActiveViewportUVMeshFromTopology(const FNGonTopology& Topology);
void RequestUVEditorSubMeshFilter(int32 SubMeshIndex);
}

using NGon::ENGonTreeIconKind;
using NGon::FNGonSocketEntry;
using NGon::FNGonSkeletonTreeUIState;
using NGon::GetSkeletonTreeUIState;
using NGon::FindUIStateByViewport;
using NGon::SNGonVectorIcon;
using NGon::SNGonSkeletonTreeRowPrefix;

#define LOCTEXT_NAMESPACE "NGonPreview"

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
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                    + SWrapBox::Slot()
                    [
                        MakeUpperTabButton(ENGonRightUpperTab::MeshStudio, LOCTEXT("MeshStudioTab", "Mesh Studio"))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeUpperTabButton(ENGonRightUpperTab::SkeletonTree, LOCTEXT("SkeletonTreeTab", "Skeleton"))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeUpperTabButton(ENGonRightUpperTab::MorphTargets, LOCTEXT("MorphTargetsTab", "Morphs"))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeUpperTabButton(ENGonRightUpperTab::Topology, LOCTEXT("TopologyTab", "Topology"))
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
                            return ActiveUpperTab == ENGonRightUpperTab::MeshStudio ? EVisibility::Visible : EVisibility::Collapsed;
                        })
                        [
                            MakeMeshStudioTab()
                        ]
                    ]
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
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                    + SWrapBox::Slot()
                    [
                        MakeLowerTabButton(ENGonRightLowerTab::AIConsole, LOCTEXT("AIConsoleTab", "AI Console"))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeLowerTabButton(ENGonRightLowerTab::Details, LOCTEXT("DetailsTab", "Details"))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeLowerTabButton(ENGonRightLowerTab::AssetDetails, LOCTEXT("AssetDetailsTab", "Asset"))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeLowerTabButton(ENGonRightLowerTab::PreviewSettings, LOCTEXT("PreviewSettingsTab", "Settings"))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeLowerTabButton(ENGonRightLowerTab::Painter, LOCTEXT("PainterTab", "Painter"))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeLowerTabButton(ENGonRightLowerTab::History, LOCTEXT("HistoryTab", "History"))
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
                                return ActiveLowerTab == ENGonRightLowerTab::AIConsole ? EVisibility::Visible : EVisibility::Collapsed;
                            })
                            [
                                MakeAIConsoleTab()
                            ]
                        ]
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
                        + SOverlay::Slot()
                        [
                            SNew(SBox)
                            .Visibility_Lambda([this]
                            {
                                return ActiveLowerTab == ENGonRightLowerTab::Painter ? EVisibility::Visible : EVisibility::Collapsed;
                            })
                            [
                                MakePainterTab()
                            ]
                        ]
                        + SOverlay::Slot()
                        [
                            SNew(SBox)
                            .Visibility_Lambda([this]
                            {
                                return ActiveLowerTab == ENGonRightLowerTab::History ? EVisibility::Visible : EVisibility::Collapsed;
                            })
                            [
                                MakeHistoryTab()
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
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(4.0f, 2.0f))
                .ToolTipText(LOCTEXT("SkelTreeSettingsTip", "Skeleton Tree Filter & Display Options"))
                .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                {
                    FMenuBuilder MenuBuilder(true, nullptr);
                    MenuBuilder.BeginSection("BoneFiltering", LOCTEXT("BoneFilterSection", "Skeleton Options"));
                    MenuBuilder.AddMenuEntry(
                        LOCTEXT("ShowWeightedOnlyLabel", "Hide Non-Weighted / IK Utility Bones"),
                        LOCTEXT("ShowWeightedOnlyTip", "Only display deforming weighted bones in the Skeleton Tree"),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                FNGonSkeletonTreeUIState& St = GetSkeletonTreeUIState(this);
                                St.bShowOnlyWeightedBones = !St.bShowOnlyWeightedBones;
                                RebuildSkeletonTreeList();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]()
                            {
                                return GetSkeletonTreeUIState(this).bShowOnlyWeightedBones;
                            })),
                        NAME_None,
                        EUserInterfaceActionType::ToggleButton);
                    MenuBuilder.AddMenuEntry(
                        LOCTEXT("HideSocketsLabel", "Hide Sockets"),
                        LOCTEXT("HideSocketsTip", "Toggle socket visibility in the Skeleton Tree"),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                FNGonSkeletonTreeUIState& St = GetSkeletonTreeUIState(this);
                                St.bHideSocketsInTree = !St.bHideSocketsInTree;
                                RebuildSkeletonTreeList();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]()
                            {
                                return GetSkeletonTreeUIState(this).bHideSocketsInTree;
                            })),
                        NAME_None,
                        EUserInterfaceActionType::ToggleButton);
                    MenuBuilder.EndSection();
                    return MenuBuilder.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SNGonVectorIcon)
                    .Kind(ENGonTreeIconKind::MoreDots)
                    .Color(FLinearColor(0.78f, 0.80f, 0.84f, 1.0f))
                ]
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
        const bool bIsWeightedBone = !bIsSocket && SkeletonBones.IsValidIndex(BoneIdx)
            && !NGon::IsUtilityNonDeformingBoneName(SkeletonBones[BoneIdx].Name);

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
                        return FLinearColor(0.05f, 0.32f, 0.68f, 1.0f);
                    }
                    return (RowNumber % 2 == 0)
                        ? FLinearColor(0.068f, 0.070f, 0.075f, 1.0f)
                        : FLinearColor(0.055f, 0.057f, 0.062f, 1.0f);
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
                        if (Viewport.IsValid())
                        {
                            Viewport->Preview().ClearSelection();
                            NGon::SelectSceneBone(&Viewport->Preview(), BoneIdx, true);
                            if (Viewport->GetGizmoMode() == ENGonGizmoMode::None)
                            {
                                Viewport->SetGizmoMode(ENGonGizmoMode::Translate);
                            }
                            Viewport->Invalidate();
                            Viewport->FocusViewport();
                        }
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
                            .bIsWeightedBone(bIsWeightedBone)
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
                        .Padding(3.0f, 1.5f, 4.0f, 1.5f)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(DisplayName))
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                            .ColorAndOpacity_Lambda([IsRowSelected, bIsSocket, bIsWeightedBone]()
                            {
                                if (IsRowSelected())
                                {
                                    return FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);
                                }
                                if (bIsSocket)
                                {
                                    return FLinearColor(0.80f, 0.82f, 0.86f, 1.0f);
                                }
                                return bIsWeightedBone
                                    ? FLinearColor(0.92f, 0.92f, 0.94f, 1.0f)
                                    : FLinearColor(0.46f, 0.47f, 0.50f, 1.0f);
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
        if (UIState.bShowOnlyWeightedBones && Depth > 0 && NGon::IsUtilityNonDeformingBoneName(Bone.Name))
        {
            return;
        }
        const TArray<int32> EmptySockets;
        const TArray<int32>& AttachedSockets = UIState.bHideSocketsInTree ? EmptySockets : BoneSockets[BoneIdx];
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
                .ColorAndOpacity(FLinearColor(0.32f, 0.88f, 1.0f))
                .Text(LOCTEXT("SubMeshOutlinerTitle", "FBX Mesh Hierarchy / Sub-Meshes"))
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(SBorder)
                .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                .BorderBackgroundColor(FLinearColor(0.06f, 0.068f, 0.085f, 1.0f))
                .Padding(FMargin(6.0f))
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        .Text_Lambda([this]
                        {
                            const TArray<FNGonSubMeshInfo>& Subs = Viewport->Preview().GetTopology().SubMeshes;
                            if (Subs.IsEmpty())
                            {
                                return LOCTEXT("NoSubMeshesText", "Single combined mesh.");
                            }
                            FString OutStr;
                            for (const FNGonSubMeshInfo& Sub : Subs)
                            {
                                if (Sub.ParentName != NAME_None)
                                {
                                    OutStr += FString::Printf(TEXT("%s / %s  (%d faces)\n"), *Sub.ParentName.ToString(), *Sub.Name.ToString(), Sub.FaceCount);
                                }
                                else
                                {
                                    OutStr += FString::Printf(TEXT("%s  (%d faces)\n"), *Sub.Name.ToString(), Sub.FaceCount);
                                }
                            }
                            return FText::FromString(OutStr.TrimEnd());
                        })
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
                    [
                        SNew(SComboButton)
                        .ContentPadding(FMargin(6.0f, 3.0f))
                        .ButtonContent()
                        [
                            SNew(STextBlock)
                            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                            .Text(LOCTEXT("SelectOrIsolateSubMeshBtn", "Select / Isolate Sub-Mesh in 3D & UV..."))
                        ]
                        .OnGetMenuContent_Lambda([this]()
                        {
                            FMenuBuilder Menu(true, nullptr);
                            const TArray<FNGonSubMeshInfo>& Subs = Viewport->Preview().GetTopology().SubMeshes;
                            Menu.BeginSection("SubMesh3D", LOCTEXT("SubMesh3DSec", "Select Sub-Mesh Faces in 3D"));
                            for (int32 i = 0; i < Subs.Num(); ++i)
                            {
                                const FNGonSubMeshInfo& Sub = Subs[i];
                                Menu.AddMenuEntry(
                                    FText::FromString(FString::Printf(TEXT("Select 3D: %s (%d faces)"), *Sub.Name.ToString(), Sub.FaceCount)),
                                    FText(),
                                    FSlateIcon(),
                                    FUIAction(FExecuteAction::CreateLambda([this, i]()
                                    {
                                        const FNGonTopology& Topo = Viewport->Preview().GetTopology();
                                        TSet<int32> SubFaces;
                                        for (const TPair<int32, int32>& Pair : Topo.FaceToSubMesh)
                                        {
                                            if (Pair.Value == i) SubFaces.Add(Pair.Key);
                                        }
                                        SetSelectionMode(ENGonElement::Face);
                                        Viewport->Preview().SetSelectionSet(ENGonElement::Face, SubFaces, false);
                                        Viewport->Invalidate();
                                    })));
                            }
                            Menu.EndSection();
                            Menu.BeginSection("SubMeshUV", LOCTEXT("SubMeshUVSec", "Isolate Sub-Mesh UDIM in UV Editor"));
                            for (int32 i = 0; i < Subs.Num(); ++i)
                            {
                                const FNGonSubMeshInfo& Sub = Subs[i];
                                Menu.AddMenuEntry(
                                    FText::FromString(FString::Printf(TEXT("Isolate UV: %s"), *Sub.Name.ToString())),
                                    FText(),
                                    FSlateIcon(),
                                    FUIAction(FExecuteAction::CreateLambda([this, i]()
                                    {
                                        FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("NGonUVEditor")));
                                        NGon::RequestUVEditorSubMeshFilter(i);
                                    })));
                            }
                            Menu.EndSection();
                            return Menu.MakeWidget();
                        })
                    ]
                ]
            ]
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

void SNGonPreview::RebuildMaterialSlotsList()
{
    if (!MaterialSlotsListBox.IsValid() || !Viewport.IsValid())
    {
        return;
    }
    MaterialSlotsListBox->ClearChildren();

    const TArray<FNGonMaterialSlotInfo> Slots = Viewport->Preview().GetMaterialSlots();
    if (Slots.IsEmpty())
    {
        return;
    }
    ActiveMaterialSlotIndex = FMath::Clamp(ActiveMaterialSlotIndex, 0, Slots.Num() - 1);

    auto PushMaterialUndo = [this](const FString& Label, FNGonMaterialSlotsSnapshot&& BeforeSnap)
    {
        if (!Viewport.IsValid() || !UndoSession.IsValid() || UndoSession->IsApplyingHistory()) return;
        TSharedPtr<FNGonMaterialSlotsSnapshot> BeforePtr = MakeShared<FNGonMaterialSlotsSnapshot>(MoveTemp(BeforeSnap));
        TSharedPtr<FNGonMaterialSlotsSnapshot> AfterPtr = MakeShared<FNGonMaterialSlotsSnapshot>(Viewport->Preview().CaptureMaterialSlotsSnapshot());
        TWeakPtr<SNGonViewport> WeakVP = Viewport;
        TWeakPtr<SNGonPreview> WeakSelf = StaticCastSharedRef<SNGonPreview>(AsShared());
        TSharedPtr<INGonUndoPayload> Payload = MakeShared<FNGonLambdaUndoPayload>(
            [WeakVP, WeakSelf, BeforePtr]() -> bool
            {
                if (TSharedPtr<SNGonViewport> VP = WeakVP.Pin())
                {
                    VP->Preview().RestoreMaterialSlotsSnapshot(*BeforePtr);
                    VP->Invalidate();
                    if (TSharedPtr<SNGonPreview> Self = WeakSelf.Pin()) Self->RebuildMaterialSlotsList();
                    return true;
                }
                return false;
            },
            [WeakVP, WeakSelf, AfterPtr]() -> bool
            {
                if (TSharedPtr<SNGonViewport> VP = WeakVP.Pin())
                {
                    VP->Preview().RestoreMaterialSlotsSnapshot(*AfterPtr);
                    VP->Invalidate();
                    if (TSharedPtr<SNGonPreview> Self = WeakSelf.Pin()) Self->RebuildMaterialSlotsList();
                    return true;
                }
                return false;
            },
            static_cast<SIZE_T>(BeforePtr->TriangleMaterialIDs.Num() * sizeof(int32) * 2 + 256));
        UndoSession->PushEdit(ENGonUndoContextId::Mesh, Label, ENGonUndoEditKind::Value, MoveTemp(Payload));
    };

    const bool bCanRemove = Slots.Num() > 1;
    for (int32 SlotIdx = 0; SlotIdx < Slots.Num(); ++SlotIdx)
    {
        const FNGonMaterialSlotInfo& Info = Slots[SlotIdx];
        const FString CachedPath = Info.AssetPath;
        const FLinearColor SwatchCol = Info.PreviewColor;

        MaterialSlotsListBox->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 2.0f)
            [
                SNew(SBorder)
                .Padding(FMargin(5.0f, 4.0f))
                .BorderBackgroundColor_Lambda([this, SlotIdx]()
                {
                    return (ActiveMaterialSlotIndex == SlotIdx)
                        ? FLinearColor(0.14f, 0.20f, 0.28f, 1.0f)
                        : FLinearColor(0.10f, 0.11f, 0.14f, 1.0f);
                })
                [
                    SNew(SVerticalBox)
                    // Row 1: Swatch + [idx] Name (N tris) + Assign + Select + Remove
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 5.0f, 0.0f)
                        [
                            SNew(SBox).WidthOverride(10.0f).HeightOverride(10.0f)
                            [
                                SNew(SBorder)
                                .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                                .BorderBackgroundColor(SwatchCol)
                            ]
                        ]
                        + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                        [
                            SNew(STextBlock)
                            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                            .ColorAndOpacity(FLinearColor(0.88f, 0.91f, 0.96f))
                            .Text(FText::FromString(FString::Printf(TEXT("[%d] %s (%d tris)"), SlotIdx, *Info.Name, Info.TriangleCount)))
                        ]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
                        [
                            SNew(SButton)
                            .ContentPadding(FMargin(5.0f, 1.0f))
                            .ToolTipText(LOCTEXT("AssignSlotTip", "Assign currently selected faces to this material slot"))
                            .OnClicked_Lambda([this, SlotIdx, PushMaterialUndo]()
                            {
                                if (!Viewport.IsValid()) return FReply::Handled();
                                ActiveMaterialSlotIndex = SlotIdx;
                                FNGonMaterialSlotsSnapshot Before = Viewport->Preview().CaptureMaterialSlotsSnapshot();
                                if (Viewport->Preview().AssignSelectedFacesToMaterialSlot(SlotIdx))
                                {
                                    PushMaterialUndo(FString::Printf(TEXT("Assign Faces to Slot %d"), SlotIdx), MoveTemp(Before));
                                    RebuildMaterialSlotsList();
                                    Viewport->Invalidate();
                                }
                                return FReply::Handled();
                            })
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("AssignSlotBtn", "Assign"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                            ]
                        ]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
                        [
                            SNew(SButton)
                            .ContentPadding(FMargin(5.0f, 1.0f))
                            .ToolTipText(LOCTEXT("SelectSlotTip", "Select all faces assigned to this material slot"))
                            .OnClicked_Lambda([this, SlotIdx]()
                            {
                                if (!Viewport.IsValid()) return FReply::Handled();
                                ActiveMaterialSlotIndex = SlotIdx;
                                Viewport->Preview().SelectFacesByMaterialSlot(SlotIdx, false);
                                Viewport->Invalidate();
                                return FReply::Handled();
                            })
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("SelectSlotBtn", "Select"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                            ]
                        ]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
                        [
                            SNew(SButton)
                            .ContentPadding(FMargin(5.0f, 1.0f))
                            .IsEnabled(bCanRemove)
                            .ToolTipText(LOCTEXT("RemoveSlotTip", "Remove this material slot and safely remap its faces"))
                            .OnClicked_Lambda([this, SlotIdx, PushMaterialUndo]()
                            {
                                if (!Viewport.IsValid()) return FReply::Handled();
                                FNGonMaterialSlotsSnapshot Before = Viewport->Preview().CaptureMaterialSlotsSnapshot();
                                if (Viewport->Preview().RemoveMaterialSlot(SlotIdx))
                                {
                                    PushMaterialUndo(FString::Printf(TEXT("Remove Material Slot %d"), SlotIdx), MoveTemp(Before));
                                    RebuildMaterialSlotsList();
                                    Viewport->Invalidate();
                                }
                                return FReply::Handled();
                            })
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("RemoveSlotBtn", "×"))
                                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                                .ColorAndOpacity(bCanRemove ? FLinearColor(0.95f, 0.45f, 0.45f) : FLinearColor(0.45f, 0.45f, 0.48f))
                            ]
                        ]
                    ]
                    // Row 2: UE Material Asset Picker (Content Browser drop/browse/clear)
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                    [
                        SNew(SObjectPropertyEntryBox)
                        .AllowedClass(UMaterialInterface::StaticClass())
                        .ObjectPath_Lambda([CachedPath]() { return CachedPath; })
                        .OnObjectChanged_Lambda([this, SlotIdx, PushMaterialUndo](const FAssetData& AssetData)
                        {
                            if (!Viewport.IsValid()) return;
                            ActiveMaterialSlotIndex = SlotIdx;
                            UMaterialInterface* NewMat = Cast<UMaterialInterface>(AssetData.GetAsset());
                            FNGonMaterialSlotsSnapshot Before = Viewport->Preview().CaptureMaterialSlotsSnapshot();
                            if (Viewport->Preview().SetMaterialSlotAsset(SlotIdx, NewMat))
                            {
                                PushMaterialUndo(
                                    NewMat ? FString::Printf(TEXT("Set Slot %d Material: %s"), SlotIdx, *NewMat->GetName())
                                           : FString::Printf(TEXT("Reset Slot %d to Studio"), SlotIdx),
                                    MoveTemp(Before));
                                RebuildMaterialSlotsList();
                                Viewport->Invalidate();
                            }
                        })
                        .DisplayThumbnail(true)
                        .DisplayUseSelected(true)
                        .AllowClear(true)
                        .AllowCreate(false)
                    ]
                ]
            ];
    }
}

TSharedRef<SWidget> SNGonPreview::MakeDetailsTab()
{
    auto MakeBoneAxisSpinBox = [this](
        const FText& AxisLabel,
        const FLinearColor& AxisColor,
        float MinVal,
        float MaxVal,
        TFunction<float()> Getter,
        TFunction<void(float)> Setter) -> TSharedRef<SWidget>
    {
        return SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 3.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(AxisLabel)
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                .ColorAndOpacity(AxisColor)
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(SSpinBox<float>)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                .MinValue(MinVal)
                .MaxValue(MaxVal)
                .Delta(0.25f)
                .Value_Lambda([Getter]() { return Getter(); })
                .OnValueChanged_Lambda([this, Setter](float NewVal)
                {
                    Setter(NewVal);
                })
            ];
    };

    auto ApplyEditedBoneLocalTransform = [this](const FTransform& NewLocal)
    {
        if (!SkeletonBones.IsValidIndex(SelectedBoneIndex) || !Viewport.IsValid())
        {
            return;
        }
        NGon::SetSceneBoneLocalTransform(&Viewport->Preview(), SelectedBoneIndex, NewLocal);
        for (int32 i = 0; i < SkeletonBones.Num(); ++i)
        {
            FTransform CurLocal, CurComp, RefLocal, RefComp;
            if (NGon::GetSceneBoneTransforms(&Viewport->Preview(), i, CurLocal, CurComp, RefLocal, RefComp))
            {
                SkeletonBones[i].LocalTransform = CurLocal;
                SkeletonBones[i].ComponentTransform = CurComp;
            }
        }
        Viewport->Invalidate();
    };

    auto PushMaterialUndo = [this](const FString& Label, FNGonMaterialSlotsSnapshot&& BeforeSnap)
    {
        if (!Viewport.IsValid() || !UndoSession.IsValid() || UndoSession->IsApplyingHistory()) return;
        TSharedPtr<FNGonMaterialSlotsSnapshot> BeforePtr = MakeShared<FNGonMaterialSlotsSnapshot>(MoveTemp(BeforeSnap));
        TSharedPtr<FNGonMaterialSlotsSnapshot> AfterPtr = MakeShared<FNGonMaterialSlotsSnapshot>(Viewport->Preview().CaptureMaterialSlotsSnapshot());
        TWeakPtr<SNGonViewport> WeakVP = Viewport;
        TWeakPtr<SNGonPreview> WeakSelf = StaticCastSharedRef<SNGonPreview>(AsShared());
        TSharedPtr<INGonUndoPayload> Payload = MakeShared<FNGonLambdaUndoPayload>(
            [WeakVP, WeakSelf, BeforePtr]() -> bool
            {
                if (TSharedPtr<SNGonViewport> VP = WeakVP.Pin())
                {
                    VP->Preview().RestoreMaterialSlotsSnapshot(*BeforePtr);
                    VP->Invalidate();
                    if (TSharedPtr<SNGonPreview> Self = WeakSelf.Pin()) Self->RebuildMaterialSlotsList();
                    return true;
                }
                return false;
            },
            [WeakVP, WeakSelf, AfterPtr]() -> bool
            {
                if (TSharedPtr<SNGonViewport> VP = WeakVP.Pin())
                {
                    VP->Preview().RestoreMaterialSlotsSnapshot(*AfterPtr);
                    VP->Invalidate();
                    if (TSharedPtr<SNGonPreview> Self = WeakSelf.Pin()) Self->RebuildMaterialSlotsList();
                    return true;
                }
                return false;
            },
            static_cast<SIZE_T>(BeforePtr->TriangleMaterialIDs.Num() * sizeof(int32) * 2 + 256));
        UndoSession->PushEdit(ENGonUndoContextId::Mesh, Label, ENGonUndoEditKind::Value, MoveTemp(Payload));
    };

    TSharedRef<SWidget> MaterialSlotsCard = SNew(SBorder)
        .Padding(FMargin(6.0f))
        .BorderBackgroundColor(FLinearColor(0.08f, 0.09f, 0.11f, 1.0f))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBorder)
                .Padding(FMargin(4.0f, 3.0f))
                .BorderBackgroundColor(FLinearColor(0.14f, 0.15f, 0.18f, 1.0f))
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                        .ColorAndOpacity(FLinearColor(0.88f, 0.90f, 0.94f))
                        .Text_Lambda([this]()
                        {
                            const int32 Cnt = Viewport.IsValid() ? Viewport->Preview().GetMaterialSlotCount() : 0;
                            return FText::FromString(FString::Printf(TEXT("▼  Material Slots (%d)"), Cnt));
                        })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
                    [
                        SNew(SButton)
                        .ContentPadding(FMargin(6.0f, 2.0f))
                        .ToolTipText(LOCTEXT("AddMatSlotTip", "Add a new Material Slot to this mesh"))
                        .OnClicked_Lambda([this, PushMaterialUndo]()
                        {
                            if (!Viewport.IsValid()) return FReply::Handled();
                            FNGonMaterialSlotsSnapshot Before = Viewport->Preview().CaptureMaterialSlotsSnapshot();
                            const int32 NewIdx = Viewport->Preview().AddMaterialSlot();
                            ActiveMaterialSlotIndex = NewIdx;
                            PushMaterialUndo(FString::Printf(TEXT("Add Material Slot %d"), NewIdx), MoveTemp(Before));
                            RebuildMaterialSlotsList();
                            Viewport->Invalidate();
                            return FReply::Handled();
                        })
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("AddMatSlotBtn", "+ Add Slot"))
                            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                        ]
                    ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
            [
                SAssignNew(MaterialSlotsListBox, SVerticalBox)
            ]
        ];

    RebuildMaterialSlotsList();

    return SNew(SVerticalBox)
        // 0. Material Slots Manager (always visible)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 8.0f)
        [
            MaterialSlotsCard
        ]
        // 1. Selected Bone & Transforms Inspector (matching UE 5.8 SkeletalMesh Editor Details panel in image_3b0306.jpg / image_3b02c7.jpg / image_3b001c.jpg)
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
            .BorderBackgroundColor(FLinearColor(0.08f, 0.09f, 0.11f, 1.0f))
            [
                SNew(SVerticalBox)
                // Category: Bone
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SBorder)
                    .Padding(FMargin(4.0f, 3.0f))
                    .BorderBackgroundColor(FLinearColor(0.14f, 0.15f, 0.18f, 1.0f))
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                        .ColorAndOpacity(FLinearColor(0.88f, 0.90f, 0.94f))
                        .Text(LOCTEXT("CatBoneHeader", "▼  Bone"))
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(4.0f, 4.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 12.0f, 0.0f)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("BoneNameFieldLbl", "Bone Name"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                        .ColorAndOpacity(FLinearColor(0.72f, 0.75f, 0.80f))
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                        .ColorAndOpacity(FLinearColor(0.25f, 0.88f, 1.0f))
                        .Text_Lambda([this]()
                        {
                            if (!SkeletonBones.IsValidIndex(SelectedBoneIndex))
                            {
                                return FText::GetEmpty();
                            }
                            const FNGonBoneEntry& B = SkeletonBones[SelectedBoneIndex];
                            const FString ParentStr = SkeletonBones.IsValidIndex(B.ParentIndex)
                                ? SkeletonBones[B.ParentIndex].Name.ToString()
                                : TEXT("Root");
                            return FText::FromString(FString::Printf(TEXT("%s   (#%d, Parent: %s)"), *B.Name.ToString(), B.BoneIndex, *ParentStr));
                        })
                    ]
                ]

                // Category: Transforms
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)
                [
                    SNew(SBorder)
                    .Padding(FMargin(4.0f, 3.0f))
                    .BorderBackgroundColor(FLinearColor(0.14f, 0.15f, 0.18f, 1.0f))
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                        .ColorAndOpacity(FLinearColor(0.88f, 0.90f, 0.94f))
                        .Text(LOCTEXT("CatTransformsHeader", "▼  Transforms (Local Joint Pose)"))
                    ]
                ]
                // Location (X, Y, Z)
                + SVerticalBox::Slot().AutoHeight().Padding(2.0f, 2.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
                    [
                        SNew(SBox).WidthOverride(56.0f)
                        [
                            SNew(STextBlock).Text(LOCTEXT("BoneLocLbl", "Location")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        ]
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                    [
                        MakeBoneAxisSpinBox(LOCTEXT("AxisX", "X"), FLinearColor(1.0f, 0.35f, 0.35f), -5000.0f, 5000.0f,
                            [this]() { return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? static_cast<float>(SkeletonBones[SelectedBoneIndex].LocalTransform.GetLocation().X) : 0.0f; },
                            [this, ApplyEditedBoneLocalTransform](float Val)
                            {
                                if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
                                {
                                    FTransform T = SkeletonBones[SelectedBoneIndex].LocalTransform;
                                    FVector L = T.GetLocation(); L.X = Val; T.SetLocation(L);
                                    ApplyEditedBoneLocalTransform(T);
                                }
                            })
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                    [
                        MakeBoneAxisSpinBox(LOCTEXT("AxisY", "Y"), FLinearColor(0.35f, 1.0f, 0.40f), -5000.0f, 5000.0f,
                            [this]() { return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? static_cast<float>(SkeletonBones[SelectedBoneIndex].LocalTransform.GetLocation().Y) : 0.0f; },
                            [this, ApplyEditedBoneLocalTransform](float Val)
                            {
                                if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
                                {
                                    FTransform T = SkeletonBones[SelectedBoneIndex].LocalTransform;
                                    FVector L = T.GetLocation(); L.Y = Val; T.SetLocation(L);
                                    ApplyEditedBoneLocalTransform(T);
                                }
                            })
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                    [
                        MakeBoneAxisSpinBox(LOCTEXT("AxisZ", "Z"), FLinearColor(0.40f, 0.65f, 1.0f), -5000.0f, 5000.0f,
                            [this]() { return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? static_cast<float>(SkeletonBones[SelectedBoneIndex].LocalTransform.GetLocation().Z) : 0.0f; },
                            [this, ApplyEditedBoneLocalTransform](float Val)
                            {
                                if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
                                {
                                    FTransform T = SkeletonBones[SelectedBoneIndex].LocalTransform;
                                    FVector L = T.GetLocation(); L.Z = Val; T.SetLocation(L);
                                    ApplyEditedBoneLocalTransform(T);
                                }
                            })
                    ]
                ]
                // Rotation (Roll X, Pitch Y, Yaw Z)
                + SVerticalBox::Slot().AutoHeight().Padding(2.0f, 2.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
                    [
                        SNew(SBox).WidthOverride(56.0f)
                        [
                            SNew(STextBlock).Text(LOCTEXT("BoneRotLbl", "Rotation")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        ]
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                    [
                        MakeBoneAxisSpinBox(LOCTEXT("AxisRoll", "X"), FLinearColor(1.0f, 0.35f, 0.35f), -360.0f, 360.0f,
                            [this]() { return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? static_cast<float>(SkeletonBones[SelectedBoneIndex].LocalTransform.GetRotation().Rotator().Roll) : 0.0f; },
                            [this, ApplyEditedBoneLocalTransform](float Val)
                            {
                                if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
                                {
                                    FTransform T = SkeletonBones[SelectedBoneIndex].LocalTransform;
                                    FRotator R = T.GetRotation().Rotator(); R.Roll = Val; T.SetRotation(FQuat(R));
                                    ApplyEditedBoneLocalTransform(T);
                                }
                            })
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                    [
                        MakeBoneAxisSpinBox(LOCTEXT("AxisPitch", "Y"), FLinearColor(0.35f, 1.0f, 0.40f), -360.0f, 360.0f,
                            [this]() { return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? static_cast<float>(SkeletonBones[SelectedBoneIndex].LocalTransform.GetRotation().Rotator().Pitch) : 0.0f; },
                            [this, ApplyEditedBoneLocalTransform](float Val)
                            {
                                if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
                                {
                                    FTransform T = SkeletonBones[SelectedBoneIndex].LocalTransform;
                                    FRotator R = T.GetRotation().Rotator(); R.Pitch = Val; T.SetRotation(FQuat(R));
                                    ApplyEditedBoneLocalTransform(T);
                                }
                            })
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                    [
                        MakeBoneAxisSpinBox(LOCTEXT("AxisYaw", "Z"), FLinearColor(0.40f, 0.65f, 1.0f), -360.0f, 360.0f,
                            [this]() { return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? static_cast<float>(SkeletonBones[SelectedBoneIndex].LocalTransform.GetRotation().Rotator().Yaw) : 0.0f; },
                            [this, ApplyEditedBoneLocalTransform](float Val)
                            {
                                if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
                                {
                                    FTransform T = SkeletonBones[SelectedBoneIndex].LocalTransform;
                                    FRotator R = T.GetRotation().Rotator(); R.Yaw = Val; T.SetRotation(FQuat(R));
                                    ApplyEditedBoneLocalTransform(T);
                                }
                            })
                    ]
                ]
                // Scale (X, Y, Z)
                + SVerticalBox::Slot().AutoHeight().Padding(2.0f, 2.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
                    [
                        SNew(SBox).WidthOverride(56.0f)
                        [
                            SNew(STextBlock).Text(LOCTEXT("BoneScaleLbl", "Scale")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        ]
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                    [
                        MakeBoneAxisSpinBox(LOCTEXT("AxisSX", "X"), FLinearColor(1.0f, 0.35f, 0.35f), 0.05f, 20.0f,
                            [this]() { return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? static_cast<float>(SkeletonBones[SelectedBoneIndex].LocalTransform.GetScale3D().X) : 1.0f; },
                            [this, ApplyEditedBoneLocalTransform](float Val)
                            {
                                if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
                                {
                                    FTransform T = SkeletonBones[SelectedBoneIndex].LocalTransform;
                                    FVector S = T.GetScale3D(); S.X = FMath::Max(0.05f, Val); T.SetScale3D(S);
                                    ApplyEditedBoneLocalTransform(T);
                                }
                            })
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                    [
                        MakeBoneAxisSpinBox(LOCTEXT("AxisSY", "Y"), FLinearColor(0.35f, 1.0f, 0.40f), 0.05f, 20.0f,
                            [this]() { return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? static_cast<float>(SkeletonBones[SelectedBoneIndex].LocalTransform.GetScale3D().Y) : 1.0f; },
                            [this, ApplyEditedBoneLocalTransform](float Val)
                            {
                                if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
                                {
                                    FTransform T = SkeletonBones[SelectedBoneIndex].LocalTransform;
                                    FVector S = T.GetScale3D(); S.Y = FMath::Max(0.05f, Val); T.SetScale3D(S);
                                    ApplyEditedBoneLocalTransform(T);
                                }
                            })
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2.0f, 0.0f)
                    [
                        MakeBoneAxisSpinBox(LOCTEXT("AxisSZ", "Z"), FLinearColor(0.40f, 0.65f, 1.0f), 0.05f, 20.0f,
                            [this]() { return SkeletonBones.IsValidIndex(SelectedBoneIndex) ? static_cast<float>(SkeletonBones[SelectedBoneIndex].LocalTransform.GetScale3D().Z) : 1.0f; },
                            [this, ApplyEditedBoneLocalTransform](float Val)
                            {
                                if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
                                {
                                    FTransform T = SkeletonBones[SelectedBoneIndex].LocalTransform;
                                    FVector S = T.GetScale3D(); S.Z = FMath::Max(0.05f, Val); T.SetScale3D(S);
                                    ApplyEditedBoneLocalTransform(T);
                                }
                            })
                    ]
                ]
                // Action Row: [Reset Bone (Ctrl+I)] [Reset All (Ctrl+Shift+I)] [Select Verts Near Joint]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
                [
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.0f, 3.0f))
                    + SWrapBox::Slot()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("BtnResetSelBone", "Reset Bone (Ctrl+I)"))
                        .ToolTipText(LOCTEXT("BtnResetSelBoneTip", "Reset the selected bone back to Reference Pose"))
                        .OnClicked_Lambda([this]()
                        {
                            if (NGon::ResetSelectedSceneBoneTransform(&Viewport->Preview()))
                            {
                                if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Viewport.Get()))
                                {
                                    if (UIState->OnViewportBoneTransformChanged)
                                    {
                                        UIState->OnViewportBoneTransformChanged();
                                    }
                                }
                                Viewport->Invalidate();
                            }
                            return FReply::Handled();
                        })
                    ]
                    + SWrapBox::Slot()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("BtnResetAllBones", "Reset All Bones (Ctrl+Shift+I)"))
                        .ToolTipText(LOCTEXT("BtnResetAllBonesTip", "Reset all skeleton bones back to Reference Pose"))
                        .OnClicked_Lambda([this]()
                        {
                            if (NGon::ResetAllSceneBonesTransforms(&Viewport->Preview()))
                            {
                                if (FNGonSkeletonTreeUIState* UIState = FindUIStateByViewport(Viewport.Get()))
                                {
                                    if (UIState->OnViewportBoneTransformChanged)
                                    {
                                        UIState->OnViewportBoneTransformChanged();
                                    }
                                }
                                Viewport->Invalidate();
                            }
                            return FReply::Handled();
                        })
                    ]
                    + SWrapBox::Slot()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("SelectVertsNearBone", "Select Vertices Near Joint"))
                        .OnClicked(this, &SNGonPreview::SelectVerticesNearSelectedBone)
                    ]
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
            .IsChecked_Lambda([this]
            {
                const bool bLive = Viewport.IsValid() ? Viewport->Preview().GetSoftSelectionSettings().bEnabled : bSoftSelection;
                return bLive ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                if (Viewport.IsValid())
                {
                    const FNGonSoftSelectionSettings Live = Viewport->Preview().GetSoftSelectionSettings();
                    if (Live.Radius > 0.1)
                    {
                        SoftRadius = static_cast<float>(Live.Radius);
                    }
                }
                bSoftSelection = (State == ECheckBoxState::Checked);
                ApplySoftSelectionSettings();
            })
            [
                SNew(STextBlock).Text(LOCTEXT("EnableSoftCheck", "Enable Soft Selection Falloff (B)"))
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(12.0f, 2.0f, 0.0f, 2.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this]
            {
                return (Viewport.IsValid() && Viewport->IsHeatmapVisible())
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bShowHeatmap = (State == ECheckBoxState::Checked);
                if (Viewport.IsValid())
                {
                    Viewport->SetHeatmapVisible(bShowHeatmap);
                }
            })
            [
                SNew(STextBlock).Text(LOCTEXT("SoftFaceHeatmapCheck", "Show Face Heatmap (H)"))
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(12.0f, 2.0f, 0.0f, 4.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this]
            {
                return (Viewport.IsValid() && NGon::IsShowingSoftVertexWeights(&Viewport->Preview()))
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                if (Viewport.IsValid())
                {
                    NGon::SetShowSoftVertexWeights(&Viewport->Preview(), State == ECheckBoxState::Checked);
                    Viewport->Invalidate();
                }
            })
            [
                SNew(STextBlock).Text(LOCTEXT("SoftVertexWeightsCheck", "Show Vertex Weight Colors"))
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
                .Value_Lambda([this]
                {
                    const float LiveR = Viewport.IsValid()
                        ? static_cast<float>(Viewport->Preview().GetSoftSelectionSettings().Radius)
                        : SoftRadius;
                    return FMath::Clamp(LiveR, 1.0f, 500.0f);
                })
                .OnValueChanged_Lambda([this](float Value)
                {
                    if (Viewport.IsValid())
                    {
                        bSoftSelection = Viewport->Preview().GetSoftSelectionSettings().bEnabled;
                    }
                    SoftRadius = Value;
                    ApplySoftSelectionSettings();
                })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text_Lambda([this]
                {
                    const float LiveR = Viewport.IsValid()
                        ? static_cast<float>(Viewport->Preview().GetSoftSelectionSettings().Radius)
                        : SoftRadius;
                    return FText::AsNumber(FMath::RoundToInt(LiveR));
                })
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
                .IsEnabled_Lambda([this] { return (Viewport.IsValid() ? Viewport->Preview().GetSoftSelectionSettings().bEnabled : bSoftSelection) && SoftMetric == ENGonSoftMetric::Hybrid; })
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
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 3.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this]
            {
                return GetSkeletonTreeUIState(this).bBackfaceSelect ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                GetSkeletonTreeUIState(this).bBackfaceSelect = (State == ECheckBoxState::Checked);
            })
            [
                SNew(STextBlock).Text(LOCTEXT("PrefBackfaceFilter", "Camera / Ray Backface Selection Filter"))
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 4.0f, 0.0f, 2.0f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this]
            {
                return (Viewport.IsValid() && NGon::IsXRayEnabled(&Viewport->Preview()))
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                if (Viewport.IsValid())
                {
                    NGon::SetXRayEnabled(&Viewport->Preview(), State == ECheckBoxState::Checked);
                    Viewport->Invalidate();
                }
            })
            [
                SNew(STextBlock).Text(LOCTEXT("XRayModeCheck", "X-Ray Mode (Half-Transparent Face & Through-Mesh Pick)"))
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(12.0f, 2.0f, 0.0f, 4.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(STextBlock).Text(LOCTEXT("XRayOpacityLabel", "X-Ray Opacity (0 - 1):"))
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(SSlider)
                .MinValue(0.0f)
                .MaxValue(1.0f)
                .Value_Lambda([this]
                {
                    return Viewport.IsValid() ? NGon::GetXRayOpacity(&Viewport->Preview()) : 0.35f;
                })
                .OnValueChanged_Lambda([this](float Value)
                {
                    if (Viewport.IsValid())
                    {
                        NGon::SetXRayOpacity(&Viewport->Preview(), Value);
                        Viewport->Invalidate();
                    }
                })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text_Lambda([this]
                {
                    const float Op = Viewport.IsValid() ? NGon::GetXRayOpacity(&Viewport->Preview()) : 0.35f;
                    return FText::FromString(FString::Printf(TEXT("%.2f"), Op));
                })
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

TSharedRef<SWidget> SNGonPreview::MakeHistoryTab()
{
    if (UndoTreePanel.IsValid())
    {
        return SNew(SBox)
            .MinDesiredHeight(380.0f)
            [
                UndoTreePanel.ToSharedRef()
            ];
    }
    return SNew(STextBlock).Text(LOCTEXT("NoHistoryPanel", "Undo History panel unavailable."));
}

TSharedRef<SWidget> SNGonPreview::MakePainterTab()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)
        [
            SNew(STextBlock)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            .Text(LOCTEXT("PainterHeader", "TiGUI Painter — Active N-Gon UV Mesh"))
        ]
        + SVerticalBox::Slot().FillHeight(1.0f)
        [
            Painter.ToSharedRef()
        ];
}



#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2

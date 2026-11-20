#include "SNGonPreview.h"
#include "NGonSceneSkeleton.h"
#include "NGonUIIcons.h"

#include "AssetRegistry/AssetData.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "PropertyCustomizationHelpers.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

using NGon::ENGonTreeIconKind;
using NGon::FNGonSocketEntry;
using NGon::FNGonSkeletonTreeUIState;
using NGon::GetSkeletonTreeUIState;
using NGon::FindUIStateByViewport;
using NGon::SNGonVectorIcon;
using NGon::SNGonSkeletonTreeRowPrefix;

#define LOCTEXT_NAMESPACE "NGonPreview"

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
                    LOCTEXT("ModelTip", "N-Gon Mesh Studio: primitives, extrude, inset, bevel, subdivide, edge/vertex ops."))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeToolButton(
                    ENGonToolShelfTab::AIStudio,
                    LOCTEXT("AIStudioIcon", "AI API"),
                    LOCTEXT("AIStudioLabel", "AI API"),
                    LOCTEXT("AIStudioTip", "Open Mesh Studio & AI JSON Command Console for programmatic mesh control."))
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
        ActiveUpperTab = ENGonRightUpperTab::MeshStudio;
        ActiveLowerTab = ENGonRightLowerTab::Details;
    }
    else if (Tab == ENGonToolShelfTab::AIStudio)
    {
        ActiveUpperTab = ENGonRightUpperTab::MeshStudio;
        ActiveLowerTab = ENGonRightLowerTab::AIConsole;
    }
    else if (Tab == ENGonToolShelfTab::Display)
    {
        ActiveLowerTab = ENGonRightLowerTab::PreviewSettings;
    }
}

TSharedRef<SWidget> SNGonPreview::MakeViewportOverlayHeader()
{
    auto MakeVerticalDivider = []() -> TSharedRef<SWidget>
    {
        return SNew(SBox)
            .WidthOverride(1.0f)
            .HeightOverride(18.0f)
            .Padding(FMargin(3.0f, 2.0f))
            [
                SNew(SBorder)
                .BorderBackgroundColor(FLinearColor(0.24f, 0.26f, 0.30f, 0.85f))
            ];
    };

    auto MakeIconToolButton = [this](
        ENGonTreeIconKind IconKind,
        const FText& Tooltip,
        TFunction<bool()> IsActiveFn,
        TFunction<void()> OnClickFn) -> TSharedRef<SWidget>
    {
        return SNew(SBorder)
            .Padding(FMargin(1.0f))
            .BorderBackgroundColor_Lambda([IsActiveFn]()
            {
                return IsActiveFn()
                    ? FLinearColor(0.04f, 0.44f, 0.92f, 0.95f)
                    : FLinearColor(0.11f, 0.12f, 0.14f, 0.85f);
            })
            [
                SNew(SCheckBox)
                .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
                .Padding(FMargin(5.0f, 3.0f))
                .ToolTipText(Tooltip)
                .IsChecked_Lambda([IsActiveFn]()
                {
                    return IsActiveFn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([this, OnClickFn](ECheckBoxState)
                {
                    OnClickFn();
                    Viewport->FocusViewport();
                })
                [
                    SNew(SNGonVectorIcon)
                    .Kind(IconKind)
                    .Color(FLinearColor(0.92f, 0.95f, 1.0f, 1.0f))
                ]
            ];
    };

    return SNew(SBorder)
        .Padding(FMargin(6.0f, 3.0f))
        .BorderBackgroundColor(FLinearColor(0.055f, 0.06f, 0.07f, 0.96f))
        [
            SNew(SHorizontalBox)

            // 1. Target Mode Dropdown (Bone Joint vs Mesh Component)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 3.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(4.0f, 2.0f))
                .ToolTipText(LOCTEXT("TargetModeComboTip", "Switch Gizmo Target between Skeleton Bone Joint and Mesh Components"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    Menu.BeginSection("GizmoTargetMode", LOCTEXT("GizmoTargetSec", "Gizmo Interaction Target"));
                    Menu.AddMenuEntry(
                        LOCTEXT("TargetBoneJointItem", "Skeleton Bone / Joint Mode (4)"),
                        LOCTEXT("TargetBoneJointTip", "Attach transform gizmo to the selected bone in the Skeleton Tree or 3D viewport"),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                NGon::SetBoneGizmoActive(&Viewport->Preview(), true);
                                Viewport->Invalidate();
                            }),
                            FCanExecuteAction::CreateLambda([this]() { return !SkeletonBones.IsEmpty(); }),
                            FIsActionChecked::CreateLambda([this]() { return NGon::IsBoneGizmoActive(&Viewport->Preview()); })),
                        NAME_None,
                        EUserInterfaceActionType::RadioButton);
                    Menu.AddMenuEntry(
                        LOCTEXT("TargetMeshCompItem", "Mesh Component Mode (1/2/3)"),
                        LOCTEXT("TargetMeshCompTip", "Attach transform gizmo to selected N-Gon vertices, edges, or polygon faces"),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                NGon::SetBoneGizmoActive(&Viewport->Preview(), false);
                                Viewport->Invalidate();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]() { return !NGon::IsBoneGizmoActive(&Viewport->Preview()); })),
                        NAME_None,
                        EUserInterfaceActionType::RadioButton);
                    Menu.EndSection();
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SNGonVectorIcon)
                    .Kind(ENGonTreeIconKind::OcclusionCube)
                    .Color(FLinearColor(0.85f, 0.88f, 0.94f, 1.0f))
                ]
            ]

            // 2. Transform Tool Strip: [Select (Q)] [Translate (W)] [Rotate (E)] [Scale (R)]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::ToolSelect,
                    LOCTEXT("SelectToolTip", "Select Objects / Components without Gizmo (Q)"),
                    [this]() { return Viewport->GetGizmoMode() == ENGonGizmoMode::None; },
                    [this]() { Viewport->SetGizmoMode(ENGonGizmoMode::None); })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::ToolTranslate,
                    LOCTEXT("TranslateToolTip", "Select and Translate Joint or Component (W)"),
                    [this]() { return Viewport->GetGizmoMode() == ENGonGizmoMode::Translate; },
                    [this]() { Viewport->SetGizmoMode(ENGonGizmoMode::Translate); })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::ToolRotate,
                    LOCTEXT("RotateToolTip", "Select and Rotate Joint or Component (E)"),
                    [this]() { return Viewport->GetGizmoMode() == ENGonGizmoMode::Rotate; },
                    [this]() { Viewport->SetGizmoMode(ENGonGizmoMode::Rotate); })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::ToolScale,
                    LOCTEXT("ScaleToolTip", "Select and Scale Joint or Component (R)"),
                    [this]() { return Viewport->GetGizmoMode() == ENGonGizmoMode::Scale; },
                    [this]() { Viewport->SetGizmoMode(ENGonGizmoMode::Scale); })
            ]

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                MakeVerticalDivider()
            ]

            // 3. Coordinate Space Switcher (Local / World) + More Gizmo Options (⋮)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::CoordSpace,
                    LOCTEXT("CoordSpaceTip", "Toggle Coordinate System between Local Space and World Space"),
                    [this]() { return NGon::IsSceneWorldCoordinateSystem(&Viewport->Preview()); },
                    [this]()
                    {
                        const bool bWorld = !NGon::IsSceneWorldCoordinateSystem(&Viewport->Preview());
                        NGon::SetSceneCoordinateSystem(&Viewport->Preview(), bWorld);
                        Viewport->Invalidate();
                    })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                SNew(SComboButton)
                .HasDownArrow(false)
                .ContentPadding(FMargin(3.0f, 3.0f))
                .ToolTipText(LOCTEXT("GizmoMoreMenuTip", "Gizmo Coordinate System, Pivot Policy & Bone Reset Actions"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    Menu.BeginSection("CoordSys", LOCTEXT("CoordSysSec", "Coordinate System"));
                    Menu.AddMenuEntry(
                        LOCTEXT("LocalCoordEntry", "Local Coordinate System"),
                        LOCTEXT("LocalCoordTip", "Align gizmo axes with the selected bone or component normal frame"),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                NGon::SetSceneCoordinateSystem(&Viewport->Preview(), false);
                                Viewport->Invalidate();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]() { return !NGon::IsSceneWorldCoordinateSystem(&Viewport->Preview()); })),
                        NAME_None,
                        EUserInterfaceActionType::RadioButton);
                    Menu.AddMenuEntry(
                        LOCTEXT("WorldCoordEntry", "World Coordinate System"),
                        LOCTEXT("WorldCoordTip", "Align gizmo axes with world X/Y/Z axes"),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                NGon::SetSceneCoordinateSystem(&Viewport->Preview(), true);
                                Viewport->Invalidate();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]() { return NGon::IsSceneWorldCoordinateSystem(&Viewport->Preview()); })),
                        NAME_None,
                        EUserInterfaceActionType::RadioButton);
                    Menu.EndSection();

                    Menu.BeginSection("GizmoModes", LOCTEXT("GizmoModesSec", "Gizmo Handle Mode & Pivot"));
                    Menu.AddMenuEntry(
                        LOCTEXT("CombinedGizmoEntry", "Combined Multi-Axis Gizmo"),
                        LOCTEXT("CombinedGizmoTip", "Show Translate, Rotate, and Scale handles simultaneously"),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]() { Viewport->SetGizmoMode(ENGonGizmoMode::Combined); }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]() { return Viewport->GetGizmoMode() == ENGonGizmoMode::Combined; })),
                        NAME_None,
                        EUserInterfaceActionType::Check);
                    Menu.AddMenuEntry(
                        LOCTEXT("ActivePivotEntry", "Component Pivot: Active Element"),
                        LOCTEXT("ActivePivotTip", "Center component gizmo on the most recently selected element instead of average"),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                const bool bActive = Viewport->Preview().GetGizmoFramePolicy() == ENGonGizmoFramePolicy::ActiveElement;
                                Viewport->Preview().SetGizmoFramePolicy(
                                    bActive ? ENGonGizmoFramePolicy::WeightedAverage : ENGonGizmoFramePolicy::ActiveElement);
                                Viewport->Invalidate();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]()
                            {
                                return Viewport->Preview().GetGizmoFramePolicy() == ENGonGizmoFramePolicy::ActiveElement;
                            })),
                        NAME_None,
                        EUserInterfaceActionType::Check);
                    Menu.EndSection();

                    Menu.BeginSection("BoneResets", LOCTEXT("BoneResetsSec", "Skeleton Bone Reset"));
                    Menu.AddMenuEntry(
                        LOCTEXT("ResetSelBoneMenu", "Reset Selected Bone Transform (Ctrl+I)"),
                        LOCTEXT("ResetSelBoneMenuTip", "Restore the selected bone to its Reference Pose transform"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]()
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
                        })));
                    Menu.AddMenuEntry(
                        LOCTEXT("ResetAllBonesMenu", "Reset All Bones Transforms (Ctrl+Shift+I)"),
                        LOCTEXT("ResetAllBonesMenuTip", "Restore all skeleton bones to Reference Pose"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]()
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
                        })));
                    Menu.EndSection();
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SNGonVectorIcon)
                    .Kind(ENGonTreeIconKind::MoreDots)
                    .Color(FLinearColor(0.80f, 0.84f, 0.90f, 1.0f))
                ]
            ]

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                MakeVerticalDivider()
            ]

            // 4. Bone / Joint Mode & Occlusion Filter
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::Bone,
                    LOCTEXT("BoneJointModeBtnTip", "Bone / Joint Selection & Gizmo Mode (4)"),
                    [this]() { return NGon::IsBoneGizmoActive(&Viewport->Preview()); },
                    [this]()
                    {
                        const bool bNewBoneMode = !NGon::IsBoneGizmoActive(&Viewport->Preview());
                        NGon::SetBoneGizmoActive(&Viewport->Preview(), bNewBoneMode);
                        Viewport->Invalidate();
                    })
            ]


            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                MakeVerticalDivider()
            ]

            // 5. Component Mode Strip: independent toggles [Vertex (1)] [Edge (2)] [Face (3)]
            //    Combine any subset (e.g. Face + Vertex) or disable all 3 to freeze selection
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::CompVertex,
                    LOCTEXT("CompVertexTip", "Toggle Vertex Selection (1) — combine with Edge/Face or turn all off to freeze selection"),
                    [this]() { return !NGon::IsBoneGizmoActive(&Viewport->Preview()) && Viewport->IsSelectionFlagEnabled(ENGonSelectionMask::Vertex); },
                    [this]()
                    {
                        ToggleSelectionMode(ENGonElement::Vertex);
                    })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::CompEdge,
                    LOCTEXT("CompEdgeTip", "Toggle Edge Selection (2) — combine with Vertex/Face or turn all off to freeze selection"),
                    [this]() { return !NGon::IsBoneGizmoActive(&Viewport->Preview()) && Viewport->IsSelectionFlagEnabled(ENGonSelectionMask::Edge); },
                    [this]()
                    {
                        ToggleSelectionMode(ENGonElement::Edge);
                    })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::CompFace,
                    LOCTEXT("CompFaceTip", "Toggle Face Selection (3) — combine with Vertex/Edge or turn all off to freeze selection"),
                    [this]() { return !NGon::IsBoneGizmoActive(&Viewport->Preview()) && Viewport->IsSelectionFlagEnabled(ENGonSelectionMask::Face); },
                    [this]()
                    {
                        ToggleSelectionMode(ENGonElement::Face);
                    })
            ]

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                MakeVerticalDivider()
            ]

            // 6. Topology & Heatmap Overlay Toggles: [Edges (Alt+9)] [Vertices] [Heatmap (H)]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::OverlayEdges,
                    LOCTEXT("OverlayEdgesBtnTip", "Toggle Authored N-Gon Edges Wireframe Overlay (Alt+9)"),
                    [this]() { return bShowEdges; },
                    [this]()
                    {
                        bShowEdges = !bShowEdges;
                        Viewport->Preview().SetShowEdges(bShowEdges);
                        Viewport->Invalidate();
                    })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::OverlayVerts,
                    LOCTEXT("OverlayVertsBtnTip", "Toggle Logical Vertices Point Overlay"),
                    [this]() { return Viewport.IsValid() ? NGon::GetSceneAccel(&Viewport->Preview()).bShowVertices : bShowVertices; },
                    [this]()
                    {
                        const bool bCurrent = Viewport.IsValid() ? NGon::GetSceneAccel(&Viewport->Preview()).bShowVertices : bShowVertices;
                        bShowVertices = !bCurrent;
                        Viewport->Preview().SetShowVertices(bShowVertices);
                        Viewport->Invalidate();
                    })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                MakeIconToolButton(
                    ENGonTreeIconKind::OverlayHeatmap,
                    LOCTEXT("OverlayHeatmapBtnTip", "Toggle Soft Selection Influence Heatmap (H)"),
                    [this]() { return Viewport->IsHeatmapVisible(); },
                    [this]()
                    {
                        bShowHeatmap = !Viewport->IsHeatmapVisible();
                        Viewport->SetHeatmapVisible(bShowHeatmap);
                    })
            ]

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                MakeVerticalDivider()
            ]

            // 7. Snapping Group (matching image_3aff81.png: Magnet menu, Surface snap, Grid snap, Rotation snap, Scale snap)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(4.0f, 2.0f))
                .ToolTipText(LOCTEXT("SnapMagnetMenuTip", "Snapping Presets & Options"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    Menu.BeginSection("SnapAll", LOCTEXT("SnapAllSec", "Transform Snapping"));
                    Menu.AddMenuEntry(
                        LOCTEXT("EnableAllSnaps", "Enable All Transform Snaps (Grid, Rot, Scale)"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            NGon::SetSceneSnapSettings(&Viewport->Preview(), true, G, true, R, true, S);
                        })));
                    Menu.AddMenuEntry(
                        LOCTEXT("DisableAllSnaps", "Disable All Transform Snaps"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            NGon::SetSceneSnapSettings(&Viewport->Preview(), false, G, false, R, false, S);
                        })));
                    Menu.EndSection();
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SNGonVectorIcon)
                    .Kind(ENGonTreeIconKind::SnapMagnet)
                    .Color(FLinearColor(0.82f, 0.85f, 0.90f, 1.0f))
                ]
            ]
            // Surface Snap Offset ("0 v")
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(4.0f, 2.0f))
                .ToolTipText(LOCTEXT("SurfaceSnapTip", "Surface Snap Offset"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    for (const float Val : { 0.0f, 1.0f, 5.0f, 10.0f, 25.0f })
                    {
                        Menu.AddMenuEntry(
                            FText::AsNumber(Val),
                            FText(),
                            FSlateIcon(),
                            FUIAction(FExecuteAction::CreateLambda([this, Val]()
                            {
                                GetSkeletonTreeUIState(this).SurfaceSnapOffset = Val;
                            })));
                    }
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 3.0f, 0.0f)
                    [
                        SNew(SNGonVectorIcon)
                        .Kind(ENGonTreeIconKind::SnapSurface)
                        .Color(FLinearColor(0.76f, 0.80f, 0.86f, 1.0f))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        .Text_Lambda([this]() { return FText::AsNumber(GetSkeletonTreeUIState(this).SurfaceSnapOffset); })
                    ]
                ]
            ]
            // Grid Snap Toggle + Value ("10 v")
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    MakeIconToolButton(
                        ENGonTreeIconKind::SnapGrid,
                        LOCTEXT("GridSnapToggleTip", "Toggle Position Grid Snapping"),
                        [this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            return bG;
                        },
                        [this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            NGon::SetSceneSnapSettings(&Viewport->Preview(), !bG, G, bR, R, bS, S);
                        })
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SComboButton)
                    .ContentPadding(FMargin(3.0f, 2.0f))
                    .ToolTipText(LOCTEXT("GridSnapValueTip", "Select Position Grid Snap Increment"))
                    .OnGetMenuContent_Lambda([this]()
                    {
                        FMenuBuilder Menu(true, nullptr);
                        for (const float Step : { 1.0f, 5.0f, 10.0f, 25.0f, 50.0f, 100.0f })
                        {
                            Menu.AddMenuEntry(
                                FText::AsNumber(Step),
                                FText(),
                                FSlateIcon(),
                                FUIAction(FExecuteAction::CreateLambda([this, Step]()
                                {
                                    bool bG, bR, bS; float G, R, S;
                                    NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                                    NGon::SetSceneSnapSettings(&Viewport->Preview(), true, Step, bR, R, bS, S);
                                })));
                        }
                        return Menu.MakeWidget();
                    })
                    .ButtonContent()
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        .Text_Lambda([this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            return FText::AsNumber(FMath::RoundToInt(G));
                        })
                    ]
                ]
            ]
            // Rotation Snap Toggle + Value ("10° v")
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    MakeIconToolButton(
                        ENGonTreeIconKind::SnapRotate,
                        LOCTEXT("RotSnapToggleTip", "Toggle Rotation Angle Snapping"),
                        [this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            return bR;
                        },
                        [this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            NGon::SetSceneSnapSettings(&Viewport->Preview(), bG, G, !bR, R, bS, S);
                        })
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SComboButton)
                    .ContentPadding(FMargin(3.0f, 2.0f))
                    .ToolTipText(LOCTEXT("RotSnapValueTip", "Select Rotation Snap Increment (Degrees)"))
                    .OnGetMenuContent_Lambda([this]()
                    {
                        FMenuBuilder Menu(true, nullptr);
                        for (const float Step : { 5.0f, 10.0f, 15.0f, 30.0f, 45.0f, 90.0f })
                        {
                            Menu.AddMenuEntry(
                                FText::Format(LOCTEXT("DegFmt", "{0}°"), FText::AsNumber(FMath::RoundToInt(Step))),
                                FText(),
                                FSlateIcon(),
                                FUIAction(FExecuteAction::CreateLambda([this, Step]()
                                {
                                    bool bG, bR, bS; float G, R, S;
                                    NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                                    NGon::SetSceneSnapSettings(&Viewport->Preview(), bG, G, true, Step, bS, S);
                                })));
                        }
                        return Menu.MakeWidget();
                    })
                    .ButtonContent()
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        .Text_Lambda([this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            return FText::Format(LOCTEXT("DegBtnFmt", "{0}°"), FText::AsNumber(FMath::RoundToInt(R)));
                        })
                    ]
                ]
            ]
            // Scale Snap Toggle + Value ("0.25 v")
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    MakeIconToolButton(
                        ENGonTreeIconKind::SnapScale,
                        LOCTEXT("ScaleSnapToggleTip", "Toggle Scale Snapping"),
                        [this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            return bS;
                        },
                        [this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            NGon::SetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, !bS, S);
                        })
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SComboButton)
                    .ContentPadding(FMargin(3.0f, 2.0f))
                    .ToolTipText(LOCTEXT("ScaleSnapValueTip", "Select Scale Snap Increment"))
                    .OnGetMenuContent_Lambda([this]()
                    {
                        FMenuBuilder Menu(true, nullptr);
                        for (const float Step : { 0.05f, 0.1f, 0.25f, 0.5f, 1.0f })
                        {
                            Menu.AddMenuEntry(
                                FText::AsNumber(Step),
                                FText(),
                                FSlateIcon(),
                                FUIAction(FExecuteAction::CreateLambda([this, Step]()
                                {
                                    bool bG, bR, bS; float G, R, S;
                                    NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                                    NGon::SetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, true, Step);
                                })));
                        }
                        return Menu.MakeWidget();
                    })
                    .ButtonContent()
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        .Text_Lambda([this]()
                        {
                            bool bG, bR, bS; float G, R, S;
                            NGon::GetSceneSnapSettings(&Viewport->Preview(), bG, G, bR, R, bS, S);
                            return FText::AsNumber(S);
                        })
                    ]
                ]
            ]

            // Flexible spacer pushing View controls to the right (matching image_3aff81.png!)
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SNew(SBox)
            ]

            // 8. Right-Aligned Viewport Controls: [Perspective v] [Cam Speed v] [Lit v] [Eye v] [Skull v] [LOD Auto v]
            // Camera Projection Dropdown ("Perspective v")
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(5.0f, 2.0f))
                .ToolTipText(LOCTEXT("PerspectiveMenuTip", "Switch Viewport Camera Projection (Perspective / Orthographic)"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    auto AddProj = [this, &Menu](ELevelViewportType Type, const FText& Label)
                    {
                        Menu.AddMenuEntry(
                            Label,
                            FText(),
                            FSlateIcon(),
                            FUIAction(
                                FExecuteAction::CreateLambda([this, Type]()
                                {
                                    GetSkeletonTreeUIState(this).ViewportType = Type;
                                    if (TSharedPtr<FEditorViewportClient> Client = Viewport->GetViewportClient())
                                    {
                                        Client->SetViewportType(Type);
                                        Client->Invalidate();
                                    }
                                }),
                                FCanExecuteAction(),
                                FIsActionChecked::CreateLambda([this, Type]()
                                {
                                    return GetSkeletonTreeUIState(this).ViewportType == Type;
                                })),
                            NAME_None,
                            EUserInterfaceActionType::RadioButton);
                    };
                    AddProj(LVT_Perspective, LOCTEXT("ProjPersp", "Perspective"));
                    AddProj(LVT_OrthoXY, LOCTEXT("ProjTop", "Top"));
                    AddProj(LVT_OrthoNegativeXY, LOCTEXT("ProjBottom", "Bottom"));
                    AddProj(LVT_OrthoXZ, LOCTEXT("ProjLeft", "Left"));
                    AddProj(LVT_OrthoNegativeXZ, LOCTEXT("ProjRight", "Right"));
                    AddProj(LVT_OrthoYZ, LOCTEXT("ProjFront", "Front"));
                    AddProj(LVT_OrthoNegativeYZ, LOCTEXT("ProjBack", "Back"));
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
                    [
                        SNew(SNGonVectorIcon)
                        .Kind(ENGonTreeIconKind::ViewCamera)
                        .Color(FLinearColor(0.82f, 0.85f, 0.90f, 1.0f))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        .Text_Lambda([this]()
                        {
                            switch (GetSkeletonTreeUIState(this).ViewportType)
                            {
                            case LVT_OrthoXY: return LOCTEXT("LblTop", "Top");
                            case LVT_OrthoNegativeXY: return LOCTEXT("LblBot", "Bottom");
                            case LVT_OrthoXZ: return LOCTEXT("LblLeft", "Left");
                            case LVT_OrthoNegativeXZ: return LOCTEXT("LblRight", "Right");
                            case LVT_OrthoYZ: return LOCTEXT("LblFront", "Front");
                            case LVT_OrthoNegativeYZ: return LOCTEXT("LblBack", "Back");
                            default: return LOCTEXT("LblPersp", "Perspective");
                            }
                        })
                    ]
                ]
            ]

            // Camera Speed Dropdown ("1 v")
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(4.0f, 2.0f))
                .ToolTipText(LOCTEXT("CamSpeedMenuTip", "Camera Movement Speed Multiplier"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    for (const float Spd : { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f })
                    {
                        Menu.AddMenuEntry(
                            FText::Format(LOCTEXT("CamSpdFmt", "{0}x"), FText::AsNumber(Spd)),
                            FText(),
                            FSlateIcon(),
                            FUIAction(FExecuteAction::CreateLambda([this, Spd]()
                            {
                                GetSkeletonTreeUIState(this).CameraSpeedScalar = Spd;
                                if (TSharedPtr<FEditorViewportClient> Client = Viewport->GetViewportClient())
                                {
                                    FEditorViewportCameraSpeedSettings SpeedSettings = Client->GetCameraSpeedSettings();
                                    SpeedSettings.SetCurrentSpeed(Spd);
                                    Client->SetCameraSpeedSettings(SpeedSettings);
                                }
                            })));
                    }
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 3.0f, 0.0f)
                    [
                        SNew(SNGonVectorIcon)
                        .Kind(ENGonTreeIconKind::ViewCamSpeed)
                        .Color(FLinearColor(0.80f, 0.83f, 0.88f, 1.0f))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        .Text_Lambda([this]() { return FText::AsNumber(GetSkeletonTreeUIState(this).CameraSpeedScalar); })
                    ]
                ]
            ]

            // Shading / View Mode Dropdown ("Lit v")
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(5.0f, 2.0f))
                .ToolTipText(LOCTEXT("ShadingModeTip", "Preview shading"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    auto AddShadingMode = [this, &Menu](ENGonViewportShading Mode, const FText& Label)
                    {
                        Menu.AddMenuEntry(Label, FText(), FSlateIcon(),
                            FUIAction(
                                FExecuteAction::CreateLambda([this, Mode]() { Viewport->SetShadingMode(Mode); }),
                                FCanExecuteAction(),
                                FIsActionChecked::CreateLambda([this, Mode]() { return Viewport->GetShadingMode() == Mode; })),
                            NAME_None, EUserInterfaceActionType::RadioButton);
                    };
                    AddShadingMode(ENGonViewportShading::Smooth, LOCTEXT("ShadeSmooth", "Smooth"));
                    AddShadingMode(ENGonViewportShading::Faceted, LOCTEXT("ShadeFaceted", "Faceted"));
                    AddShadingMode(ENGonViewportShading::Wireframe, LOCTEXT("ShadeWireframe", "Wireframe (Edges Only)"));
                    Menu.AddMenuSeparator();
                    Menu.AddMenuEntry(
                        LOCTEXT("ShadeXRayToggle", "X-Ray (Half-Transparent Face)"),
                        LOCTEXT("ShadeXRayTip", "Make faces half-transparent (0..1 opacity in Details/Settings) and allow through-mesh vertex detection."),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                const bool bNext = !NGon::IsXRayEnabled(&Viewport->Preview());
                                NGon::SetXRayEnabled(&Viewport->Preview(), bNext);
                                Viewport->Invalidate();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]()
                            {
                                return NGon::IsXRayEnabled(&Viewport->Preview());
                            })),
                        NAME_None,
                        EUserInterfaceActionType::ToggleButton);
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
                    [
                        SNew(SNGonVectorIcon)
                        .Kind(ENGonTreeIconKind::ViewLit)
                        .Color(FLinearColor(0.82f, 0.85f, 0.90f, 1.0f))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        .Text_Lambda([this]()
                        {
                            switch (Viewport->GetShadingMode())
                            {
                            case ENGonViewportShading::Smooth: return LOCTEXT("ShadeSmooth", "Smooth");
                            case ENGonViewportShading::Wireframe: return LOCTEXT("ShadeWireframe", "Wireframe");
                            default: return LOCTEXT("ShadeFaceted", "Faceted");
                            }
                        })
                    ]
                ]
            ]

            // Show Flags Eye Dropdown ("[Eye v]")
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(4.0f, 2.0f))
                .ToolTipText(LOCTEXT("ShowFlagsEyeTip", "Viewport Show Flags (Grid, N-Gon Edges, Vertices, HUD Overlays)"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    Menu.BeginSection("ViewportShow", LOCTEXT("ViewportShowSec", "Show Flags"));
                    Menu.AddMenuEntry(
                        LOCTEXT("ShowGridMenu", "Ground Grid"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]() { Viewport->SetGridVisible(!Viewport->IsGridVisible()); }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]() { return Viewport->IsGridVisible(); })),
                        NAME_None,
                        EUserInterfaceActionType::Check);
                    Menu.AddMenuEntry(
                        LOCTEXT("ShowEdgesMenu", "Authored N-Gon Edges (Alt+9)"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                bShowEdges = !bShowEdges;
                                Viewport->Preview().SetShowEdges(bShowEdges);
                                Viewport->Invalidate();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]() { return bShowEdges; })),
                        NAME_None,
                        EUserInterfaceActionType::Check);
                    Menu.AddMenuEntry(
                        LOCTEXT("ShowVertsMenu", "Logical Vertices"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                const bool bCurrent = Viewport.IsValid() ? NGon::GetSceneAccel(&Viewport->Preview()).bShowVertices : bShowVertices;
                                bShowVertices = !bCurrent;
                                Viewport->Preview().SetShowVertices(bShowVertices);
                                Viewport->Invalidate();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]() { return Viewport.IsValid() ? NGon::GetSceneAccel(&Viewport->Preview()).bShowVertices : bShowVertices; })),
                        NAME_None,
                        EUserInterfaceActionType::Check);
                    Menu.AddMenuEntry(
                        LOCTEXT("ShowStatsMenu", "Top-Left Mesh & Bone Stats HUD"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]() { bShowStatsOverlay = !bShowStatsOverlay; }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]() { return bShowStatsOverlay; })),
                        NAME_None,
                        EUserInterfaceActionType::Check);
                    Menu.AddMenuEntry(
                        LOCTEXT("ShowHotkeysMenu", "Top-Right Hotkeys Legend"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]() { bShowHotkeysOverlay = !bShowHotkeysOverlay; }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]() { return bShowHotkeysOverlay; })),
                        NAME_None,
                        EUserInterfaceActionType::Check);
                    Menu.EndSection();
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SNGonVectorIcon)
                    .Kind(ENGonTreeIconKind::ViewEye)
                    .Color(FLinearColor(0.82f, 0.85f, 0.90f, 1.0f))
                ]
            ]

            // Skeleton / Bone Display Dropdown ("[Skull v]" matching image_3aff81.png!)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(4.0f, 2.0f))
                .ToolTipText(LOCTEXT("SkullBoneMenuTip", "Character / Skeleton Bone Drawing & Joint Options"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    Menu.BeginSection("BoneDrawing", LOCTEXT("BoneDrawSec", "Bone Drawing"));
                    auto AddBoneDrawMode = [this, &Menu](int32 Mode, const FText& Label)
                    {
                        Menu.AddMenuEntry(
                            Label,
                            FText(),
                            FSlateIcon(),
                            FUIAction(
                                FExecuteAction::CreateLambda([this, Mode]()
                                {
                                    GetSkeletonTreeUIState(this).BoneDrawMode = Mode;
                                    Viewport->Invalidate();
                                }),
                                FCanExecuteAction(),
                                FIsActionChecked::CreateLambda([this, Mode]()
                                {
                                    return GetSkeletonTreeUIState(this).BoneDrawMode == Mode;
                                })),
                            NAME_None,
                            EUserInterfaceActionType::RadioButton);
                    };
                    AddBoneDrawMode(0, LOCTEXT("BoneDrawAll", "All Hierarchy"));
                    AddBoneDrawMode(1, LOCTEXT("BoneDrawSubtree", "Selected & Subtree"));
                    AddBoneDrawMode(2, LOCTEXT("BoneDrawSelected", "Selected Bone Only"));
                    AddBoneDrawMode(3, LOCTEXT("BoneDrawNone", "None (Hide Bones)"));
                    Menu.EndSection();

                    Menu.BeginSection("BoneLabels", LOCTEXT("BoneLabelsSec", "Labels & Pose"));
                    Menu.AddMenuEntry(
                        LOCTEXT("ShowBoneNamesEntry", "Show 3D Bone Name Labels"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(
                            FExecuteAction::CreateLambda([this]()
                            {
                                FNGonSkeletonTreeUIState& State = GetSkeletonTreeUIState(this);
                                State.bShowBoneNames = !State.bShowBoneNames;
                                Viewport->Invalidate();
                            }),
                            FCanExecuteAction(),
                            FIsActionChecked::CreateLambda([this]()
                            {
                                return GetSkeletonTreeUIState(this).bShowBoneNames;
                            })),
                        NAME_None,
                        EUserInterfaceActionType::Check);
                    Menu.AddMenuEntry(
                        LOCTEXT("ResetBoneSkullMenu", "Reset Selected Bone (Ctrl+I)"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]()
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
                        })));
                    Menu.AddMenuEntry(
                        LOCTEXT("ResetAllBonesSkullMenu", "Reset All Bones (Ctrl+Shift+I)"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]()
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
                        })));
                    Menu.EndSection();
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(SNGonVectorIcon)
                    .Kind(ENGonTreeIconKind::ViewSkull)
                    .Color(FLinearColor(0.86f, 0.89f, 0.94f, 1.0f))
                ]
            ]

            // LOD Auto Dropdown ("LOD Auto v" + Frame / Actions)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f)
            [
                SNew(SComboButton)
                .ContentPadding(FMargin(5.0f, 2.0f))
                .ToolTipText(LOCTEXT("LodMenuTip", "Mesh Level of Detail & Quick Viewport Actions"))
                .OnGetMenuContent_Lambda([this]()
                {
                    FMenuBuilder Menu(true, nullptr);
                    Menu.BeginSection("LodSec", LOCTEXT("LodSecTitle", "LOD Selection"));
                    Menu.AddMenuEntry(LOCTEXT("LodAutoItem", "LOD Auto"), FText(), FSlateIcon(), FUIAction());
                    Menu.AddMenuEntry(LOCTEXT("Lod0Item", "LOD 0 (Base N-Gon / Skeletal)"), FText(), FSlateIcon(), FUIAction());
                    Menu.EndSection();
                    Menu.BeginSection("QuickActions", LOCTEXT("QuickActionsSec", "Viewport Actions"));
                    Menu.AddMenuEntry(
                        LOCTEXT("FrameMeshMenu", "Frame Selection / Mesh (F)"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]() { Viewport->FrameMesh(); })));
                    Menu.AddMenuEntry(
                        LOCTEXT("DeleteFacesMenu", "Delete Selected Faces (Del)"),
                        FText(),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]() { Viewport->DeleteSelectedFaces(); })));
                    Menu.EndSection();
                    return Menu.MakeWidget();
                })
                .ButtonContent()
                [
                    SNew(STextBlock)
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                    .Text(LOCTEXT("LodAutoLabel", "LOD Auto"))
                ]
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
                MakeHotkeyRow(LOCTEXT("HkMode", "Selection Mode:"), LOCTEXT("HkModeKeys", "1 Vtx / 2 Edge / 3 Face / 4 Joint"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkGizmo", "Transform Gizmo:"), LOCTEXT("HkGizmoKeys", "Q Select / W Move / E Rot / R Scale"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkSoftSel", "Soft Selection:"), LOCTEXT("HkSoftSelKeys", "B Toggle / B+LMB Drag Radius"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkPieMenu", "Pie Menu:"), LOCTEXT("HkPieMenuKeys", "RMB Tap / Space (Context Vertex/Edge/Face/Object)"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkToggleEdges", "Toggle Mesh Edges Rendering:"), LOCTEXT("HkToggleEdgesKey", "Alt+9"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkResetBone", "Reset Bone Transforms:"), LOCTEXT("HkResetBoneKey", "Ctrl+I"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkResetAllBones", "Reset All Bones Transforms:"), LOCTEXT("HkResetAllBonesKey", "Ctrl+Shift+I"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkEdgeLoop", "Select Edge Loop:"), LOCTEXT("HkEdgeLoopKey", "Ctrl+LMB"))
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                MakeHotkeyRow(LOCTEXT("HkFrame", "Frame Selection / Mesh:"), LOCTEXT("HkFrameKey", "F"))
            ]
        ];
}


TSharedRef<SWidget> SNGonPreview::MakeStatus()
{
    return SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(FLinearColor(0.055f, 0.062f, 0.075f, 1.0f))
        .Clipping(EWidgetClipping::ClipToBounds)
        .Padding(FMargin(8.0f, 4.0f))
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
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
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
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
            ]
        ];
}



#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2

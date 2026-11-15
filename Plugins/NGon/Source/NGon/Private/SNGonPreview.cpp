#include "SNGonPreview.h"

#include "AssetRegistry/AssetData.h"
#include "DesktopPlatformModule.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "IDesktopPlatform.h"
#include "InputCoreTypes.h"
#include "Misc/Paths.h"
#include "NGonFbx.h"
#include "NGonMeshFiles.h"
#include "NGonPreviewScene.h"
#include "PropertyCustomizationHelpers.h"
#include "SceneView.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "NGonPreview"

namespace
{
// Base classes are destroyed in reverse order. Keep the scene alive through
// the FEditorViewportClient destructor, which holds its FPreviewScene pointer.
struct FNGonPreviewSceneOwner
{
    explicit FNGonPreviewSceneOwner(const TSharedRef<FNGonPreviewScene>& InPreview)
        : PreviewOwner(InPreview)
    {
    }

    TSharedRef<FNGonPreviewScene> PreviewOwner;
};

class FNGonViewportClient final : private FNGonPreviewSceneOwner, public FEditorViewportClient
{
public:
    FNGonViewportClient(
        const TSharedRef<FNGonPreviewScene>& InPreview,
        const TSharedRef<SNGonViewport>& InWidget)
        : FNGonPreviewSceneOwner(InPreview)
        , FEditorViewportClient(nullptr, &InPreview->GetPreviewScene(), InWidget)
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
        if (FEditorModeTools* ViewportModeTools = GetModeTools())
        {
            if (UModeManagerInteractiveToolsContext* ToolsContext = ViewportModeTools->GetInteractiveToolsContext();
                ToolsContext && ToolsContext->ToolManager)
            {
                PreviewOwner->SetOverlayMaterials(ToolsContext->ToolManager.Get());
            }
        }
    }

    void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key,
        EInputEvent Event, uint32 HitX, uint32 HitY) override
    {
        if (Key == EKeys::LeftMouseButton && !IsAltPressed())
        {
            if (const TSharedPtr<SNGonViewport> Pinned = Widget.Pin())
            {
                Pinned->Pick(View, FVector2D(HitX, HitY), IsShiftPressed());
                return;
            }
        }
        FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
    }

private:
    TWeakPtr<SNGonViewport> Widget;
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
    }
}

void SNGonViewport::Pick(const FSceneView& View, const FVector2D& Cursor, bool bAdd)
{
    PreviewScene->Pick(SelectionMode, View, Cursor, bAdd);
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
            return SelectionMode == Element ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
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
            SNew(SCheckBox).IsChecked(ECheckBoxState::Checked)
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                Viewport->Preview().SetShowEdges(State == ECheckBoxState::Checked);
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
            })
            [
                SNew(STextBlock).Text(LOCTEXT("Vertices", "Show Vertices"))
            ]
        ]
        + SWrapBox::Slot()
        [
            SNew(SCheckBox)
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bShowHeatmap = State == ECheckBoxState::Checked;
                Viewport->Preview().SetHeatmapVisible(bShowHeatmap);
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
                .IsEnabled_Lambda([this] { return bShowHeatmap; })
                .Value_Lambda([this] { return HeatBias; })
                .OnValueChanged_Lambda([this](float Value)
                {
                    HeatBias = Value;
                    Viewport->Preview().SetHeatBias(Value);
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
                })
            ]
        ]
        + SWrapBox::Slot()
        [
            SNew(STextBlock).Text(LOCTEXT("Controls", "Shift-click to add to selection  ·  Alt + drag to orbit"))
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

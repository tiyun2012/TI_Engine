#include "SNGonPreview.h"
#include "NGonSceneSkeleton.h"
#include "NGonUIIcons.h"

#include "Animation/MorphTarget.h"
#include "AssetRegistry/AssetData.h"
#include "DesktopPlatformModule.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "IDesktopPlatform.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "NGonFbx.h"
#include "NGonMeshFiles.h"
#include "NGonPreviewScene.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "STiPaintEditor.h"
#include "StaticMeshAttributes.h"
#include "TiPaintDocument.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"

namespace NGon
{
void SetActiveViewportUVMesh(const FNGonMeshData& Data, const FText& InSourceName, const FText& InSourceType);
void NotifyHostViewportClosed();
}

using NGon::FNGonSocketEntry;
using NGon::FNGonSkeletonTreeUIState;
using NGon::GetSkeletonTreeUIState;

#define LOCTEXT_NAMESPACE "NGonPreview"

namespace
{
template <typename SlotType>
TArray<UMaterialInterface*> BindPreviewMaterials(const FMeshDescription& Source,
    const TArray<SlotType>& AssetSlots, FNGonMeshData& Data)
{
    const auto SlotNames = FStaticMeshConstAttributes(Source).GetPolygonGroupMaterialSlotNames();
    TMap<FPolygonGroupID, int32> GroupToPreviewSlot;
    TArray<UMaterialInterface*> Materials;
    for (const FPolygonGroupID Group : Source.PolygonGroups().GetElementIDs())
    {
        const FName Name = SlotNames.IsValid() ? SlotNames[Group] : NAME_None;
        const SlotType* Slot = nullptr;
        if (!Name.IsNone())
        {
            Slot = AssetSlots.FindByPredicate([Name](const SlotType& Entry) { return Entry.ImportedMaterialSlotName == Name; });
            if (!Slot)
            {
                Slot = AssetSlots.FindByPredicate([Name](const SlotType& Entry) { return Entry.MaterialSlotName == Name; });
            }
        }
        if (!Slot && AssetSlots.IsValidIndex(Group.GetValue()))
        {
            Slot = &AssetSlots[Group.GetValue()];
        }
        GroupToPreviewSlot.Add(Group, Materials.Add(Slot ? Slot->MaterialInterface.Get() : nullptr));
    }

    auto* Attributes = Data.Mesh.Attributes();
    if (!Attributes->HasMaterialID())
    {
        Attributes->EnableMaterialID();
    }
    auto* MaterialIds = Attributes->GetMaterialID();
    for (const TPair<int32, FNGonFace>& Face : Data.Topology.Faces)
    {
        const FPolygonGroupID Group = Source.GetPolygonPolygonGroup(FPolygonID(Face.Key));
        const int32 Index = GroupToPreviewSlot.FindChecked(Group);
        for (const int32 TriangleId : Face.Value.Triangles)
        {
            MaterialIds->SetValue(TriangleId, Index);
        }
    }
    return Materials;
}
}

void SNGonPreview::Construct(const FArguments& InArgs)
{
    SAssignNew(Viewport, SNGonViewport);
    SAssignNew(Painter, STiPaintEditor);
    Painter->SetPaintedCallback([this](const TArray<FColor>& Pixels, int32 Width, int32 Height)
    {
        if (Viewport.IsValid())
        {
            Viewport->SetPaintTexture(Pixels, Width, Height);
            Viewport->Invalidate();
        }
    });

    FNGonSkeletonTreeUIState& InitUIState = GetSkeletonTreeUIState(this);
    InitUIState.ViewportPtr = Viewport.Get();
    InitUIState.OnViewportBonePicked = [this](int32 PickedBoneIdx)
    {
        if (SkeletonBones.IsValidIndex(PickedBoneIdx))
        {
            GetSkeletonTreeUIState(this).SelectedSocketIndex = INDEX_NONE;
            SelectedBoneIndex = PickedBoneIdx;
            ActiveLowerTab = ENGonRightLowerTab::Details;
            RebuildSkeletonTreeList();
        }
    };
    InitUIState.OnViewportBoneTransformChanged = [this]()
    {
        for (int32 i = 0; i < SkeletonBones.Num(); ++i)
        {
            FTransform CurLocal, CurComp, RefLocal, RefComp;
            if (NGon::GetSceneBoneTransforms(&Viewport->Preview(), i, CurLocal, CurComp, RefLocal, RefComp))
            {
                SkeletonBones[i].LocalTransform = CurLocal;
                SkeletonBones[i].ComponentTransform = CurComp;
            }
        }
    };
    InitUIState.OnToggleEdgesShortcut = [this]()
    {
        bShowEdges = !bShowEdges;
        Viewport->Preview().SetShowEdges(bShowEdges);
        Viewport->Invalidate();
    };

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
                    // 3D Viewport + Constant-Height Draggable Bottom Status Bar (vertical splitter prevents viewport jump)
                    + SVerticalBox::Slot()
                    .FillHeight(1.0f)
                    [
                        SNew(SSplitter)
                        .Orientation(Orient_Vertical)
                        .PhysicalSplitterHandleSize(4.0f)
                        + SSplitter::Slot()
                        .Value(0.92f)
                        .MinSize(220.0f)
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
                        + SSplitter::Slot()
                        .Value(0.08f)
                        .MinSize(44.0f)
                        [
                            MakeStatus()
                        ]
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

SNGonPreview::~SNGonPreview()
{
    NGon::RemoveSkeletonTreeUIState(this);
    if (Painter)
    {
        Painter->SetPaintedCallback({});
    }
    NGon::NotifyHostViewportClosed();
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
        {
            FNGonSkeletonTreeUIState& St = GetSkeletonTreeUIState(this);
            St.CollapsedBones.Reset();
            St.Sockets.Reset();
            St.SelectedSocketIndex = INDEX_NONE;
        }
        RebuildSkeletonTreeList();
        RebuildMorphTargetList();
        bShowEdges = true;
        bShowVertices = true;
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

    bShowEdges = !bTriangleOnly;
    SelectionMode = Viewport.IsValid() ? Viewport->GetSelectionMode() : SelectionMode;
    bShowVertices = (SelectionMode == ENGonElement::Vertex);
    TArray<UMaterialInterface*> Materials = BindPreviewMaterials(*Source, StaticMesh->GetStaticMaterials(), Data);
    ShowMesh(MoveTemp(Data), FText::FromName(Asset.AssetName), LOCTEXT("StaticMeshType", "Static Mesh"),
        ENGonViewportShading::Smooth, MoveTemp(Materials));
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

    // Extract LOD 0 Skin Weights from ImportedModel SoftVertices mapped to our Logical Vertices
    const int32 NumLogicalVerts = Data.Topology.RestPositions.Num();
    TArray<TArray<NGon::FNGonVertexBoneWeight>> ExtractedSkinWeights;
    ExtractedSkinWeights.SetNum(NumLogicalVerts);

    if (const FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel())
    {
        if (ImportedModel->LODModels.IsValidIndex(0))
        {
            const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[0];

            // Build fast lookup from SourceVertexId (ImportVertexId) -> LogicalVertexId
            TMap<int32, int32> SourceToLogical;
            SourceToLogical.Reserve(NumLogicalVerts);
            for (int32 LogicalIdx = 0; LogicalIdx < Data.Topology.SourceVertexIds.Num(); ++LogicalIdx)
            {
                SourceToLogical.FindOrAdd(Data.Topology.SourceVertexIds[LogicalIdx], LogicalIdx);
            }

            // Also build quantized 3D position lookup for direct position matching
            TMultiMap<FIntVector, int32> PosHashToLogical;
            PosHashToLogical.Reserve(NumLogicalVerts);
            for (int32 LogicalIdx = 0; LogicalIdx < NumLogicalVerts; ++LogicalIdx)
            {
                const FVector& P = Data.Topology.RestPositions[LogicalIdx];
                const FIntVector Key(
                    FMath::RoundToInt(P.X * 20.0),
                    FMath::RoundToInt(P.Y * 20.0),
                    FMath::RoundToInt(P.Z * 20.0));
                PosHashToLogical.Add(Key, LogicalIdx);
            }

            int32 GlobalSoftIdx = 0;
            for (const FSkelMeshSection& Section : LODModel.Sections)
            {
                for (const FSoftSkinVertex& SoftVert : Section.SoftVertices)
                {
                    TArray<NGon::FNGonVertexBoneWeight, TInlineAllocator<12>> VertWeights;
                    float WeightSum = 0.0f;
                    for (int32 InfIdx = 0; InfIdx < MAX_TOTAL_INFLUENCES; ++InfIdx)
                    {
                        const uint32 RawW = static_cast<uint32>(SoftVert.InfluenceWeights[InfIdx]);
                        if (RawW > 0)
                        {
                            const int32 SecBoneIdx = static_cast<int32>(SoftVert.InfluenceBones[InfIdx]);
                            if (Section.BoneMap.IsValidIndex(SecBoneIdx))
                            {
                                const int32 RefBoneIdx = static_cast<int32>(Section.BoneMap[SecBoneIdx]);
                                if (RefBoneIdx >= 0 && RefBoneIdx < NumBones)
                                {
                                    const float W = static_cast<float>(RawW);
                                    VertWeights.Add({ RefBoneIdx, W });
                                    WeightSum += W;
                                }
                            }
                        }
                    }

                    if (WeightSum > 0.0f)
                    {
                        for (NGon::FNGonVertexBoneWeight& BW : VertWeights)
                        {
                            BW.Weight /= WeightSum;
                        }

                        bool bMappedByImportId = false;
                        if (LODModel.MeshToImportVertexMap.IsValidIndex(GlobalSoftIdx))
                        {
                            const int32 ImportId = LODModel.MeshToImportVertexMap[GlobalSoftIdx];
                            if (const int32* LogicalPtr = SourceToLogical.Find(ImportId))
                            {
                                if (ExtractedSkinWeights[*LogicalPtr].IsEmpty())
                                {
                                    ExtractedSkinWeights[*LogicalPtr].Append(VertWeights.GetData(), VertWeights.Num());
                                    bMappedByImportId = true;
                                }
                            }
                        }

                        if (!bMappedByImportId)
                        {
                            const FVector SoftPos(SoftVert.Position);
                            const FIntVector Key(
                                FMath::RoundToInt(SoftPos.X * 20.0),
                                FMath::RoundToInt(SoftPos.Y * 20.0),
                                FMath::RoundToInt(SoftPos.Z * 20.0));
                            TArray<int32, TInlineAllocator<8>> Matches;
                            PosHashToLogical.MultiFind(Key, Matches);
                            for (const int32 CandLogical : Matches)
                            {
                                if (ExtractedSkinWeights[CandLogical].IsEmpty() &&
                                    FVector::DistSquared(Data.Topology.RestPositions[CandLogical], SoftPos) <= 0.04)
                                {
                                    ExtractedSkinWeights[CandLogical].Append(VertWeights.GetData(), VertWeights.Num());
                                }
                            }
                        }
                    }
                    ++GlobalSoftIdx;
                }
            }
        }
    }

    TArray<NGon::FNGonSceneBoneInit> SceneBonesInit;
    SceneBonesInit.Reserve(SkeletonBones.Num());
    for (const FNGonBoneEntry& Entry : SkeletonBones)
    {
        NGon::FNGonSceneBoneInit InitBone;
        InitBone.BoneIndex = Entry.BoneIndex;
        InitBone.ParentIndex = Entry.ParentIndex;
        InitBone.Name = Entry.Name;
        InitBone.RefLocalTransform = Entry.LocalTransform;
        InitBone.RefComponentTransform = Entry.ComponentTransform;
        SceneBonesInit.Add(InitBone);
    }

    // Prefer selecting spine_01 or pelvis by default if present so the joint gizmo is immediately ready on the torso
    const int32 Spine01Idx = RefSkeleton.FindBoneIndex(TEXT("spine_01"));
    const int32 PelvisIdx = RefSkeleton.FindBoneIndex(TEXT("pelvis"));
    if (Spine01Idx != INDEX_NONE)
    {
        SelectedBoneIndex = Spine01Idx;
    }
    else if (PelvisIdx != INDEX_NONE)
    {
        SelectedBoneIndex = PelvisIdx;
    }

    StaticMeshAssetPath.Reset();
    SkeletalMeshAssetPath = Asset.GetSoftObjectPath().ToString();
    ActiveUpperTab = ENGonRightUpperTab::SkeletonTree;
    RebuildSkeletonTreeList();
    RebuildMorphTargetList();

    // Match native UE5 Skeletal Mesh Editor: smooth lit surface with mesh edges hidden by default (toggleable via Alt+9)
    bShowEdges = false;
    SelectionMode = Viewport.IsValid() ? Viewport->GetSelectionMode() : SelectionMode;
    bShowVertices = (SelectionMode == ENGonElement::Vertex);
    SkelUIState.BoneDrawMode = 2; // Selected Bone Only (matching image_3b0306.jpg)

    // Use clean 3-point studio clay material for SkeletalMesh preview on UDynamicMeshComponent
    // (SkeletalMesh materials compiled only for GPUSkinVertexFactory fall back to WorldGridMaterial on UDynamicMeshComponent)
    ShowMesh(MoveTemp(Data), FText::FromName(Asset.AssetName), LOCTEXT("SkeletalMeshType", "Skeletal Mesh"),
        ENGonViewportShading::Smooth);

    NGon::SetSceneSkeleton(&Viewport->Preview(), SceneBonesInit, ExtractedSkinWeights);
    if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
    {
        NGon::SelectSceneBone(&Viewport->Preview(), SelectedBoneIndex, true);
        if (Viewport->GetGizmoMode() == ENGonGizmoMode::None)
        {
            Viewport->SetGizmoMode(ENGonGizmoMode::Translate);
        }
    }
    Viewport->Invalidate();
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
    NGon::FNGonFbxSkeletonData FbxSkeleton;
    const FText Name = FText::FromString(FPaths::GetCleanFilename(Filename));
    if (!NGon::LoadFbxWithSkeleton(Filename, Data, FbxSkeleton, Error))
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

    FNGonSkeletonTreeUIState& SkelUIState = GetSkeletonTreeUIState(this);
    SkelUIState.CollapsedBones.Reset();
    SkelUIState.Sockets.Reset();
    SkelUIState.SelectedSocketIndex = INDEX_NONE;

    if (!FbxSkeleton.Bones.IsEmpty())
    {
        SkeletonBones.Reserve(FbxSkeleton.Bones.Num());
        for (const NGon::FNGonSceneBoneInit& InitBone : FbxSkeleton.Bones)
        {
            FNGonBoneEntry Entry;
            Entry.BoneIndex = InitBone.BoneIndex;
            Entry.ParentIndex = InitBone.ParentIndex;
            Entry.Name = InitBone.Name;
            Entry.LocalTransform = InitBone.RefLocalTransform;
            Entry.ComponentTransform = InitBone.RefComponentTransform;
            if (Entry.ParentIndex >= 0 && SkeletonBones.IsValidIndex(Entry.ParentIndex))
            {
                Entry.Depth = SkeletonBones[Entry.ParentIndex].Depth + 1;
                ++SkeletonBones[Entry.ParentIndex].ChildCount;
            }
            else
            {
                Entry.Depth = 0;
            }
            SkeletonBones.Add(Entry);
        }
        SelectedBoneIndex = 0;
        for (const FNGonBoneEntry& Entry : SkeletonBones)
        {
            const FString LowerName = Entry.Name.ToString().ToLower();
            if (LowerName == TEXT("spine_01") || LowerName == TEXT("pelvis") || LowerName == TEXT("hips"))
            {
                SelectedBoneIndex = Entry.BoneIndex;
                break;
            }
        }
        SkelUIState.BoneDrawMode = 0; // All Hierarchy visible by default for rigged FBX
        ActiveUpperTab = ENGonRightUpperTab::SkeletonTree;
    }

    for (const NGon::FNGonFbxMorphInfo& MorphInfo : FbxSkeleton.MorphTargets)
    {
        FNGonMorphEntry MorphEntry;
        MorphEntry.Name = MorphInfo.Name;
        MorphEntry.VertexDeltaCount = MorphInfo.VertexDeltaCount;
        MorphTargets.Add(MorphEntry);
    }

    RebuildSkeletonTreeList();
    RebuildMorphTargetList();

    bShowEdges = !bTriangleOnly;
    SelectionMode = Viewport.IsValid() ? Viewport->GetSelectionMode() : SelectionMode;
    bShowVertices = (SelectionMode == ENGonElement::Vertex);
    const FText TypeLabel = FbxSkeleton.Bones.IsEmpty()
        ? LOCTEXT("FbxType", "FBX Polygon Mesh")
        : LOCTEXT("FbxSkelType", "FBX Skeletal Polygon Mesh");
    ShowMesh(MoveTemp(Data), Name, TypeLabel);

    if (!FbxSkeleton.Bones.IsEmpty())
    {
        NGon::SetSceneSkeleton(&Viewport->Preview(), FbxSkeleton.Bones, FbxSkeleton.VertexSkinWeights);
        if (SkeletonBones.IsValidIndex(SelectedBoneIndex))
        {
            NGon::SelectSceneBone(&Viewport->Preview(), SelectedBoneIndex, false);
        }
    }
    else
    {
        NGon::ClearSceneSkeleton(&Viewport->Preview());
    }
    Viewport->Invalidate();

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

void SNGonPreview::ShowMesh(FNGonMeshData&& Data, const FText& SourceName, const FText& InSourceType,
    ENGonViewportShading Shading, TArray<UMaterialInterface*> Materials)
{
    TArray<FTiPaintUVTriangle> PaintTriangles;
    if (Data.Mesh.HasAttributes() && Data.Mesh.Attributes()->PrimaryUV())
    {
        const UE::Geometry::FDynamicMeshUVOverlay* UVs = Data.Mesh.Attributes()->PrimaryUV();
        PaintTriangles.Reserve(Data.Mesh.TriangleCount());
        for (const int32 TriangleId : Data.Mesh.TriangleIndicesItr())
        {
            if (!UVs->IsSetTriangle(TriangleId))
            {
                continue;
            }
            const UE::Geometry::FIndex3i UVIds = UVs->GetTriangle(TriangleId);
            FTiPaintUVTriangle& Triangle = PaintTriangles.AddDefaulted_GetRef();
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                Triangle.UV[Corner] = UVs->IsElement(UVIds[Corner]) ? UVs->GetElement(UVIds[Corner]) : FVector2f::ZeroVector;
            }
        }
    }
    SourceLabel = SourceName;
    SourceTypeLabel = InSourceType;
    ErrorText = FText::GetEmpty();
    bShowHeatmap = false;
    HeatBias = 0.0f;
    Bend = 0.0f;
    NGon::SetActiveViewportUVMesh(Data, SourceName, InSourceType);
    Viewport->SetMesh(MoveTemp(Data), MoveTemp(Materials));
    Viewport->SetShadingMode(Shading);
    if (Painter.IsValid())
    {
        Painter->ResetForMesh(PaintTriangles);
    }
    if (Viewport.IsValid())
    {
        SelectionMode = Viewport->GetSelectionMode();
    }
    if (SelectionMode == ENGonElement::Vertex)
    {
        bShowVertices = true;
    }
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
    if (Element == ENGonElement::Vertex)
    {
        bShowVertices = true;
    }
    Viewport->SetSelectionMode(Element);
}

#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2

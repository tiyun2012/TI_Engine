#include "NGonPreviewScene.h"
#include "NGonSceneSkeleton.h"

#include "BaseGizmos/CombinedTransformGizmo.h"
#include "BaseGizmos/GizmoBaseComponent.h"
#include "BaseGizmos/GizmoViewContext.h"
#include "BaseGizmos/TransformGizmoUtil.h"
#include "BaseGizmos/TransformProxy.h"
#include "BaseGizmos/ViewAdjustedStaticMeshGizmoComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Drawing/LineSetComponent.h"
#include "Drawing/PointSetComponent.h"
#include "Drawing/PreviewGeometryActor.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "Engine/Texture2D.h"
#include "InteractiveGizmoManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPixelNormalWS.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TextureResource.h"
#include "ToolSetupUtil.h"
#include "UDynamicMesh.h"

using namespace UE::Geometry;
using NGon::FNGonSceneAccel;
using NGon::GetSceneAccel;

#define LOCTEXT_NAMESPACE "NGonPreviewScene"

namespace
{
const FColor EdgeColor(80, 224, 246);
const FColor PointColor(230, 249, 255);
const FColor SelectionColor(255, 181, 65);
const FName PaintTextureParameter(TEXT("PaintTexture"));

UMaterial* MakeSurfaceMaterial(UTexture2D* PaintTexture)
{
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
    Material->SetShadingModel(MSM_Unlit);
    Material->TwoSided = true;

    auto* VertexColor = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionVertexColor::StaticClass(), -400, 0);
    auto* PaintSample = CastChecked<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, 180));
    PaintSample->Texture = PaintTexture;
    PaintSample->ParameterName = PaintTextureParameter;
    PaintSample->SamplerType = SAMPLERTYPE_Color;
    auto* MeshUV = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionTextureCoordinate::StaticClass(), -850, 180);
    auto* PaintUV = CastChecked<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionCustom::StaticClass(), -620, 180));
    PaintUV->OutputType = CMOT_Float2;
    PaintUV->Inputs.SetNum(1);
    PaintUV->Inputs[0].InputName = TEXT("MeshUV");
    PaintUV->Inputs[0].Input.Connect(0, MeshUV);
    PaintUV->Code = TEXT("return float2(MeshUV.x, 1.0 - MeshUV.y);");
    PaintSample->Coordinates.Connect(0, PaintUV);
    auto* Normal = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionPixelNormalWS::StaticClass(), -400, 360);
    auto* Camera = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionCameraVectorWS::StaticClass(), -400, 490);
    auto* Shade = CastChecked<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionCustom::StaticClass(), -180, 0));
    Shade->OutputType = CMOT_Float3;
    Shade->Inputs.SetNum(5);
    Shade->Inputs[0].InputName = TEXT("MeshData");
    Shade->Inputs[0].Input.Connect(0, VertexColor);
    Shade->Inputs[1].InputName = TEXT("PaintColor");
    Shade->Inputs[1].Input.Connect(0, PaintSample);
    Shade->Inputs[2].InputName = TEXT("PaintAlpha");
    // Texture Sample exposes RGB at output 0 and alpha at output 4.
    Shade->Inputs[2].Input.Connect(4, PaintSample);
    Shade->Inputs[3].InputName = TEXT("SurfaceNormal");
    Shade->Inputs[3].Input.Connect(0, Normal);
    Shade->Inputs[4].InputName = TEXT("ViewDirection");
    Shade->Inputs[4].Input.Connect(0, Camera);
    // MeshData contains soft weight, face selection and heatmap visibility.
    // Paint is sampled separately in UV space so it cannot change those flags.
    Shade->Code = TEXT(
        "float t = saturate(MeshData.r);\n"
        "float3 cold = float3(0.025, 0.11, 0.85);\n"
        "float3 mid = float3(0.055, 0.76, 0.34);\n"
        "float3 hot = float3(0.98, 0.08, 0.018);\n"
        "float3 ramp = lerp(lerp(cold, mid, saturate(2 * t)), hot, saturate(2 * t - 1));\n"
        "float3 baseAlbedo = float3(0.56, 0.58, 0.61);\n"
        "float3 painted = lerp(baseAlbedo, PaintColor.rgb, saturate(PaintAlpha));\n"
        "float3 surface = lerp(painted, ramp, MeshData.b);\n"
        "float3 albedo = lerp(surface, float3(1.0, 0.48, 0.075), 0.65 * MeshData.g);\n"
        "float3 N = SurfaceNormal;\n"
        "float3 V = ViewDirection;\n"
        "float3 Lkey = normalize(float3(0.42, -0.52, 0.74));\n"
        "float3 Lfill = normalize(float3(-0.65, 0.38, 0.25));\n"
        "float ndotlKey = saturate(dot(N, Lkey));\n"
        "float ndotlFill = saturate(dot(N, Lfill));\n"
        "float ndotv = abs(dot(N, V));\n"
        "float hemi = saturate(N.z * 0.5 + 0.5);\n"
        "float3 halfVector = Lkey + V;\n"
        "float3 H = halfVector * rsqrt(max(dot(halfVector, halfVector), 1e-6));\n"
        "float spec = pow(saturate(dot(N, H)), 28.0) * 0.22 * (1.0 - MeshData.b);\n"
        "float lighting = 0.28 + 0.48 * ndotlKey + 0.18 * ndotlFill + 0.14 * hemi + 0.16 * ndotv;\n"
        "return albedo * lighting + float3(spec, spec, spec);");

    Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Shade);
    Material->PostEditChange();
    return Material;
}

UTexture2D* MakePaintTexture(int32 Width, int32 Height)
{
    UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
    if (Texture)
    {
        Texture->SRGB = true;
        Texture->Filter = TF_Bilinear;
        Texture->AddressX = TA_Clamp;
        Texture->AddressY = TA_Clamp;
        Texture->NeverStream = true;
    }
    return Texture;
}

bool UploadPaintPixels(UTexture2D* Texture, const FColor* Pixels, int64 PixelCount)
{
    if (!Texture || !Pixels || PixelCount <= 0)
    {
        return false;
    }
    FTexturePlatformData* PlatformData = Texture->GetPlatformData();
    if (!PlatformData || PlatformData->Mips.Num() == 0)
    {
        return false;
    }
    FTexture2DMipMap& Mip = PlatformData->Mips[0];
    const int64 RequiredBytes = PixelCount * static_cast<int64>(sizeof(FColor));
    if (Mip.BulkData.GetBulkDataSize() < RequiredBytes)
    {
        return false;
    }
    void* Destination = Mip.BulkData.Lock(LOCK_READ_WRITE);
    if (Destination)
    {
        FMemory::Memcpy(Destination, Pixels, RequiredBytes);
    }
    Mip.BulkData.Unlock();
    if (!Destination)
    {
        return false;
    }
    Texture->UpdateResource();
    return true;
}
}

FNGonPreviewScene::FNGonPreviewScene()
    : Scene(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(true))
{
    GetSceneAccel(this);

    Surface = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
    Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Surface->SetCastShadow(false);
    Surface->SetEnableFlatShading(bFlatShading);
    Surface->SetVertexColorSpaceTransformMode(EDynamicMeshVertexColorTransformMode::NoTransform);
    EmptyPaintTexture.Reset(MakePaintTexture(1, 1));
    const FColor Transparent(0, 0, 0, 0);
    if (UploadPaintPixels(EmptyPaintTexture.Get(), &Transparent, 1))
    {
        SurfaceMaterial = UMaterialInstanceDynamic::Create(MakeSurfaceMaterial(EmptyPaintTexture.Get()), Surface);
    }
    Surface->SetMaterial(0, SurfaceMaterial ? static_cast<UMaterialInterface*>(SurfaceMaterial)
        : UMaterial::GetDefaultMaterial(MD_Surface));
    Scene.AddComponent(Surface, FTransform::Identity);

    Overlay.Reset(NewObject<UPreviewGeometry>());
    Overlay->CreateInWorld(Scene.GetWorld(), FTransform::Identity);
    EdgeLines = Overlay->AddLineSet(TEXT("Polygon edges"));
    VertexPoints = Overlay->AddPointSet(TEXT("Logical vertices"));
    SoftInfluencePoints = Overlay->AddPointSet(TEXT("Soft selection influence"));
    SelectedEdge = Overlay->AddLineSet(TEXT("Selected edge"));
    SelectedVertex = Overlay->AddPointSet(TEXT("Selected vertex"));
    SoftInfluencePoints->SetVisibility(false);
}

FNGonPreviewScene::~FNGonPreviewScene()
{
    TeardownGizmo();
    NGon::RemoveSceneAccel(this);
    Picker.SetMesh(nullptr);
    Overlay->Disconnect();
    Scene.RemoveComponent(Surface);
}

void FNGonPreviewScene::SetupGizmo(UInteractiveGizmoManager* InGizmoManager, UGizmoViewContext* ViewContext)
{
    TeardownGizmo();
    GizmoManager = InGizmoManager;
    TransformGizmo = UE::TransformGizmoUtil::CreateCustomTransformGizmo(
        GizmoManager, ETransformGizmoSubElements::FullTranslateRotateScale, this, TEXT("NGonPreview"));
    if (!ensureMsgf(TransformGizmo, TEXT("NGon could not create its preview transform gizmo.")))
    {
        return;
    }

    TransformGizmo->bUseContextGizmoMode = false;
    TransformGizmo->bUseContextCoordinateSystem = false;
    TransformGizmo->bSnapToWorldGrid = false;
    TransformGizmo->bSnapToWorldRotGrid = false;
    TransformGizmo->bSnapToScaleGrid = false;
    TransformGizmo->SetDisallowNegativeScaling(true);
    TransformGizmo->SetIsNonUniformScaleAllowedFunction([this]()
    {
        return !NGon::IsBoneGizmoActive(this) || !NGon::IsSceneWorldCoordinateSystem(this);
    });
    TransformProxy = NewObject<UTransformProxy>(GizmoManager);
    TransformProxy->OnBeginTransformEdit.AddLambda([this](UTransformProxy* Proxy)
    {
        BeginSoftTransform(Proxy->GetTransform(), SoftTransformMode);
    });
    TransformProxy->OnTransformChanged.AddLambda([this](UTransformProxy*, FTransform Transform)
    {
        ApplyGizmoTransform(Transform);
    });
    TransformGizmo->SetActiveTarget(TransformProxy, nullptr);
    if (ACombinedTransformGizmoActor* Actor = TransformGizmo->GetGizmoActor())
    {
        for (UGizmoBaseComponent* Component : TInlineComponentArray<UGizmoBaseComponent*>(Actor))
        {
            Component->SetGizmoViewContext(ViewContext);
            Component->MarkRenderStateDirty();
        }
        for (UViewAdjustedStaticMeshGizmoComponent* Component : TInlineComponentArray<UViewAdjustedStaticMeshGizmoComponent*>(Actor))
        {
            Component->SetGizmoViewContext(ViewContext);
            Component->MarkRenderStateDirty();
        }
    }
    UpdateGizmo();
}

void FNGonPreviewScene::TeardownGizmo()
{
    if (TransformProxy)
    {
        TransformProxy->OnBeginTransformEdit.Clear();
        TransformProxy->OnTransformChanged.Clear();
    }
    if (TransformGizmo)
    {
        GizmoManager->DestroyGizmo(TransformGizmo);
    }
    TransformGizmo = nullptr;
    TransformProxy = nullptr;
    GizmoManager = nullptr;
}

void FNGonPreviewScene::SetGizmoMode(ENGonGizmoMode InMode)
{
    if (GizmoMode != InMode)
    {
        GizmoMode = InMode;
        UpdateGizmo();
    }
}

bool FNGonPreviewScene::HasGizmoTarget() const
{
    return GizmoMode != ENGonGizmoMode::None && (NGon::IsBoneGizmoActive(this) || !SelectedIds.IsEmpty());
}

void FNGonPreviewScene::UpdateGizmo()
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);

    if (Accel.bPendingSkinningUpdate && Surface && Surface->GetDynamicMesh())
    {
        Accel.bPendingSkinningUpdate = false;
        Accel.RecomputeBoneHierarchy();

        const int32 LogicalCount = Positions.Num();
        if (Accel.SkinnedBasePositions.Num() == LogicalCount)
        {
            if (Accel.bPendingFullSkinReset)
            {
                Accel.bPendingFullSkinReset = false;
                Positions = Accel.SkinnedBasePositions;
            }
            else if (Accel.VertexSkinOffsets.Num() == LogicalCount + 1)
            {
                for (int32 LogicalId = 0; LogicalId < LogicalCount; ++LogicalId)
                {
                    const FVector& BasePos = Accel.SkinnedBasePositions[LogicalId];
                    const int32 StartW = Accel.VertexSkinOffsets[LogicalId];
                    const int32 EndW = Accel.VertexSkinOffsets[LogicalId + 1];
                    if (StartW < EndW)
                    {
                        FVector Blended = FVector::ZeroVector;
                        float TotalW = 0.0f;
                        for (int32 wIdx = StartW; wIdx < EndW; ++wIdx)
                        {
                            const NGon::FNGonVertexBoneWeight& BW = Accel.VertexSkinWeights[wIdx];
                            if (Accel.Bones.IsValidIndex(BW.BoneIndex))
                            {
                                Blended += Accel.Bones[BW.BoneIndex].SkinMatrix.TransformPosition(BasePos) * BW.Weight;
                                TotalW += BW.Weight;
                            }
                        }
                        if (TotalW > 1e-5f)
                        {
                            Positions[LogicalId] = Blended / TotalW;
                        }
                    }
                }
            }

            FDynamicMesh3& Mesh = Surface->GetDynamicMesh()->GetMeshRef();
            for (const int32 DynVert : Mesh.VertexIndicesItr())
            {
                const int32 LogicalId = Topology.DynamicToLogicalVertex.IsValidIndex(DynVert)
                    ? Topology.DynamicToLogicalVertex[DynVert] : INDEX_NONE;
                if (Positions.IsValidIndex(LogicalId))
                {
                    Mesh.SetVertex(DynVert, Positions[LogicalId]);
                }
            }
            NotifySurfacePositionsChanged(Mesh);
            Picker.PositionsChanged();
            UpdateOverlayPositions();
            DrawSelection();
            ++PositionUpdates;
        }
    }

    if (!TransformGizmo || SoftSession.IsActive() || Accel.bBoneDragActive)
    {
        return;
    }

    switch (GizmoMode)
    {
    case ENGonGizmoMode::Translate:
        TransformGizmo->ActiveGizmoMode = EToolContextTransformGizmoMode::Translation;
        break;
    case ENGonGizmoMode::Rotate:
        TransformGizmo->ActiveGizmoMode = EToolContextTransformGizmoMode::Rotation;
        break;
    case ENGonGizmoMode::Scale:
        TransformGizmo->ActiveGizmoMode = EToolContextTransformGizmoMode::Scale;
        break;
    case ENGonGizmoMode::None:
        TransformGizmo->ActiveGizmoMode = EToolContextTransformGizmoMode::NoGizmo;
        break;
    case ENGonGizmoMode::Combined:
    default:
        TransformGizmo->ActiveGizmoMode = EToolContextTransformGizmoMode::Combined;
        break;
    }
    TransformGizmo->CurrentCoordinateSystem = Accel.bUseWorldCoordinateSystem
        ? EToolContextCoordinateSystem::World
        : EToolContextCoordinateSystem::Local;

    const bool bVisible = HasGizmoTarget();
    if (bVisible)
    {
        TransformGizmo->ReinitializeGizmoTransform(GetSelectionGizmoTransform(), true);
    }
    TransformGizmo->SetVisibility(bVisible);
    GizmoManager->Tick(0.0f);
}

FBox FNGonPreviewScene::GetMeshBounds() const
{
    FBox Bounds(ForceInit);
    for (const FVector& Position : Positions)
    {
        Bounds += Position;
    }
    return Bounds;
}

FBox FNGonPreviewScene::GetSelectionOrMeshBounds() const
{
    const FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.bBoneGizmoActive && Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex))
    {
        FBox BoneBounds(ForceInit);
        const FVector BonePos = Accel.Bones[Accel.SelectedBoneIndex].CurrentComponentTransform.GetLocation();
        BoneBounds += BonePos;
        for (const int32 ChildIdx : Accel.Bones[Accel.SelectedBoneIndex].ChildBones)
        {
            if (Accel.Bones.IsValidIndex(ChildIdx))
            {
                BoneBounds += Accel.Bones[ChildIdx].CurrentComponentTransform.GetLocation();
            }
        }
        const double MinExtent = FMath::Max(Topology.RestBounds.GetSize().GetMax() * 0.12, 15.0);
        return BoneBounds.ExpandBy(MinExtent);
    }
    if (SelectedIds.IsEmpty())
    {
        return GetMeshBounds();
    }

    FBox Bounds(ForceInit);
    for (const int32 Id : SelectedIds)
    {
        if (SelectedElement == ENGonElement::Vertex && Positions.IsValidIndex(Id))
        {
            Bounds += Positions[Id];
        }
        else if (SelectedElement == ENGonElement::Edge && Topology.Edges.IsValidIndex(Id))
        {
            const FIntPoint Edge = Topology.Edges[Id];
            if (Positions.IsValidIndex(Edge.X) && Positions.IsValidIndex(Edge.Y))
            {
                Bounds += Positions[Edge.X];
                Bounds += Positions[Edge.Y];
            }
        }
        else if (SelectedElement == ENGonElement::Face)
        {
            if (const FNGonFace* Face = Topology.Faces.Find(Id))
            {
                for (const int32 Vertex : Face->Perimeter)
                {
                    if (Positions.IsValidIndex(Vertex))
                    {
                        Bounds += Positions[Vertex];
                    }
                }
            }
        }
    }
    return Bounds.IsValid ? Bounds : GetMeshBounds();
}

void FNGonPreviewScene::SetOverlayMaterials(UInteractiveToolManager* ToolManager)
{
    if (UMaterialInterface* LineMaterial = ToolSetupUtil::GetDefaultLineComponentMaterial(ToolManager, true))
    {
        Overlay->SetAllLineSetsMaterial(LineMaterial);
    }
    if (UMaterialInterface* PointMaterial = ToolSetupUtil::GetDefaultPointComponentMaterial(ToolManager, true))
    {
        Overlay->SetAllPointSetsMaterial(PointMaterial);
    }
}

void FNGonPreviewScene::SetMesh(FNGonMeshData&& Data)
{
    ClearPaintTexture();
    Picker.SetMesh(nullptr);
    SelectedElement = ENGonElement::Face;
    SelectedIds.Reset();
    ActiveSelectedId = INDEX_NONE;
    SoftSettings = FNGonSoftSelectionSettings{};
    SoftTransformMode = ENGonSoftTransformMode::Fixed;
    SoftSession.Cancel();
    SoftSolver.Reset();
    SoftInfluences.Reset();
    bHeatmapVisible = false;
    bSmoothNormalsDirty = false;
    Bend = 0.0f;
    HeatBias = 0.0f;

    Topology = MoveTemp(Data.Topology);
    Positions = Topology.RestPositions;
    RebuildGizmoAdjacency();

    const FVector Size = Topology.RestBounds.GetSize();
    int32 Axis = Size.Y > Size.X ? 1 : 0;
    if (Size.Z > Size[Axis])
    {
        Axis = 2;
    }

    ImportedSampleCount = 0;
    AxisFractions.Reset(Positions.Num());
    HeatSamples.Reset(Positions.Num());
    ImportedHeatMask.Init(false, Positions.Num());
    for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
    {
        const float Fraction = static_cast<float>(
            (Positions[Vertex][Axis] - Topology.RestBounds.Min[Axis]) / FMath::Max(Size[Axis], 1.e-8));
        AxisFractions.Add(Fraction);
        const float Imported = Data.ImportedHeatSamples.IsValidIndex(Vertex)
            ? Data.ImportedHeatSamples[Vertex] : -1.0f;
        if (Imported >= 0.0f)
        {
            HeatSamples.Add(Imported);
            ImportedHeatMask[Vertex] = true;
            ++ImportedSampleCount;
        }
        else
        {
            HeatSamples.Add(Fraction);
        }
    }
    SoftHeatSamples.Init(0.0f, Positions.Num());

    InitializeColorOverlay(Data.Mesh);
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    Accel.BuildTopologyTables(Topology, Data.Mesh);
    Accel.BuildColorTables(Positions.Num(), ColorElements.Num(), [this](int32 ElemIdx)
    {
        return ColorElements[ElemIdx].LogicalVertex;
    });

    Surface->SetMesh(MoveTemp(Data.Mesh));
    Picker.SetMesh(Surface->GetDynamicMesh()->GetMeshPtr());
    RebuildOverlays();
    ++TopologyUpdates;
    UpdateGizmo();
}

void FNGonPreviewScene::InitializeColorOverlay(FDynamicMesh3& Mesh)
{
    if (!Mesh.HasAttributes())
    {
        Mesh.EnableAttributes();
    }

    FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
    Attributes->DisableMaterialID();
    Attributes->DisablePrimaryColors();
    Attributes->EnablePrimaryColors();
    FDynamicMeshColorOverlay* Colors = Attributes->PrimaryColors();
    ColorElements.Reset();
    TMap<uint64, int32> CornerElements;
    for (const int32 TriangleId : Mesh.TriangleIndicesItr())
    {
        const int32 Face = Mesh.GetTriangleGroup(TriangleId);
        const FIndex3i Vertices = Mesh.GetTriangle(TriangleId);
        FIndex3i ColorIds;
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const int32 Vertex = Vertices[Corner];
            const uint64 Key = (static_cast<uint64>(static_cast<uint32>(Face)) << 32)
                | static_cast<uint32>(Vertex);
            int32* ColorId = CornerElements.Find(Key);
            if (!ColorId)
            {
                const int32 NewId = Colors->AppendElement(FVector4f(0, 0, 0, 1));
                ColorElements.Add({NewId, Topology.DynamicToLogicalVertex[Vertex], Face});
                ColorId = &CornerElements.Add(Key, NewId);
            }
            ColorIds[Corner] = *ColorId;
        }
        Colors->SetTriangle(TriangleId, ColorIds);
    }

    WriteColors(Mesh);
}

FVector4f FNGonPreviewScene::EncodeOverlayColor(const FColorElement& Element, bool bUseSoftHeat) const
{
    const float Sample = bUseSoftHeat && SoftHeatSamples.IsValidIndex(Element.LogicalVertex)
        ? SoftHeatSamples[Element.LogicalVertex] : 0.0f;
    const bool bSelected = SelectedElement == ENGonElement::Face && SelectedIds.Contains(Element.Face);
    return FVector4f(Sample, bSelected ? 1.0f : 0.0f, bUseSoftHeat ? 1.0f : 0.0f, 1.0f);
}

void FNGonPreviewScene::WriteColors(FDynamicMesh3& Mesh) const
{
    FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
    const bool bUseSoftHeat = bHeatmapVisible && SoftSettings.bEnabled && !SoftInfluences.IsEmpty();
    for (const FColorElement& Element : ColorElements)
    {
        Colors->SetElement(Element.OverlayId, EncodeOverlayColor(Element, bUseSoftHeat));
    }
}

void FNGonPreviewScene::RebuildSoftHeatSamples()
{
    SoftHeatSamples.Init(0.0f, Positions.Num());
    if (!SoftSettings.bEnabled || SoftInfluences.IsEmpty())
    {
        return;
    }

    for (const FNGonVertexInfluence& Influence : SoftInfluences)
    {
        if (SoftHeatSamples.IsValidIndex(Influence.Vertex))
        {
            SoftHeatSamples[Influence.Vertex] = FMath::Max(
                SoftHeatSamples[Influence.Vertex],
                FMath::Clamp(static_cast<float>(Influence.Weight), 0.0f, 1.0f));
        }
    }
}

void FNGonPreviewScene::UpdateColors()
{
    if (!ColorElements.IsEmpty())
    {
        // This private preview owns the mesh. A direct edit plus the fast notification
        // keeps the color-overlay elements and the surface triangulation in place.
        WriteColors(Surface->GetDynamicMesh()->GetMeshRef());
        Surface->FastNotifyColorsUpdated();
        ++ColorUpdates;
    }
}

void FNGonPreviewScene::RebuildOverlays()
{
    EdgeLines->Clear();
    VertexPoints->Clear();

    TArray<FVector> Starts;
    TArray<FVector> Ends;
    Starts.Reserve(Topology.Edges.Num());
    Ends.Reserve(Topology.Edges.Num());
    for (const FIntPoint& Edge : Topology.Edges)
    {
        Starts.Add(Positions[Edge.X]);
        Ends.Add(Positions[Edge.Y]);
    }

    EdgeLines->AddLines(Starts, Ends, EdgeColor, 1.6f, 0.35f);
    VertexPoints->AddPoints(Positions, PointColor, 5.0f, 0.6f);
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    Accel.bEdgesOverlayDirty = false;
    Accel.bVerticesOverlayDirty = false;
    DrawSelection();
    DrawSoftSelection();
}

void FNGonPreviewScene::UpdateOverlayPositions()
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.bShowEdges)
    {
        for (int32 EdgeIndex = 0; EdgeIndex < Topology.Edges.Num(); ++EdgeIndex)
        {
            const FIntPoint Edge = Topology.Edges[EdgeIndex];
            if (Positions.IsValidIndex(Edge.X) && Positions.IsValidIndex(Edge.Y))
            {
                EdgeLines->SetLineStart(EdgeIndex, Positions[Edge.X]);
                EdgeLines->SetLineEnd(EdgeIndex, Positions[Edge.Y]);
            }
        }
        Accel.bEdgesOverlayDirty = false;
    }
    else
    {
        Accel.bEdgesOverlayDirty = true;
    }

    if (Accel.bShowVertices)
    {
        for (int32 VertexIndex = 0; VertexIndex < Positions.Num(); ++VertexIndex)
        {
            VertexPoints->SetPointPosition(VertexIndex, Positions[VertexIndex]);
        }
        Accel.bVerticesOverlayDirty = false;
    }
    else
    {
        Accel.bVerticesOverlayDirty = true;
    }

    DrawSelection();
    DrawSoftSelection();
}

void FNGonPreviewScene::DrawSoftSelection()
{
    if (!SoftInfluencePoints)
    {
        return;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    const bool bVisible = SoftSettings.bEnabled && !SoftInfluences.IsEmpty();
    SoftInfluencePoints->SetVisibility(bVisible);
    if (!bVisible)
    {
        if (Accel.CachedSoftPointCount > 0)
        {
            SoftInfluencePoints->Clear();
            Accel.CachedSoftPointCount = 0;
        }
        return;
    }

    SoftInfluencePoints->Clear();
    int32 AddedCount = 0;
    for (const FNGonVertexInfluence& Influence : SoftInfluences)
    {
        if (!Positions.IsValidIndex(Influence.Vertex))
        {
            continue;
        }
        const float Weight = FMath::Clamp(static_cast<float>(Influence.Weight), 0.0f, 1.0f);
        const FLinearColor Color = FLinearColor::LerpUsingHSV(
            FLinearColor(0.04f, 0.35f, 1.0f), FLinearColor(1.0f, 0.12f, 0.02f), Weight);
        SoftInfluencePoints->AddPoint(Positions[Influence.Vertex], Color.ToFColor(true), 5.0f + 8.0f * Weight, 1.8f);
        ++AddedCount;
    }
    Accel.CachedSoftPointCount = AddedCount;
}

void FNGonPreviewScene::SetShowEdges(bool bShow)
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    Accel.bShowEdges = bShow;
    if (bShow && Accel.bEdgesOverlayDirty)
    {
        for (int32 EdgeIndex = 0; EdgeIndex < Topology.Edges.Num(); ++EdgeIndex)
        {
            const FIntPoint Edge = Topology.Edges[EdgeIndex];
            if (Positions.IsValidIndex(Edge.X) && Positions.IsValidIndex(Edge.Y))
            {
                EdgeLines->SetLineStart(EdgeIndex, Positions[Edge.X]);
                EdgeLines->SetLineEnd(EdgeIndex, Positions[Edge.Y]);
            }
        }
        Accel.bEdgesOverlayDirty = false;
    }
    EdgeLines->SetVisibility(bShow);
}

void FNGonPreviewScene::SetShowVertices(bool bShow)
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    Accel.bShowVertices = bShow;
    if (bShow && Accel.bVerticesOverlayDirty)
    {
        for (int32 VertexIndex = 0; VertexIndex < Positions.Num(); ++VertexIndex)
        {
            VertexPoints->SetPointPosition(VertexIndex, Positions[VertexIndex]);
        }
        Accel.bVerticesOverlayDirty = false;
    }
    VertexPoints->SetVisibility(bShow);
}

void FNGonPreviewScene::SetHeatmapVisible(bool bShow)
{
    if (bHeatmapVisible != bShow)
    {
        bHeatmapVisible = bShow;
        UpdateColors();
    }
}

void FNGonPreviewScene::SetFlatShading(bool bEnable)
{
    if (bFlatShading == bEnable)
    {
        return;
    }
    bFlatShading = bEnable;
    if (!bFlatShading && bSmoothNormalsDirty)
    {
        FMeshNormals::QuickRecomputeOverlayNormals(Surface->GetDynamicMesh()->GetMeshRef(), false, true, false);
        bSmoothNormalsDirty = false;
    }
    Surface->SetEnableFlatShading(bFlatShading);
    Surface->MarkRenderStateDirty();
}

void FNGonPreviewScene::NotifySurfacePositionsChanged(FDynamicMesh3& Mesh)
{
    // The renderer generates triangle normals for flat shading. In smooth
    // mode, refresh the imported normal overlay while retaining its hard seams.
    bSmoothNormalsDirty = bFlatShading;
    if (!bFlatShading)
    {
        FMeshNormals::QuickRecomputeOverlayNormals(Mesh, false, true, false);
    }
    Surface->FastNotifyPositionsUpdated(true, false, false);
}

void FNGonPreviewScene::SetPaintTexture(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
    const int64 Expected = static_cast<int64>(FMath::Max(0, Width)) * static_cast<int64>(FMath::Max(0, Height));
    if (Expected <= 0 || Pixels.Num() != Expected)
    {
        ClearPaintTexture();
        return;
    }
    if (!SurfaceMaterial)
    {
        return;
    }
    if (PaintTexture.IsValid() && PaintTexture->GetSizeX() == Width && PaintTexture->GetSizeY() == Height)
    {
        UploadPaintPixels(PaintTexture.Get(), Pixels.GetData(), Expected);
        return;
    }

    TStrongObjectPtr<UTexture2D> NewPaint(MakePaintTexture(Width, Height));
    if (UploadPaintPixels(NewPaint.Get(), Pixels.GetData(), Expected))
    {
        SurfaceMaterial->SetTextureParameterValue(PaintTextureParameter, NewPaint.Get());
        PaintTexture = MoveTemp(NewPaint);
    }
}

void FNGonPreviewScene::ClearPaintTexture()
{
    if (PaintTexture.IsValid())
    {
        SurfaceMaterial->SetTextureParameterValue(PaintTextureParameter, EmptyPaintTexture.Get());
        PaintTexture.Reset();
    }
}

void FNGonPreviewScene::SetHeatBias(float Value)
{
    if (HeatBias != Value)
    {
        HeatBias = Value;
        UpdateColors();
    }
}

void FNGonPreviewScene::SetBend(float Value)
{
    if (Positions.IsEmpty() || Bend == Value)
    {
        return;
    }

    Bend = Value;
    const double Amplitude = Bend * Topology.RestBounds.GetSize().GetMax() * 0.25;
    for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
    {
        Positions[Vertex] = Topology.RestPositions[Vertex];
        Positions[Vertex].Z += Amplitude * FMath::Sin(UE_DOUBLE_PI * AxisFractions[Vertex]);
    }

    FDynamicMesh3& Mesh = Surface->GetDynamicMesh()->GetMeshRef();
    for (const int32 Vertex : Mesh.VertexIndicesItr())
    {
        const int32 LogicalId = Topology.DynamicToLogicalVertex[Vertex];
        if (LogicalId != INDEX_NONE)
        {
            Mesh.SetVertex(Vertex, Positions[LogicalId]);
        }
    }

    NotifySurfacePositionsChanged(Mesh);
    Picker.PositionsChanged();
    UpdateOverlayPositions();
    if (!SelectedIds.IsEmpty())
    {
        GetSceneAccel(this).bBoneGizmoActive = false;
    }
    RebuildSoftSelection();
    UpdateGizmo();
    ++PositionUpdates;
}

void FNGonPreviewScene::SetSoftSelection(const FNGonSoftSelectionSettings& Settings)
{
    SoftSettings = Settings;
    RebuildSoftSelection();
}

void FNGonPreviewScene::RebuildSoftSelection()
{
    TArray<int32> Seeds;
    if (SelectedElement == ENGonElement::Vertex)
    {
        for (const int32 Id : SelectedIds) if (Positions.IsValidIndex(Id)) Seeds.Add(Id);
    }
    else if (SelectedElement == ENGonElement::Edge)
    {
        for (const int32 Id : SelectedIds) if (Topology.Edges.IsValidIndex(Id))
        { Seeds.AddUnique(Topology.Edges[Id].X); Seeds.AddUnique(Topology.Edges[Id].Y); }
    }
    else
    {
        for (const int32 Id : SelectedIds) if (const FNGonFace* Face = Topology.Faces.Find(Id))
            for (const int32 Vertex : Face->Perimeter) Seeds.AddUnique(Vertex);
    }
    SoftInfluences = SoftSolver.Solve(Topology, Positions, Seeds, FTransform::Identity, SoftSettings);
    RebuildSoftHeatSamples();
    DrawSoftSelection();
    UpdateColors();
}

FTransform FNGonPreviewScene::GetSelectionGizmoTransform() const
{
    const FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.bBoneGizmoActive && Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex))
    {
        const FTransform& BoneComp = Accel.Bones[Accel.SelectedBoneIndex].CurrentComponentTransform;
        const FQuat GizmoRot = Accel.bUseWorldCoordinateSystem ? FQuat::Identity : BoneComp.GetRotation();
        return FTransform(GizmoRot, BoneComp.GetLocation(), FVector::OneVector);
    }
    FTransform CompFrame = BuildComponentGizmoTransform();
    if (Accel.bUseWorldCoordinateSystem)
    {
        CompFrame.SetRotation(FQuat::Identity);
    }
    return CompFrame;
}

void FNGonPreviewScene::RebuildGizmoAdjacency()
{
    GizmoVertexFaces.Reset();
    GizmoEdgeFaces.Reset();
    for (const TPair<int32, FNGonFace>& Pair : Topology.Faces)
    {
        const int32 FaceId = Pair.Key;
        const TArray<int32>& Perimeter = Pair.Value.Perimeter;
        for (const int32 Vertex : Perimeter)
        {
            GizmoVertexFaces.FindOrAdd(Vertex).AddUnique(FaceId);
        }
        for (int32 Corner = 0; Corner < Perimeter.Num(); ++Corner)
        {
            const int32 A = Perimeter[Corner];
            const int32 B = Perimeter[(Corner + 1) % Perimeter.Num()];
            if (A != B)
            {
                GizmoEdgeFaces.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B))).AddUnique(FaceId);
            }
        }
    }
}

FTransform FNGonPreviewScene::BuildComponentGizmoTransform() const
{
    struct FSample
    {
        FVector Position = FVector::ZeroVector;
        FVector Normal = FVector::ZeroVector;
        FVector Tangent = FVector::ZeroVector;
        double Weight = 1.0;
        bool bActive = false;
    };

    auto FaceNormal = [this](const FNGonFace& Face)
    {
        FVector Normal = FVector::ZeroVector;
        if (Face.Perimeter.Num() < 3 || !Positions.IsValidIndex(Face.Perimeter[0]))
        {
            return Normal;
        }
        const FVector Origin = Positions[Face.Perimeter[0]];
        for (int32 Corner = 1; Corner < Face.Perimeter.Num(); ++Corner)
        {
            const int32 Next = (Corner + 1) % Face.Perimeter.Num();
            if (Positions.IsValidIndex(Face.Perimeter[Corner]) && Positions.IsValidIndex(Face.Perimeter[Next]))
            {
                Normal += FVector::CrossProduct(Positions[Face.Perimeter[Corner]] - Origin,
                    Positions[Face.Perimeter[Next]] - Origin);
            }
        }
        return Normal.GetSafeNormal();
    };

    TArray<FSample> Samples;
    Samples.Reserve(SelectedIds.Num());
    for (const int32 Id : SelectedIds)
    {
        FSample Sample;
        Sample.bActive = Id == ActiveSelectedId;
        if (SelectedElement == ENGonElement::Vertex)
        {
            if (!Positions.IsValidIndex(Id)) continue;
            Sample.Position = Positions[Id];
            if (const TArray<int32>* IncidentFaces = GizmoVertexFaces.Find(Id))
            {
                for (const int32 FaceId : *IncidentFaces)
                {
                    const FNGonFace* FacePtr = Topology.Faces.Find(FaceId);
                    if (!FacePtr) continue;
                    const FNGonFace& Face = *FacePtr;
                    const FVector FaceN = FaceNormal(Face);
                    Sample.Normal += FaceN;
                    for (int32 Corner = 0; Corner < Face.Perimeter.Num(); ++Corner)
                    {
                        if (Face.Perimeter[Corner] != Id) continue;
                        const int32 Next = Face.Perimeter[(Corner + 1) % Face.Perimeter.Num()];
                        if (Positions.IsValidIndex(Next))
                        {
                            // Accumulate incident perimeter directions so the vertex
                            // frame does not depend on TMap face iteration order.
                            Sample.Tangent += Positions[Next] - Sample.Position;
                        }
                        break;
                    }
                }
            }
            Sample.Weight = 1.0;
        }
        else if (SelectedElement == ENGonElement::Edge)
        {
            if (!Topology.Edges.IsValidIndex(Id)) continue;
            const FIntPoint Edge = Topology.Edges[Id];
            if (!Positions.IsValidIndex(Edge.X) || !Positions.IsValidIndex(Edge.Y)) continue;
            Sample.Position = (Positions[Edge.X] + Positions[Edge.Y]) * 0.5;
            Sample.Tangent = Positions[Edge.Y] - Positions[Edge.X];
            Sample.Weight = FMath::Max(Sample.Tangent.Size(), 1.e-4);
            const FIntPoint EdgeKey(FMath::Min(Edge.X, Edge.Y), FMath::Max(Edge.X, Edge.Y));
            if (const TArray<int32>* IncidentFaces = GizmoEdgeFaces.Find(EdgeKey))
            {
                for (const int32 FaceId : *IncidentFaces)
                {
                    if (const FNGonFace* Face = Topology.Faces.Find(FaceId))
                    {
                        Sample.Normal += FaceNormal(*Face);
                    }
                }
            }
        }
        else
        {
            const FNGonFace* Face = Topology.Faces.Find(Id);
            if (!Face || Face->Perimeter.Num() < 3) continue;
            for (const int32 Vertex : Face->Perimeter)
            {
                if (Positions.IsValidIndex(Vertex)) Sample.Position += Positions[Vertex];
            }
            Sample.Position /= static_cast<double>(Face->Perimeter.Num());
            Sample.Normal = FaceNormal(*Face);
            if (Positions.IsValidIndex(Face->Perimeter[0]) && Positions.IsValidIndex(Face->Perimeter[1]))
            {
                Sample.Tangent = Positions[Face->Perimeter[1]] - Positions[Face->Perimeter[0]];
            }
            double Area = 0.0;
            for (int32 Corner = 1; Corner + 1 < Face->Perimeter.Num(); ++Corner)
            {
                if (Positions.IsValidIndex(Face->Perimeter[0]) && Positions.IsValidIndex(Face->Perimeter[Corner]) && Positions.IsValidIndex(Face->Perimeter[Corner + 1]))
                {
                    Area += FVector::CrossProduct(Positions[Face->Perimeter[Corner]] - Positions[Face->Perimeter[0]],
                        Positions[Face->Perimeter[Corner + 1]] - Positions[Face->Perimeter[0]]).Size() * 0.5;
                }
            }
            Sample.Weight = FMath::Max(Area, 1.e-4);
        }
        if (!Sample.Normal.Normalize()) continue;
        Sample.Tangent = Sample.Tangent - Sample.Normal * FVector::DotProduct(Sample.Tangent, Sample.Normal);
        if (!Sample.Tangent.Normalize())
        {
            FVector FallbackY;
            Sample.Normal.FindBestAxisVectors(Sample.Tangent, FallbackY);
        }
        Samples.Add(Sample);
    }

    if (Samples.IsEmpty()) return FTransform::Identity;
    const FSample* Active = nullptr;
    if (GizmoFramePolicy == ENGonGizmoFramePolicy::ActiveElement)
    {
        for (const FSample& Sample : Samples) if (Sample.bActive) { Active = &Sample; break; }
        if (!Active) Active = &Samples[0];
    }

    FVector Position = FVector::ZeroVector;
    FVector Normal = FVector::ZeroVector;
    FVector Tangent = FVector::ZeroVector;
    if (Active)
    {
        Position = Active->Position; Normal = Active->Normal; Tangent = Active->Tangent;
    }
    else
    {
        double TotalWeight = 0.0;
        for (const FSample& Sample : Samples)
        {
            const double Weight = FMath::Max(Sample.Weight, 1.e-4);
            Position += Sample.Position * Weight; Normal += Sample.Normal * Weight; Tangent += Sample.Tangent * Weight; TotalWeight += Weight;
        }
        Position /= FMath::Max(TotalWeight, 1.e-8);
        Normal.Normalize();
        Tangent = Tangent - Normal * FVector::DotProduct(Tangent, Normal);
        if (!Tangent.Normalize())
        {
            FVector FallbackY;
            Normal.FindBestAxisVectors(Tangent, FallbackY);
        }
    }
    if (!Normal.Normalize()) return FTransform(FQuat::Identity, Position);
    Tangent = Tangent - Normal * FVector::DotProduct(Tangent, Normal);
    if (!Tangent.Normalize())
    {
        FVector FallbackY;
        Normal.FindBestAxisVectors(Tangent, FallbackY);
    }
    FVector Binormal = FVector::CrossProduct(Normal, Tangent).GetSafeNormal();
    Tangent = FVector::CrossProduct(Binormal, Normal).GetSafeNormal();
    if (Binormal.IsNearlyZero() || Tangent.IsNearlyZero()) return FTransform(FQuat::Identity, Position);
    const FMatrix Basis = FRotationMatrix::MakeFromXZ(Tangent, Normal);
    return FTransform(FQuat(Basis), Position);
}

bool FNGonPreviewScene::BeginSoftTransform(const FTransform& GizmoTransform, ENGonSoftTransformMode Mode)
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    Accel.DragStartGizmoTransform = GizmoTransform;
    if (Accel.bBoneGizmoActive && Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex))
    {
        Accel.bBoneDragActive = true;
        Accel.DragStartBoneLocalTransform = Accel.Bones[Accel.SelectedBoneIndex].CurrentLocalTransform;
        Accel.DragStartBoneCompTransform = Accel.Bones[Accel.SelectedBoneIndex].CurrentComponentTransform;
        if (Accel.SkinnedBasePositions.Num() != Positions.Num())
        {
            Accel.SkinnedBasePositions = Positions;
        }
        Accel.CollectSubtreeBonesAndVertices(Accel.SelectedBoneIndex);
        ActiveGizmoTransform = GizmoTransform;
        return true;
    }

    TArray<int32> Seeds;
    if (SelectedElement == ENGonElement::Vertex)
    {
        for (const int32 Id : SelectedIds)
        {
            if (Positions.IsValidIndex(Id))
            {
                Seeds.AddUnique(Id);
            }
        }
    }
    else if (SelectedElement == ENGonElement::Edge)
    {
        for (const int32 EdgeId : SelectedIds)
        {
            if (Topology.Edges.IsValidIndex(EdgeId))
            {
                Seeds.AddUnique(Topology.Edges[EdgeId].X);
                Seeds.AddUnique(Topology.Edges[EdgeId].Y);
            }
        }
    }
    else
    {
        for (const int32 FaceId : SelectedIds)
        {
            if (const FNGonFace* Face = Topology.Faces.Find(FaceId))
            {
                for (const int32 Vertex : Face->Perimeter)
                {
                    Seeds.AddUnique(Vertex);
                }
            }
        }
    }

    ActiveGizmoTransform = GizmoTransform;
    const bool bStarted = SoftSession.Begin(Topology, Positions, Seeds, FTransform::Identity, GizmoTransform, SoftSettings, Mode);
    if (bStarted)
    {
        SoftInfluences = SoftSession.Influences();
        Accel.PreviousSessionVertices.Reset(SoftInfluences.Num());
        for (const FNGonVertexInfluence& Inf : SoftInfluences)
        {
            if (Positions.IsValidIndex(Inf.Vertex))
            {
                Accel.PreviousSessionVertices.Add(Inf.Vertex);
            }
        }
    }
    return bStarted;
}

bool FNGonPreviewScene::ApplyGizmoTransform(const FTransform& GizmoTransform)
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);

    // Path A: Joint / Bone Transform Gizmo Interaction (Translate, Rotate, Scale on Skeletal Bone)
    if (Accel.bBoneDragActive && Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex))
    {
        const FTransform SnappedGizmo = Accel.ApplySnappingToGizmo(Accel.DragStartGizmoTransform, GizmoTransform);
        if (SnappedGizmo.Equals(ActiveGizmoTransform, 1e-6))
        {
            return true;
        }

        const int32 LogicalCount = Positions.Num();
        if (Accel.VertexStamp.Num() < LogicalCount)
        {
            Accel.BuildTopologyTables(Topology, Surface->GetDynamicMesh()->GetMeshRef());
            Accel.BuildColorTables(LogicalCount, ColorElements.Num(), [this](int32 ElemIdx)
            {
                return ColorElements[ElemIdx].LogicalVertex;
            });
        }

        const int32 BoneIdx = Accel.SelectedBoneIndex;
        const FTransform& StartGizmo = Accel.DragStartGizmoTransform;
        const FTransform& StartComp = Accel.DragStartBoneCompTransform;

        // Compute new component-space transform for the selected bone from the gizmo delta
        const FVector DeltaCompPos = SnappedGizmo.GetLocation() - StartGizmo.GetLocation();
        const FVector NewCompPos = StartComp.GetLocation() + DeltaCompPos;

        const FQuat LocalDeltaRot = StartGizmo.GetRotation().Inverse() * SnappedGizmo.GetRotation();
        const FQuat WorldDeltaRot = StartGizmo.GetRotation() * LocalDeltaRot * StartGizmo.GetRotation().Inverse();
        const FQuat NewCompRot = (WorldDeltaRot * StartComp.GetRotation()).GetNormalized();

        const FVector ScaleRatio = SnappedGizmo.GetScale3D();
        const FVector NewLocalScale = Accel.DragStartBoneLocalTransform.GetScale3D() * ScaleRatio;

        const int32 ParentIdx = Accel.Bones[BoneIdx].ParentIndex;
        const FTransform ParentComp = (ParentIdx >= 0 && Accel.Bones.IsValidIndex(ParentIdx))
            ? Accel.Bones[ParentIdx].CurrentComponentTransform
            : FTransform::Identity;

        const FVector NewLocalPos = ParentComp.InverseTransformPosition(NewCompPos);
        const FQuat NewLocalRot = (ParentComp.GetRotation().Inverse() * NewCompRot).GetNormalized();

        Accel.Bones[BoneIdx].CurrentLocalTransform = FTransform(NewLocalRot, NewLocalPos, NewLocalScale);
        Accel.RecomputeBoneHierarchy();

        // Deform only the vertices influenced by the selected bone or its descendant subtree
        if (Accel.SkinnedBasePositions.Num() == LogicalCount && Accel.VertexSkinOffsets.Num() == LogicalCount + 1)
        {
            FDynamicMesh3& Mesh = Surface->GetDynamicMesh()->GetMeshRef();
            for (const int32 LogicalId : Accel.ActiveSubtreeVertices)
            {
                const FVector& BasePos = Accel.SkinnedBasePositions[LogicalId];
                const int32 StartW = Accel.VertexSkinOffsets[LogicalId];
                const int32 EndW = Accel.VertexSkinOffsets[LogicalId + 1];
                FVector BlendedPos = FVector::ZeroVector;
                float TotalWeight = 0.0f;
                for (int32 wIdx = StartW; wIdx < EndW; ++wIdx)
                {
                    const NGon::FNGonVertexBoneWeight& BW = Accel.VertexSkinWeights[wIdx];
                    if (Accel.Bones.IsValidIndex(BW.BoneIndex))
                    {
                        BlendedPos += Accel.Bones[BW.BoneIndex].SkinMatrix.TransformPosition(BasePos) * BW.Weight;
                        TotalWeight += BW.Weight;
                    }
                }
                if (TotalWeight > 1e-5f)
                {
                    Positions[LogicalId] = BlendedPos / TotalWeight;
                }

                const int32 StartDyn = Accel.LogicalToDynamicOffsets[LogicalId];
                const int32 EndDyn = Accel.LogicalToDynamicOffsets[LogicalId + 1];
                for (int32 i = StartDyn; i < EndDyn; ++i)
                {
                    Mesh.SetVertex(Accel.LogicalToDynamicVertices[i], Positions[LogicalId]);
                }
            }

            NotifySurfacePositionsChanged(Mesh);
            Picker.PositionsChanged();

            if (Accel.bShowVertices)
            {
                for (const int32 LogicalId : Accel.ActiveSubtreeVertices)
                {
                    VertexPoints->SetPointPosition(LogicalId, Positions[LogicalId]);
                }
            }
            else
            {
                Accel.bVerticesOverlayDirty = true;
            }

            if (Accel.bShowEdges)
            {
                const int32 Mark = Accel.NextStamp();
                Accel.DirtyEdges.Reset();
                const int32 EdgeCount = Topology.Edges.Num();
                for (const int32 LogicalId : Accel.ActiveSubtreeVertices)
                {
                    const int32 StartE = Accel.LogicalToEdgeOffsets[LogicalId];
                    const int32 EndE = Accel.LogicalToEdgeOffsets[LogicalId + 1];
                    for (int32 i = StartE; i < EndE; ++i)
                    {
                        const int32 EdgeIdx = Accel.LogicalToEdges[i];
                        if (EdgeIdx >= 0 && EdgeIdx < EdgeCount && Accel.EdgeStamp[EdgeIdx] != Mark)
                        {
                            Accel.EdgeStamp[EdgeIdx] = Mark;
                            Accel.DirtyEdges.Add(EdgeIdx);
                        }
                    }
                }
                for (const int32 EdgeIndex : Accel.DirtyEdges)
                {
                    const FIntPoint Edge = Topology.Edges[EdgeIndex];
                    EdgeLines->SetLineStart(EdgeIndex, Positions[Edge.X]);
                    EdgeLines->SetLineEnd(EdgeIndex, Positions[Edge.Y]);
                }
            }
            else
            {
                Accel.bEdgesOverlayDirty = true;
            }

            DrawSelection();
            ++PositionUpdates;
        }

        ActiveGizmoTransform = SnappedGizmo;
        return true;
    }

    // Path B: Mesh Component (Vertex / Edge / Face) Soft Selection & Sculpt Gizmo Interaction
    if (!SoftSession.IsActive())
    {
        return false;
    }
    const FTransform EffectiveGizmo = Accel.ApplySnappingToGizmo(Accel.DragStartGizmoTransform, GizmoTransform);
    if (EffectiveGizmo.Equals(ActiveGizmoTransform, 1e-6))
    {
        return true;
    }
    const int32 LogicalCount = Positions.Num();
    if (Accel.VertexStamp.Num() < LogicalCount)
    {
        Accel.BuildTopologyTables(Topology, Surface->GetDynamicMesh()->GetMeshRef());
        Accel.BuildColorTables(LogicalCount, ColorElements.Num(), [this](int32 ElemIdx)
        {
            return ColorElements[ElemIdx].LogicalVertex;
        });
    }

    const int32 Mark = Accel.NextStamp();
    Accel.DirtyVertices.Reset();
    auto MarkDirtyVertex = [&](int32 Vertex)
    {
        if (Vertex >= 0 && Vertex < LogicalCount && Accel.VertexStamp[Vertex] != Mark)
        {
            Accel.VertexStamp[Vertex] = Mark;
            Accel.DirtyVertices.Add(Vertex);
        }
    };

    for (const int32 PrevV : Accel.PreviousSessionVertices)
    {
        MarkDirtyVertex(PrevV);
    }
    for (const FNGonVertexInfluence& Inf : SoftInfluences)
    {
        MarkDirtyVertex(Inf.Vertex);
    }

    // Update Positions in-place without allocating or copying the entire vertex array.
    if (!SoftSession.BuildPositions(Topology, Positions, EffectiveGizmo, SoftSettings, Positions))
    {
        return false;
    }

    const TArray<FNGonVertexInfluence>& SessionInfluences = SoftSession.Influences();
    for (const FNGonVertexInfluence& Inf : SessionInfluences)
    {
        MarkDirtyVertex(Inf.Vertex);
    }

    // Record currently displaced vertices for the next step.
    Accel.PreviousSessionVertices.Reset(SessionInfluences.Num());
    for (const FNGonVertexInfluence& Inf : SessionInfluences)
    {
        if (Positions.IsValidIndex(Inf.Vertex))
        {
            Accel.PreviousSessionVertices.Add(Inf.Vertex);
        }
    }

    // 1. Update only the displaced dynamic mesh vertices.
    FDynamicMesh3& Mesh = Surface->GetDynamicMesh()->GetMeshRef();
    for (const int32 LogicalId : Accel.DirtyVertices)
    {
        const FVector& Pos = Positions[LogicalId];
        const int32 StartDyn = Accel.LogicalToDynamicOffsets[LogicalId];
        const int32 EndDyn = Accel.LogicalToDynamicOffsets[LogicalId + 1];
        for (int32 i = StartDyn; i < EndDyn; ++i)
        {
            Mesh.SetVertex(Accel.LogicalToDynamicVertices[i], Pos);
        }
    }
    NotifySurfacePositionsChanged(Mesh);
    Picker.PositionsChanged();

    // 2. Update only the affected vertex points and incident edge lines.
    if (Accel.bShowVertices)
    {
        for (const int32 LogicalId : Accel.DirtyVertices)
        {
            VertexPoints->SetPointPosition(LogicalId, Positions[LogicalId]);
        }
    }
    else
    {
        Accel.bVerticesOverlayDirty = true;
    }

    if (Accel.bShowEdges)
    {
        Accel.DirtyEdges.Reset();
        const int32 EdgeCount = Topology.Edges.Num();
        for (const int32 LogicalId : Accel.DirtyVertices)
        {
            const int32 StartE = Accel.LogicalToEdgeOffsets[LogicalId];
            const int32 EndE = Accel.LogicalToEdgeOffsets[LogicalId + 1];
            for (int32 i = StartE; i < EndE; ++i)
            {
                const int32 EdgeIdx = Accel.LogicalToEdges[i];
                if (EdgeIdx >= 0 && EdgeIdx < EdgeCount && Accel.EdgeStamp[EdgeIdx] != Mark)
                {
                    Accel.EdgeStamp[EdgeIdx] = Mark;
                    Accel.DirtyEdges.Add(EdgeIdx);
                }
            }
        }
        for (const int32 EdgeIndex : Accel.DirtyEdges)
        {
            const FIntPoint Edge = Topology.Edges[EdgeIndex];
            EdgeLines->SetLineStart(EdgeIndex, Positions[Edge.X]);
            EdgeLines->SetLineEnd(EdgeIndex, Positions[Edge.Y]);
        }
    }
    else
    {
        Accel.bEdgesOverlayDirty = true;
    }

    DrawSelection();
    ++PositionUpdates;
    ActiveGizmoTransform = EffectiveGizmo;

    // 3. Only update soft selection overlays and vertex colors when needed.
    if (SoftSettings.bEnabled)
    {
        if (SoftTransformMode == ENGonSoftTransformMode::Fixed)
        {
            // In Fixed mode, weights and colors do not change during a drag;
            // only update the positions of existing SoftInfluencePoints.
            if (SoftInfluencePoints && SoftInfluencePoints->IsVisible() &&
                Accel.CachedSoftPointCount == SoftInfluences.Num())
            {
                for (int32 idx = 0; idx < SoftInfluences.Num(); ++idx)
                {
                    const int32 VertId = SoftInfluences[idx].Vertex;
                    if (Positions.IsValidIndex(VertId))
                    {
                        SoftInfluencePoints->SetPointPosition(idx, Positions[VertId]);
                    }
                }
            }
            else
            {
                DrawSoftSelection();
            }
        }
        else
        {
            SoftInfluences = SessionInfluences;
            for (const int32 LogicalId : Accel.DirtyVertices)
            {
                if (SoftHeatSamples.IsValidIndex(LogicalId))
                {
                    SoftHeatSamples[LogicalId] = 0.0f;
                }
            }
            for (const FNGonVertexInfluence& Influence : SoftInfluences)
            {
                if (SoftHeatSamples.IsValidIndex(Influence.Vertex))
                {
                    SoftHeatSamples[Influence.Vertex] = FMath::Max(
                        SoftHeatSamples[Influence.Vertex],
                        FMath::Clamp(static_cast<float>(Influence.Weight), 0.0f, 1.0f));
                }
            }
            DrawSoftSelection();

            if (bHeatmapVisible && !ColorElements.IsEmpty())
            {
                FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
                const bool bUseSoftHeat = !SoftInfluences.IsEmpty();
                for (const int32 LogicalId : Accel.DirtyVertices)
                {
                    const int32 StartC = Accel.LogicalToColorOffsets[LogicalId];
                    const int32 EndC = Accel.LogicalToColorOffsets[LogicalId + 1];
                    for (int32 i = StartC; i < EndC; ++i)
                    {
                        const FColorElement& Element = ColorElements[Accel.LogicalToColorElements[i]];
                        Colors->SetElement(Element.OverlayId, EncodeOverlayColor(Element, bUseSoftHeat));
                    }
                }
                Surface->FastNotifyColorsUpdated();
                ++ColorUpdates;
            }
        }
    }

    return true;
}

void FNGonPreviewScene::EndSoftTransform()
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.bBoneDragActive)
    {
        Accel.bBoneDragActive = false;
        UpdateGizmo();
        return;
    }
    SoftSession.End();
    if (!SelectedIds.IsEmpty())
    {
        GetSceneAccel(this).bBoneGizmoActive = false;
    }
    RebuildSoftSelection();
    UpdateGizmo();
}

void FNGonPreviewScene::CancelSoftTransform()
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.bBoneDragActive)
    {
        Accel.bBoneDragActive = false;
        if (Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex))
        {
            Accel.Bones[Accel.SelectedBoneIndex].CurrentLocalTransform = Accel.DragStartBoneLocalTransform;
            Accel.bPendingSkinningUpdate = true;
        }
        UpdateGizmo();
        return;
    }
    if (!SoftSession.IsActive()) return;
    Positions = SoftSession.OriginalPositions();
    FDynamicMesh3& Mesh = Surface->GetDynamicMesh()->GetMeshRef();
    for (const int32 Vertex : Mesh.VertexIndicesItr())
    {
        const int32 LogicalId = Topology.DynamicToLogicalVertex.IsValidIndex(Vertex) ? Topology.DynamicToLogicalVertex[Vertex] : INDEX_NONE;
        if (Positions.IsValidIndex(LogicalId)) Mesh.SetVertex(Vertex, Positions[LogicalId]);
    }
    NotifySurfacePositionsChanged(Mesh);
    Picker.PositionsChanged();
    UpdateOverlayPositions();
    SoftSession.Cancel();
    if (!SelectedIds.IsEmpty())
    {
        GetSceneAccel(this).bBoneGizmoActive = false;
    }
    RebuildSoftSelection();
    UpdateGizmo();
}

int32 FNGonPreviewScene::Pick(
    ENGonElement Element, const FSceneView& View, const FVector2D& Cursor, bool bAdd)
{
    const int32 Id = Picker.Pick(Element, View, Cursor, Topology, Positions);
    UpdateSelection(Element, Id, bAdd);
    return Id;
}

void FNGonPreviewScene::BoxSelect(
    ENGonElement Element, const FSceneView& View, const FBox2D& ScreenRect, bool bAdd, bool bFullyContained)
{
    TSet<int32> BoxIds;
    Picker.BoxPick(Element, View, ScreenRect, Topology, Positions, BoxIds, bFullyContained);
    ApplySelectionSet(Element, BoxIds, bAdd);
}

void FNGonPreviewScene::ApplySelectionSet(ENGonElement Element, const TSet<int32>& Ids, bool bAdd)
{
    const bool bHadFaces = SelectedElement == ENGonElement::Face && !SelectedIds.IsEmpty();
    const bool bHadOverlaySelection = SelectedElement != ENGonElement::Face && !SelectedIds.IsEmpty();
    if (!bAdd || SelectedElement != Element) SelectedIds.Reset();
    SelectedElement = Element;
    for (const int32 Id : Ids) SelectedIds.Add(Id);
    if (!Ids.IsEmpty()) ActiveSelectedId = *Ids.CreateConstIterator();
    else if (SelectedIds.IsEmpty()) ActiveSelectedId = INDEX_NONE;
    if (!SelectedIds.IsEmpty())
    {
        GetSceneAccel(this).bBoneGizmoActive = false;
    }
    RebuildSoftSelection();
    UpdateGizmo();
    if (bHadFaces || (Element == ENGonElement::Face && !Ids.IsEmpty())) UpdateColors();
    if (bHadOverlaySelection || (Element != ENGonElement::Face && !Ids.IsEmpty())) DrawSelection();
}

bool FNGonPreviewScene::SelectEdgeLoop(int32 StartEdgeId, bool bAdd)
{
    if (!Topology.Edges.IsValidIndex(StartEdgeId)) return false;
    TMap<FIntPoint, int32> EdgeToId;
    EdgeToId.Reserve(Topology.Edges.Num());
    for (int32 EdgeId = 0; EdgeId < Topology.Edges.Num(); ++EdgeId) EdgeToId.Add(Topology.Edges[EdgeId], EdgeId);
    TMap<int32, TArray<int32>> EdgeFaces;
    for (const TPair<int32, FNGonFace>& FacePair : Topology.Faces)
    {
        const TArray<int32>& Perimeter = FacePair.Value.Perimeter;
        if (Perimeter.Num() < 3) continue;
        for (int32 Corner = 0; Corner < Perimeter.Num(); ++Corner)
        {
            const int32 A = Perimeter[Corner], B = Perimeter[(Corner + 1) % Perimeter.Num()];
            if (A == B) continue;
            if (const int32* EdgeId = EdgeToId.Find(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)))) EdgeFaces.FindOrAdd(*EdgeId).Add(FacePair.Key);
        }
    }

    TSet<int32> Loop;
    TArray<int32> Pending{StartEdgeId};
    const int32 Budget = FMath::Max(Topology.Edges.Num() * 2, 1);
    while (!Pending.IsEmpty() && Loop.Num() < Budget)
    {
        const int32 EdgeId = Pending.Pop(EAllowShrinking::No);
        if (!Topology.Edges.IsValidIndex(EdgeId) || Loop.Contains(EdgeId)) continue;
        Loop.Add(EdgeId);
        const TArray<int32>* AdjacentFaces = EdgeFaces.Find(EdgeId);
        if (!AdjacentFaces || AdjacentFaces->Num() != 2) continue;
        for (const int32 FaceId : *AdjacentFaces)
        {
            const FNGonFace* Face = Topology.Faces.Find(FaceId);
            if (!Face || Face->Perimeter.Num() != 4) continue;
            const FIntPoint Edge = Topology.Edges[EdgeId];
            int32 Corner = INDEX_NONE;
            for (int32 I = 0; I < 4; ++I)
            {
                const int32 A = Face->Perimeter[I], B = Face->Perimeter[(I + 1) % 4];
                if (FMath::Min(A, B) == Edge.X && FMath::Max(A, B) == Edge.Y) { Corner = I; break; }
            }
            if (Corner == INDEX_NONE) continue;
            const int32 OppA = Face->Perimeter[(Corner + 2) % 4], OppB = Face->Perimeter[(Corner + 3) % 4];
            if (const int32* Opposite = EdgeToId.Find(FIntPoint(FMath::Min(OppA, OppB), FMath::Max(OppA, OppB)))) Pending.Add(*Opposite);
        }
    }
    if (Loop.Num() >= Budget && !Pending.IsEmpty()) return false;
    ApplySelectionSet(ENGonElement::Edge, Loop, bAdd);
    return !Loop.IsEmpty();
}

void FNGonPreviewScene::ClearSelection()
{
    UpdateSelection(SelectedElement, INDEX_NONE, false);
}

bool FNGonPreviewScene::DeleteSelectedFaces()
{
    if (SelectedElement != ENGonElement::Face || SelectedIds.IsEmpty() ||
        Topology.Faces.Num() <= 1 || Positions.IsEmpty())
    {
        return false;
    }

    TSet<int32> ValidSelectedIds;
    for (const int32 Id : SelectedIds)
    {
        if (Topology.Faces.Contains(Id))
        {
            ValidSelectedIds.Add(Id);
        }
    }
    if (ValidSelectedIds.IsEmpty() || ValidSelectedIds.Num() >= Topology.Faces.Num())
    {
        return false;
    }

    TArray<TArray<int32>> RemainingFaces;
    RemainingFaces.Reserve(Topology.Faces.Num() - ValidSelectedIds.Num());
    for (const TPair<int32, FNGonFace>& FacePair : Topology.Faces)
    {
        if (!ValidSelectedIds.Contains(FacePair.Key) && FacePair.Value.Perimeter.Num() >= 3)
        {
            RemainingFaces.Add(FacePair.Value.Perimeter);
        }
    }
    if (RemainingFaces.IsEmpty()) return false;

    FNGonMeshData Replacement;
    FText Error;
    if (!NGon::BuildPolygonMesh(Positions, RemainingFaces, Replacement, Error)) return false;

    const FNGonSoftSelectionSettings SavedSoftSettings = SoftSettings;
    const ENGonSoftTransformMode SavedSoftMode = SoftTransformMode;
    const float SavedHeatBias = HeatBias;

    // BuildPolygonMesh may compact logical vertices after the deleted faces are
    // removed. The rebuilt SourceVertexIds point back to the old logical
    // position array, so carry imported FBX samples by that old index.
    Replacement.ImportedHeatSamples.Init(-1.0f, Replacement.Topology.RestPositions.Num());
    for (int32 NewVertex = 0; NewVertex < Replacement.Topology.SourceVertexIds.Num(); ++NewVertex)
    {
        const int32 OldVertex = Replacement.Topology.SourceVertexIds[NewVertex];
        if (ImportedHeatMask.IsValidIndex(OldVertex) && ImportedHeatMask[OldVertex] &&
            HeatSamples.IsValidIndex(OldVertex))
        {
            Replacement.ImportedHeatSamples[NewVertex] = HeatSamples[OldVertex];
        }
    }

    SetMesh(MoveTemp(Replacement));
    SoftTransformMode = SavedSoftMode;
    SoftSettings = SavedSoftSettings;
    HeatBias = SavedHeatBias;
    UpdateColors();
    RebuildSoftSelection();
    return true;
}

void FNGonPreviewScene::UpdateSelection(ENGonElement Element, int32 Id, bool bAdd)
{
    if (Id == INDEX_NONE && (bAdd || SelectedIds.IsEmpty()))
    {
        return;
    }
    if (SelectedElement == Element && SelectedIds.Contains(Id) && (bAdd || SelectedIds.Num() == 1))
    {
        return;
    }

    const bool bHadFaces = SelectedElement == ENGonElement::Face && !SelectedIds.IsEmpty();
    const bool bHadOverlaySelection = SelectedElement != ENGonElement::Face && !SelectedIds.IsEmpty();

    if (!bAdd || SelectedElement != Element)
    {
        SelectedIds.Reset();
    }
    SelectedElement = Element;
    if (Id != INDEX_NONE)
    {
        SelectedIds.Add(Id);
        ActiveSelectedId = Id;
    }
    else if (SelectedIds.IsEmpty())
    {
        ActiveSelectedId = INDEX_NONE;
    }

    if (!SelectedIds.IsEmpty())
    {
        GetSceneAccel(this).bBoneGizmoActive = false;
    }
    RebuildSoftSelection();
    UpdateGizmo();
    if (bHadFaces || (Element == ENGonElement::Face && Id != INDEX_NONE))
    {
        UpdateColors();
    }
    if (bHadOverlaySelection || (Element != ENGonElement::Face && Id != INDEX_NONE))
    {
        DrawSelection();
    }
}

void FNGonPreviewScene::DrawSelection()
{
    SelectedEdge->Clear();
    SelectedVertex->Clear();

    if (SelectedElement == ENGonElement::Edge && !SelectedIds.IsEmpty())
    {
        TArray<FVector> Starts;
        TArray<FVector> Ends;
        Starts.Reserve(SelectedIds.Num());
        Ends.Reserve(SelectedIds.Num());
        for (const int32 Id : SelectedIds)
        {
            if (!Topology.Edges.IsValidIndex(Id)) continue;
            const FIntPoint Edge = Topology.Edges[Id];
            if (!Positions.IsValidIndex(Edge.X) || !Positions.IsValidIndex(Edge.Y)) continue;
            Starts.Add(Positions[Edge.X]);
            Ends.Add(Positions[Edge.Y]);
        }
        SelectedEdge->AddLines(Starts, Ends, SelectionColor, 3.5f, 1.2f);
    }

    if (SelectedElement == ENGonElement::Vertex && !SelectedIds.IsEmpty())
    {
        TArray<FVector> Points;
        Points.Reserve(SelectedIds.Num());
        for (const int32 Id : SelectedIds)
        {
            if (Positions.IsValidIndex(Id)) Points.Add(Positions[Id]);
        }
        SelectedVertex->AddPoints(Points, SelectionColor, 9.0f, 1.5f);
    }
}

FText FNGonPreviewScene::DescribeSelection() const
{
    const FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.bBoneGizmoActive && Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex))
    {
        const FNGonSceneAccel::FSceneBoneState& Bone = Accel.Bones[Accel.SelectedBoneIndex];
        const FVector Loc = Bone.CurrentLocalTransform.GetLocation();
        const FRotator Rot = Bone.CurrentLocalTransform.GetRotation().Rotator();
        return FText::Format(
            LOCTEXT("BoneSelectionHud", "Joint: {0} (#{1})  |  Loc ({2}, {3}, {4})  |  Rot ({5}°, {6}°, {7}°)"),
            FText::FromName(Bone.Name),
            FText::AsNumber(Bone.BoneIndex),
            FText::AsNumber(FMath::RoundToFloat(Loc.X * 10.0f) / 10.0f),
            FText::AsNumber(FMath::RoundToFloat(Loc.Y * 10.0f) / 10.0f),
            FText::AsNumber(FMath::RoundToFloat(Loc.Z * 10.0f) / 10.0f),
            FText::AsNumber(FMath::RoundToFloat(Rot.Roll * 10.0f) / 10.0f),
            FText::AsNumber(FMath::RoundToFloat(Rot.Pitch * 10.0f) / 10.0f),
            FText::AsNumber(FMath::RoundToFloat(Rot.Yaw * 10.0f) / 10.0f));
    }
    if (SelectedIds.IsEmpty())
    {
        return LOCTEXT("NoSelection", "No selection");
    }

    if (SelectedIds.Num() > 1)
    {
        switch (SelectedElement)
        {
        case ENGonElement::Face:
            return FText::Format(LOCTEXT("SelectedFaces", "{0} faces selected"), FText::AsNumber(SelectedIds.Num()));
        case ENGonElement::Edge:
            return FText::Format(LOCTEXT("SelectedEdges", "{0} edges selected"), FText::AsNumber(SelectedIds.Num()));
        case ENGonElement::Vertex:
            return FText::Format(LOCTEXT("SelectedVertices", "{0} vertices selected"), FText::AsNumber(SelectedIds.Num()));
        }
    }

    const int32 Id = *SelectedIds.CreateConstIterator();
    switch (SelectedElement)
    {
    case ENGonElement::Face:
        if (const FNGonFace* Face = Topology.Faces.Find(Id))
        {
            return FText::Format(LOCTEXT("FaceInfo", "Face {0}     {1} sides     {2} triangles"),
                FText::AsNumber(Id), FText::AsNumber(Face->Perimeter.Num()),
                FText::AsNumber(Face->Triangles.Num()));
        }
        break;
    case ENGonElement::Edge:
        if (Topology.Edges.IsValidIndex(Id))
        {
            const FIntPoint Edge = Topology.Edges[Id];
            return FText::Format(LOCTEXT("EdgeInfo", "Edge     vertices {0} and {1}"),
                FText::AsNumber(Topology.SourceVertexIds[Edge.X]),
                FText::AsNumber(Topology.SourceVertexIds[Edge.Y]));
        }
        break;
    case ENGonElement::Vertex:
        if (Topology.SourceVertexIds.IsValidIndex(Id))
        {
            const float Influence = (SoftSettings.bEnabled && SoftHeatSamples.IsValidIndex(Id))
                ? SoftHeatSamples[Id]
                : (SelectedIds.Contains(Id) ? 1.0f : 0.0f);
            return FText::Format(LOCTEXT("VertexInfo", "Vertex {0}     soft influence {1}"),
                FText::AsNumber(Topology.SourceVertexIds[Id]), FText::AsNumber(Influence));
        }
        break;
    }
    return LOCTEXT("NoSelection", "No selection");
}

FText FNGonPreviewScene::DescribeMesh() const
{
    const FNGonSceneAccel& Accel = GetSceneAccel(this);
    const FText MeshInfo = FText::Format(LOCTEXT("MeshInfo", "{0} faces     {1} triangles     {2} edges     {3} vertices     {4} N-Gons (max {5} sides)"),
        FText::AsNumber(Topology.Faces.Num()), FText::AsNumber(Topology.TriangleCount),
        FText::AsNumber(Topology.Edges.Num()), FText::AsNumber(Topology.RestPositions.Num()),
        FText::AsNumber(Accel.CachedNGonCount), FText::AsNumber(Topology.MaxFaceCorners));
    if (SoftSettings.bEnabled && !SelectedIds.IsEmpty())
    {
        return FText::Format(LOCTEXT("SoftHeatmapStatus", "{0}  |  Soft heatmap follows selection influence"), MeshInfo);
    }
    return MeshInfo;
}

FText FNGonPreviewScene::DescribeUpdates() const
{
    return FText::Format(LOCTEXT("Updates", "Updates   Topology {0}    Positions {1}    Colors {2}"),
        FText::AsNumber(TopologyUpdates), FText::AsNumber(PositionUpdates), FText::AsNumber(ColorUpdates));
}



#undef LOCTEXT_NAMESPACE

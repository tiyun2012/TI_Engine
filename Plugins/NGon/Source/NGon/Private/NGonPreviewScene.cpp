#include "NGonPreviewScene.h"
#include "NGonSceneSkeleton.h"
#include "NGonMeshAPI.h"

#include "Algo/AnyOf.h"
#include "Components/DynamicMeshComponent.h"
#include "Drawing/LineSetComponent.h"
#include "Drawing/PointSetComponent.h"
#include "Drawing/PreviewGeometryActor.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "Engine/Texture2D.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SceneInterface.h"
#include "SceneView.h"
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
const FColor PointColor(178, 92, 255);            // Default purple vertex color
const FColor SelectionColor(255, 181, 65);
const FColor SelectedVertexColor(255, 232, 48);   // Bright yellow selected vertex color

constexpr float NGonEdgeThickness = 1.6f;
// 5/4 relative to rendered edge width at near camera, shrinking smoothly to 2/3 when far/zoomed out
constexpr float NGonVertexNearSize = NGonEdgeThickness * (5.0f / 4.0f) * 2.0f;       // 4.0 px near -> 2.67 px far
constexpr float NGonSelectedVertexNearSize = NGonVertexNearSize * (5.0f / 4.0f);     // 5.0 px near -> 3.33 px far
constexpr float NGonVertexDepthBias = 1.4f;
constexpr float NGonSelectedVertexDepthBias = 2.2f;
const FName PaintTextureParameter(TEXT("PaintTexture"));

const FLinearColor StudioColor(0.46f, 0.49f, 0.53f);
TMap<const FNGonPreviewScene*, TStrongObjectPtr<UMaterialInstanceDynamic>> GFaceHighlightMaterialMap;
TStrongObjectPtr<UMaterial> GXRayBaseMaterial;
TMap<const FNGonPreviewScene*, TStrongObjectPtr<UMaterialInstanceDynamic>> GXRayMaterialMap;

struct FNGonDisplayModeState
{
    bool bShowSoftVertexWeights = true;
    bool bXRayEnabled = false;
    float XRayOpacity = 0.35f;
    bool bWireframeEdgesOnly = false;
};
TMap<const FNGonPreviewScene*, FNGonDisplayModeState> GDisplayModeStateMap;

FNGonDisplayModeState& GetDisplayState(const FNGonPreviewScene* Scene)
{
    return GDisplayModeStateMap.FindOrAdd(Scene);
}

UMaterial* MakeXRayMaterial()
{
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
    Material->BlendMode = BLEND_Translucent;
    Material->SetShadingModel(MSM_Unlit);
    Material->TwoSided = true;

    auto* OpacityParam = CastChecked<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionScalarParameter::StaticClass(), -320, 120));
    OpacityParam->ParameterName = TEXT("XRayOpacity");
    OpacityParam->DefaultValue = 0.35f;

    auto* Shade = CastChecked<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionCustom::StaticClass(), -320, -60));
    Shade->OutputType = CMOT_Float3;
    Shade->Inputs.Reset();
    Shade->Code = TEXT(
        "float3 N = normalize(Parameters.WorldNormal);\n"
        "float3 V = normalize(Parameters.CameraVector);\n"
        "float ndotv = abs(dot(N, V));\n"
        "float rim = pow(1.0 - ndotv, 1.8) * 0.25;\n"
        "float lum = 0.34 + 0.36 * ndotv + rim;\n"
        "return float3(0.56, 0.64, 0.76) * lum;");

    Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Shade);
    Material->GetEditorOnlyData()->Opacity.Connect(0, OpacityParam);
    Material->PostEditChange();
    return Material;
}

FLinearColor HeatColor(float Alpha)
{
    const FLinearColor Cold(0.025f, 0.11f, 0.85f);
    const FLinearColor Mid(0.055f, 0.76f, 0.34f);
    const FLinearColor Hot(0.98f, 0.08f, 0.018f);
    const float T = FMath::Clamp(Alpha, 0.0f, 1.0f) * 2.0f;
    return T < 1.0f ? FMath::Lerp(Cold, Mid, T) : FMath::Lerp(Mid, Hot, T - 1.0f);
}

UMaterial* MakeStudioMaterial(bool bUseVertexColor)
{
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
    Material->SetShadingModel(MSM_DefaultLit);
    Material->TwoSided = true;

    if (bUseVertexColor)
    {
        auto* VertColorExpr = CastChecked<UMaterialExpressionVertexColor>(UMaterialEditingLibrary::CreateMaterialExpression(
            Material, UMaterialExpressionVertexColor::StaticClass(), -320, 0));
        Material->GetEditorOnlyData()->BaseColor.Connect(0, VertColorExpr);
        Material->GetEditorOnlyData()->EmissiveColor.Connect(0, VertColorExpr);
        Material->PostEditChange();
        return Material;
    }

    auto* Shade = CastChecked<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionCustom::StaticClass(), -320, 0));
    Shade->OutputType = CMOT_Float3;
    Shade->Inputs.Reset();

    if (false)
    {
    }
    else
    {
        auto* AlbedoParam = CastChecked<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(
            Material, UMaterialExpressionVectorParameter::StaticClass(), -540, 0));
        AlbedoParam->ParameterName = TEXT("BaseAlbedo");
        AlbedoParam->DefaultValue = FLinearColor(0.54f, 0.57f, 0.61f, 1.0f);

        Shade->Inputs.SetNum(1);
        Shade->Inputs[0].InputName = TEXT("BaseAlbedo");
        Shade->Inputs[0].Input.Connect(0, AlbedoParam);
        Shade->Code = TEXT(
            "float3 N = normalize(Parameters.WorldNormal);\n"
            "float3 V = normalize(Parameters.CameraVector);\n"
            "float3 R = normalize(cross(V, float3(0.0, 0.0, 1.0)) + float3(1e-5, 0.0, 0.0));\n"
            "float3 U = normalize(cross(R, V));\n"
            "float3 Lkey = normalize(V * 0.45 + R * 0.42 + U * 0.62);\n"
            "float3 Lfill = normalize(V * 0.35 - R * 0.60 - U * 0.25);\n"
            "float ndotlKey = saturate(dot(N, Lkey));\n"
            "float ndotlFill = saturate(dot(N, Lfill));\n"
            "float ndotv = abs(dot(N, V));\n"
            "float hemi = saturate(N.z * 0.5 + 0.5);\n"
            "float3 H = normalize(Lkey + V);\n"
            "float spec = pow(saturate(dot(N, H)), 28.0) * 0.20;\n"
            "float lighting = 0.28 + 0.48 * ndotlKey + 0.20 * ndotlFill + 0.14 * hemi + 0.18 * ndotv;\n"
            "return BaseAlbedo * lighting + float3(spec, spec, spec);");
    }

    Material->GetEditorOnlyData()->BaseColor.Connect(0, Shade);
    Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Shade);
    Material->PostEditChange();
    return Material;
}

UMaterial* MakePaintMaterial(UTexture2D* PaintTexture)
{
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
    Material->SetShadingModel(MSM_DefaultLit);
    Material->TwoSided = true;

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

    auto* Base = CastChecked<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionConstant3Vector::StaticClass(), -400, 0));
    Base->Constant = StudioColor;
    auto* Composite = CastChecked<UMaterialExpressionLinearInterpolate>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionLinearInterpolate::StaticClass(), -160, 0));
    Composite->A.Connect(0, Base);
    Composite->B.Connect(0, PaintSample);
    Composite->Alpha.Connect(4, PaintSample);

    auto* Shade = CastChecked<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionCustom::StaticClass(), -40, 0));
    Shade->OutputType = CMOT_Float3;
    Shade->Inputs.SetNum(1);
    Shade->Inputs[0].InputName = TEXT("Albedo");
    Shade->Inputs[0].Input.Connect(0, Composite);
    Shade->Code = TEXT(
        "float3 N = normalize(Parameters.WorldNormal);\n"
        "float3 V = normalize(Parameters.CameraVector);\n"
        "float3 R = normalize(cross(V, float3(0.0, 0.0, 1.0)) + float3(1e-5, 0.0, 0.0));\n"
        "float3 U = normalize(cross(R, V));\n"
        "float3 Lkey = normalize(V * 0.45 + R * 0.42 + U * 0.62);\n"
        "float3 Lfill = normalize(V * 0.35 - R * 0.60 - U * 0.25);\n"
        "float ndotlKey = saturate(dot(N, Lkey));\n"
        "float ndotlFill = saturate(dot(N, Lfill));\n"
        "float ndotv = abs(dot(N, V));\n"
        "float hemi = saturate(N.z * 0.5 + 0.5);\n"
        "float lighting = 0.30 + 0.46 * ndotlKey + 0.20 * ndotlFill + 0.14 * hemi + 0.16 * ndotv;\n"
        "return Albedo * lighting;");

    Material->GetEditorOnlyData()->BaseColor.Connect(0, Shade);
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
    : Scene(FPreviewScene::ConstructionValues()
        .SetCreatePhysicsScene(false)
        .SetLightBrightness(4.5f)
        .SetSkyBrightness(2.0f))
{
    GetSceneAccel(this);
    StudioMaterial.Reset(MakeStudioMaterial(false));
    ColorMaterial.Reset(MakeStudioMaterial(true));

    Surface = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
    Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Surface->SetCastShadow(false);
    Surface->SetEnableFlatShading(bFlatShading);
    Surface->SetVertexColorSpaceTransformMode(EDynamicMeshVertexColorTransformMode::NoTransform);
    Surface->SetTangentsType(EDynamicMeshComponentTangentsMode::AutoCalculated);
    Surface->SetMaterial(0, StudioMaterial.IsValid() ? StudioMaterial.Get() : UMaterial::GetDefaultMaterial(MD_Surface));
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
    GFaceHighlightMaterialMap.Remove(this);
    FNGonMeshAPI::ClearHistory(this);
    GFaceHighlightMaterialMap.Remove(this);
    GXRayMaterialMap.Remove(this);
    GDisplayModeStateMap.Remove(this);
    NGon::RemoveSceneAccel(this);
    Picker.SetMesh(nullptr);
    Overlay->Disconnect();
    Scene.RemoveComponent(Surface);
}

bool FNGonPreviewScene::HasGizmoTarget() const
{
    const FNGonSceneAccel& Accel = GetSceneAccel(this);
    const bool bHasBone = Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex) && (Accel.bBoneGizmoActive || SelectedIds.IsEmpty());
    return GizmoMode != ENGonGizmoMode::None && (bHasBone || !SelectedIds.IsEmpty());
}

void FNGonPreviewScene::RefreshBoneDeformation()
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
            Accel.RecomputeAllVertexNormals(Positions);
            UpdateOverlayPositions();
            DrawSelection();
            ++PositionUpdates;
        }
    }
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

void FNGonPreviewScene::SetPreviewMaterials(UInteractiveToolManager* ToolManager)
{
    if (!StudioMaterial.IsValid())
    {
        StudioMaterial.Reset(MakeStudioMaterial(false));
    }
    if (!ColorMaterial.IsValid())
    {
        ColorMaterial.Reset(MakeStudioMaterial(true));
    }
    if (!ToolManager)
    {
        return;
    }
    TStrongObjectPtr<UMaterialInstanceDynamic>& HighlightRef = GFaceHighlightMaterialMap.FindOrAdd(this);
    if (!HighlightRef.IsValid() && StudioMaterial.IsValid())
    {
        if (UMaterialInstanceDynamic* HighlightMID = UMaterialInstanceDynamic::Create(StudioMaterial.Get(), GetTransientPackage()))
        {
            HighlightMID->SetVectorParameterValue(TEXT("BaseAlbedo"), FLinearColor(0.84f, 0.58f, 0.24f, 1.0f));
            HighlightRef.Reset(HighlightMID);
        }
    }
    if (HighlightRef.IsValid())
    {
        Surface->SetSecondaryRenderMaterial(HighlightRef.Get());
        Surface->EnableSecondaryTriangleBuffers([this](const FDynamicMesh3* Mesh, int32 TriangleId)
        {
            return SelectedElement == ENGonElement::Face && SelectedIds.Contains(Mesh->GetTriangleGroup(TriangleId));
        });
        bSecondaryFaceHighlight = true;
    }

    if (UMaterialInterface* LineMaterial = ToolSetupUtil::GetDefaultLineComponentMaterial(ToolManager, true))
    {
        Overlay->SetAllLineSetsMaterial(LineMaterial);
    }
    if (UMaterialInterface* PointMaterial = ToolSetupUtil::GetDefaultPointComponentMaterial(ToolManager, true))
    {
        Overlay->SetAllPointSetsMaterial(PointMaterial);
    }
}

void FNGonPreviewScene::SetMesh(FNGonMeshData&& Data, TArray<UMaterialInterface*> Materials)
{
    bHeatmapVisible = false;
    ClearPaintTexture();
    UpdateSurfaceDisplay(false);
    Picker.SetMesh(nullptr);
    SelectedElement = ENGonElement::Face;
    SelectedIds.Reset();
    ActiveSelectedId = INDEX_NONE;
    SoftSettings = FNGonSoftSelectionSettings{};
    SoftTransformMode = ENGonSoftTransformMode::Fixed;
    SoftSession.Cancel();
    SoftSolver.Reset();
    SoftInfluences.Reset();
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
    Accel.bHasCameraView = false;
    Accel.LastVertexSizeUpdateFrame = 0;
    Accel.BuildTopologyTables(Topology, Data.Mesh);
    Accel.BuildColorTables(Positions.Num(), ColorElements.Num(), [this](int32 ElemIdx)
    {
        return ColorElements[ElemIdx].LogicalVertex;
    });

    Surface->SetTwoSided(Materials.IsEmpty());
    Surface->SetMesh(MoveTemp(Data.Mesh));
    UMaterialInterface* Neutral = StudioMaterial.IsValid() ? StudioMaterial.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
    if (Materials.IsEmpty())
    {
        Materials.Add(Neutral);
    }
    for (UMaterialInterface*& Material : Materials)
    {
        if (!Material || Material == UMaterial::GetDefaultMaterial(MD_Surface))
        {
            Material = Neutral;
        }
    }
    Surface->ConfigureMaterialSet(Materials, true);
    Picker.SetMesh(Surface->GetDynamicMesh()->GetMeshPtr());
    RebuildOverlays();
    ++TopologyUpdates;
}

void FNGonPreviewScene::InitializeColorOverlay(FDynamicMesh3& Mesh)
{
    if (!Mesh.HasAttributes())
    {
        Mesh.EnableAttributes();
    }

    FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
    const FDynamicMeshUVOverlay* UVs = Attributes->PrimaryUV();
    const FDynamicMeshColorOverlay* Previous = Attributes->PrimaryColors();
    ColorElements.Reset(Mesh.TriangleCount() * 3);
    for (const int32 TriangleId : Mesh.TriangleIndicesItr())
    {
        const FIndex3i Vertices = Mesh.GetTriangle(TriangleId);
        const bool bHasColor = Previous && Previous->IsSetTriangle(TriangleId);
        const bool bHasUV = UVs && UVs->IsSetTriangle(TriangleId);
        const FIndex3i ColorIds = bHasColor ? Previous->GetTriangle(TriangleId) : FIndex3i(-1, -1, -1);
        const FIndex3i UVIds = bHasUV ? UVs->GetTriangle(TriangleId) : FIndex3i(-1, -1, -1);
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const FVector4f Color = bHasColor ? Previous->GetElement(ColorIds[Corner]) : FVector4f(1, 1, 1, 1);
            const FVector2f UV = bHasUV ? UVs->GetElement(UVIds[Corner]) : FVector2f::ZeroVector;
            ColorElements.Add({ INDEX_NONE, Topology.DynamicToLogicalVertex[Vertices[Corner]], Color, UV });
        }
    }

    Attributes->DisablePrimaryColors();
    Attributes->EnablePrimaryColors();
    FDynamicMeshColorOverlay* Colors = Attributes->PrimaryColors();
    int32 ElementIndex = 0;
    for (const int32 TriangleId : Mesh.TriangleIndicesItr())
    {
        FIndex3i ColorIds;
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            FColorElement& Element = ColorElements[ElementIndex++];
            Element.OverlayId = Colors->AppendElement(Element.SourceColor);
            ColorIds[Corner] = Element.OverlayId;
        }
        Colors->SetTriangle(TriangleId, ColorIds);
    }
}

FVector4f FNGonPreviewScene::GetOverlayColor(const FColorElement& Element) const
{
    if (SurfaceDisplay == ESurfaceDisplay::Heatmap)
    {
        const float Sample = SoftHeatSamples.IsValidIndex(Element.LogicalVertex) ? SoftHeatSamples[Element.LogicalVertex] : 0.0f;
        const FLinearColor Color = HeatColor(Sample);
        return FVector4f(Color.R, Color.G, Color.B, 1.0f);
    }
    if (SurfaceDisplay == ESurfaceDisplay::PaintColors)
    {
        const int32 X = FMath::RoundToInt(FMath::Clamp(Element.UV.X, 0.0f, 1.0f) * (PaintWidth - 1));
        const int32 Y = FMath::RoundToInt((1.0f - FMath::Clamp(Element.UV.Y, 0.0f, 1.0f)) * (PaintHeight - 1));
        const FLinearColor Paint(PaintPixels[Y * PaintWidth + X]);
        const FLinearColor Composite = FMath::Lerp(StudioColor, Paint, Paint.A);
        return FVector4f(Composite.R, Composite.G, Composite.B, 1.0f);
    }
    return Element.SourceColor;
}

void FNGonPreviewScene::WriteColors(FDynamicMesh3& Mesh) const
{
    FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
    for (const FColorElement& Element : ColorElements)
    {
        Colors->SetElement(Element.OverlayId, GetOverlayColor(Element));
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

bool FNGonPreviewScene::IsPaintMaterialReady()
{
    FSceneInterface* RenderScene = Scene.GetScene();
    FMaterialResource* Resource = PaintMaterial.IsValid() && RenderScene
        ? PaintMaterial->GetMaterialResource(RenderScene->GetShaderPlatform(), EMaterialQualityLevel::Num)
        : nullptr;
    return Resource && Resource->HasValidGameThreadShaderMap();
}

void FNGonPreviewScene::UpdateSurfaceDisplay(bool bColorsChanged)
{
    const bool bShowHeat = bHeatmapVisible && HasSoftSelectionInfluence();
    ESurfaceDisplay Desired = ESurfaceDisplay::Source;
    if (bShowHeat)
    {
        Desired = ESurfaceDisplay::Heatmap;
    }
    else if (!PaintPixels.IsEmpty())
    {
        Desired = IsPaintMaterialReady() ? ESurfaceDisplay::PaintTexture : ESurfaceDisplay::PaintColors;
    }

    const bool bModeChanged = SurfaceDisplay != Desired;
    SurfaceDisplay = Desired;
    const bool bVertexColors = Desired == ESurfaceDisplay::Heatmap || Desired == ESurfaceDisplay::PaintColors;
    if (!ColorElements.IsEmpty() && (bModeChanged || (bVertexColors && bColorsChanged)))
    {
        WriteColors(Surface->GetDynamicMesh()->GetMeshRef());
        Surface->FastNotifyColorsUpdated();
        ++ColorUpdates;
    }
    const FNGonDisplayModeState& DispState = GetDisplayState(this);
    if (Surface)
    {
        Surface->SetVisibility(!DispState.bWireframeEdgesOnly);
    }
    if (DispState.bXRayEnabled && !DispState.bWireframeEdgesOnly)
    {
        if (!GXRayBaseMaterial.IsValid())
        {
            GXRayBaseMaterial.Reset(MakeXRayMaterial());
        }
        TStrongObjectPtr<UMaterialInstanceDynamic>& XRayRef = GXRayMaterialMap.FindOrAdd(this);
        if (!XRayRef.IsValid() && GXRayBaseMaterial.IsValid())
        {
            XRayRef.Reset(UMaterialInstanceDynamic::Create(GXRayBaseMaterial.Get(), GetTransientPackage()));
        }
        if (XRayRef.IsValid())
        {
            XRayRef->SetScalarParameterValue(TEXT("XRayOpacity"), FMath::Clamp(DispState.XRayOpacity, 0.0f, 1.0f));
            Surface->SetOverrideRenderMaterial(XRayRef.Get());
        }
    }
    else if (bModeChanged || !DispState.bXRayEnabled)
    {
        UMaterialInterface* Override = bVertexColors ? ColorMaterial.Get() : nullptr;
        if (Desired == ESurfaceDisplay::PaintTexture)
        {
            Override = PaintMaterial.Get();
        }
        if (Override)
        {
            Surface->SetOverrideRenderMaterial(Override);
        }
        else
        {
            Surface->ClearOverrideRenderMaterial();
        }
    }
}

void FNGonPreviewScene::RefreshPaintMaterial()
{
    if (SurfaceDisplay == ESurfaceDisplay::PaintColors && IsPaintMaterialReady())
    {
        UpdateSurfaceDisplay(false);
    }
}

void FNGonPreviewScene::UpdateFaceHighlight()
{
    if (bSecondaryFaceHighlight)
    {
        Surface->FastNotifySecondaryTrianglesChanged();
    }
}

void FNGonPreviewScene::RebuildOverlays()
{
    EdgeLines->Clear();
    VertexPoints->Clear();

    FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.LogicalVertexNormals.Num() != Positions.Num() || Accel.DisplacedVertexPositions.Num() != Positions.Num())
    {
        Accel.RecomputeAllVertexNormals(Positions);
    }

    TArray<FVector> Starts;
    TArray<FVector> Ends;
    Starts.Reserve(Topology.Edges.Num());
    Ends.Reserve(Topology.Edges.Num());
    for (const FIntPoint& Edge : Topology.Edges)
    {
        Starts.Add(Accel.GetDisplacedVertexPosition(Positions, Edge.X, 0.25f));
        Ends.Add(Accel.GetDisplacedVertexPosition(Positions, Edge.Y, 0.25f));
    }

    EdgeLines->AddLines(Starts, Ends, EdgeColor, NGonEdgeThickness, 0.35f);
    const int32 NumVerts = Accel.DisplacedVertexPositions.Num();
    for (int32 v = 0; v < NumVerts; ++v)
    {
        const float Scale = Accel.GetVertexCameraDepthScale(Positions[v]);
        VertexPoints->AddPoint(Accel.DisplacedVertexPositions[v], PointColor, NGonVertexNearSize * Scale, NGonVertexDepthBias);
    }
    Accel.bEdgesOverlayDirty = false;
    Accel.bVerticesOverlayDirty = false;
    DrawSelection();
    DrawSoftSelection();
}

void FNGonPreviewScene::UpdateCameraVertexScales(const FVector& CameraOrigin, const FVector& CameraDir)
{
    if (Positions.IsEmpty() || SoftSession.IsActive())
    {
        return;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.bBoneDragActive)
    {
        return;
    }

    const FBox Bounds = Topology.RestBounds.IsValid ? Topology.RestBounds : GetMeshBounds();
    const double MeshRadius = FMath::Max(Bounds.GetExtent().Size(), 10.0);
    const double MoveThresholdSq = FMath::Square(FMath::Max(MeshRadius * 0.035, 1.5));
    if (Accel.bHasCameraView && FVector::DistSquared(CameraOrigin, Accel.LastCameraOrigin) < MoveThresholdSq)
    {
        return;
    }

    Accel.LastCameraOrigin = CameraOrigin;
    Accel.LastCameraDir = CameraDir.GetSafeNormal();
    Accel.bHasCameraView = true;

    // Compute near and far vertex distances from camera using a fast strided sample
    const int32 NumVerts = Positions.Num();
    const int32 Step = FMath::Max(1, NumVerts / 512);
    float MinDist = TNumericLimits<float>::Max();
    float MaxDist = 0.0f;
    for (int32 i = 0; i < NumVerts; i += Step)
    {
        const float D = static_cast<float>(FVector::Distance(Positions[i], CameraOrigin));
        MinDist = FMath::Min(MinDist, D);
        MaxDist = FMath::Max(MaxDist, D);
    }
    if (MinDist >= MaxDist)
    {
        MinDist = FMath::Max(1.0f, static_cast<float>(FVector::Distance(Bounds.GetCenter(), CameraOrigin) - MeshRadius));
        MaxDist = static_cast<float>(FVector::Distance(Bounds.GetCenter(), CameraOrigin) + MeshRadius);
    }
    Accel.NearCameraDist = MinDist;
    Accel.FarCameraDist = MaxDist;

    // Scale smoothly from 1.0x (close) down to 2/3 (zoomed out / far from mesh)
    const float CamCenterDist = static_cast<float>(FVector::Distance(Bounds.GetCenter(), CameraOrigin));
    Accel.CameraZoomScale = FMath::GetMappedRangeValueClamped(
        FVector2f(static_cast<float>(MeshRadius * 1.0), static_cast<float>(MeshRadius * 4.2)),
        FVector2f(1.0f, 2.0f / 3.0f),
        CamCenterDist);

    // Rebuild VertexPoints sizes when visible (throttled on very high-poly meshes during rapid camera motion)
    if (Accel.bShowVertices && VertexPoints && (NumVerts <= 15000 || Accel.LastVertexSizeUpdateFrame == 0 || (GFrameCounter - Accel.LastVertexSizeUpdateFrame) >= 6))
    {
        Accel.LastVertexSizeUpdateFrame = GFrameCounter;
        if (Accel.DisplacedVertexPositions.Num() != NumVerts)
        {
            Accel.RecomputeAllVertexNormals(Positions);
        }
        VertexPoints->Clear();
        for (int32 v = 0; v < NumVerts; ++v)
        {
            const float Scale = Accel.GetVertexCameraDepthScale(Positions[v]);
            VertexPoints->AddPoint(Accel.DisplacedVertexPositions[v], PointColor, NGonVertexNearSize * Scale, NGonVertexDepthBias);
        }
    }

    if (SelectedElement == ENGonElement::Vertex && !SelectedIds.IsEmpty())
    {
        Accel.CachedSelectedVertexCount = 0;
        DrawSelection();
    }
    if (SoftSettings.bEnabled && !SoftInfluences.IsEmpty())
    {
        DrawSoftSelection();
    }
}

void FNGonPreviewScene::UpdateOverlayPositions()
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.LogicalVertexNormals.Num() != Positions.Num() || Accel.DisplacedVertexPositions.Num() != Positions.Num())
    {
        Accel.RecomputeAllVertexNormals(Positions);
    }

    if (Accel.bShowEdges)
    {
        for (int32 EdgeIndex = 0; EdgeIndex < Topology.Edges.Num(); ++EdgeIndex)
        {
            const FIntPoint Edge = Topology.Edges[EdgeIndex];
            if (Positions.IsValidIndex(Edge.X) && Positions.IsValidIndex(Edge.Y))
            {
                EdgeLines->SetLineStart(EdgeIndex, Accel.GetDisplacedVertexPosition(Positions, Edge.X, 0.25f));
                EdgeLines->SetLineEnd(EdgeIndex, Accel.GetDisplacedVertexPosition(Positions, Edge.Y, 0.25f));
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
            VertexPoints->SetPointPosition(VertexIndex, Accel.DisplacedVertexPositions[VertexIndex]);
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
    const bool bVisible = SoftSettings.bEnabled && !SoftInfluences.IsEmpty() && GetDisplayState(this).bShowSoftVertexWeights;
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
        const float DepthScale = Accel.GetVertexCameraDepthScale(Positions[Influence.Vertex]);
        const float SoftSize = NGonVertexNearSize * DepthScale;
        SoftInfluencePoints->AddPoint(
            Accel.GetDisplacedVertexPosition(Positions, Influence.Vertex, 1.0f),
            Color.ToFColor(true),
            SoftSize,
            1.85f);
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
                EdgeLines->SetLineStart(EdgeIndex, Accel.GetDisplacedVertexPosition(Positions, Edge.X, 0.25f));
                EdgeLines->SetLineEnd(EdgeIndex, Accel.GetDisplacedVertexPosition(Positions, Edge.Y, 0.25f));
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
    if (bShow && VertexPoints && Accel.bVerticesOverlayDirty)
    {
        if (Accel.DisplacedVertexPositions.Num() != Positions.Num())
        {
            Accel.RecomputeAllVertexNormals(Positions);
        }
        for (int32 VertexIndex = 0; VertexIndex < Positions.Num(); ++VertexIndex)
        {
            VertexPoints->SetPointPosition(VertexIndex, Accel.DisplacedVertexPositions[VertexIndex]);
        }
        Accel.bVerticesOverlayDirty = false;
    }
    if (VertexPoints)
    {
        VertexPoints->SetVisibility(bShow);
    }
}

void FNGonPreviewScene::SetHeatmapVisible(bool bShow)
{
    if (bHeatmapVisible != bShow)
    {
        bHeatmapVisible = bShow;
        UpdateSurfaceDisplay(true);
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
    const int64 Expected = static_cast<int64>(Width) * Height;
    if (Width <= 0 || Height <= 0 || Pixels.Num() != Expected
        || !Algo::AnyOf(Pixels, [](FColor Pixel) { return Pixel.A != 0; }))
    {
        ClearPaintTexture();
        return;
    }

    if (!PaintTexture.IsValid() || PaintWidth != Width || PaintHeight != Height)
    {
        TStrongObjectPtr<UTexture2D> NewTexture(MakePaintTexture(Width, Height));
        if (!UploadPaintPixels(NewTexture.Get(), Pixels.GetData(), Expected))
        {
            return;
        }
        PaintTexture = MoveTemp(NewTexture);
        if (!PaintMaterial.IsValid())
        {
            PaintMaterial.Reset(UMaterialInstanceDynamic::Create(MakePaintMaterial(PaintTexture.Get()), Surface));
        }
        if (PaintMaterial.IsValid())
        {
            PaintMaterial->SetTextureParameterValue(PaintTextureParameter, PaintTexture.Get());
        }
    }
    else if (!UploadPaintPixels(PaintTexture.Get(), Pixels.GetData(), Expected))
    {
        return;
    }

    PaintPixels = Pixels;
    PaintWidth = Width;
    PaintHeight = Height;
    UpdateSurfaceDisplay(true);
}

void FNGonPreviewScene::ClearPaintTexture()
{
    if (!PaintPixels.IsEmpty())
    {
        PaintPixels.Reset();
        PaintWidth = 0;
        PaintHeight = 0;
        PaintTexture.Reset();
        PaintMaterial.Reset();
        UpdateSurfaceDisplay(false);
    }
}

void FNGonPreviewScene::SetHeatBias(float Value)
{
    if (HeatBias != Value)
    {
        HeatBias = Value;
        UpdateSurfaceDisplay(true);
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
    GetSceneAccel(this).RecomputeAllVertexNormals(Positions);
    UpdateOverlayPositions();
    if (!SelectedIds.IsEmpty())
    {
        GetSceneAccel(this).bBoneGizmoActive = false;
    }
    RebuildSoftSelection();
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
    UpdateSurfaceDisplay(true);
}

FTransform FNGonPreviewScene::GetSelectionGizmoTransform() const
{
    const FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex) && (Accel.bBoneGizmoActive || SelectedIds.IsEmpty()))
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

FTransform FNGonPreviewScene::GetGizmoDisplayTransform() const
{
    const FNGonSceneAccel& Accel = GetSceneAccel(this);
    if (SoftSession.IsActive() || Accel.bBoneDragActive)
    {
        return FTransform(Accel.DragStartGizmoTransform.GetRotation(), ActiveGizmoTransform.GetLocation());
    }
    return GetSelectionGizmoTransform();
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
                const FVector EdgeA = (Positions[Face.Perimeter[Corner]] - Origin).GetSafeNormal(1.e-12);
                const FVector EdgeB = (Positions[Face.Perimeter[Next]] - Origin).GetSafeNormal(1.e-12);
                Normal += FVector::CrossProduct(EdgeA, EdgeB);
            }
        }
        return Normal.GetSafeNormal(1.e-12);
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
            const FNGonSceneAccel& Accel = GetSceneAccel(this);
            Sample.Position = Positions[Id];
            if (Accel.LogicalVertexNormals.IsValidIndex(Id))
            {
                Sample.Normal = Accel.GetVertexNormal(Id);
                Sample.Tangent = Accel.GetVertexTangent(Id);
            }
            else if (const TArray<int32>* IncidentFaces = GizmoVertexFaces.Find(Id))
            {
                for (const int32 FaceId : *IncidentFaces)
                {
                    const FNGonFace* FacePtr = Topology.Faces.Find(FaceId);
                    if (!FacePtr) continue;
                    const FNGonFace& Face = *FacePtr;
                    Sample.Normal += FaceNormal(Face);
                    for (int32 Corner = 0; Corner < Face.Perimeter.Num(); ++Corner)
                    {
                        if (Face.Perimeter[Corner] != Id) continue;
                        const int32 Next = Face.Perimeter[(Corner + 1) % Face.Perimeter.Num()];
                        if (Positions.IsValidIndex(Next))
                        {
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
        if (!Sample.Normal.Normalize(1.e-12))
        {
            Sample.Normal = FVector::UpVector;
        }
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
    if (Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex) && (Accel.bBoneGizmoActive || SelectedIds.IsEmpty()))
    {
        Accel.bBoneGizmoActive = true;
    }
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
            Accel.RecomputeDirtyVertexNormals(Accel.ActiveSubtreeVertices, Positions);

            if (Accel.bShowVertices)
            {
                for (const int32 LogicalId : Accel.ActiveSubtreeVertices)
                {
                    VertexPoints->SetPointPosition(LogicalId, Accel.DisplacedVertexPositions[LogicalId]);
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
                    EdgeLines->SetLineStart(EdgeIndex, Accel.GetDisplacedVertexPosition(Positions, Edge.X, 0.25f));
                    EdgeLines->SetLineEnd(EdgeIndex, Accel.GetDisplacedVertexPosition(Positions, Edge.Y, 0.25f));
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
    Accel.RecomputeDirtyVertexNormals(Accel.DirtyVertices, Positions);

    // 2. Update only the affected vertex points and incident edge lines using stored mesh-space vertex normals.
    if (Accel.bShowVertices)
    {
        for (const int32 LogicalId : Accel.DirtyVertices)
        {
            VertexPoints->SetPointPosition(LogicalId, Accel.DisplacedVertexPositions[LogicalId]);
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
            EdgeLines->SetLineStart(EdgeIndex, Accel.GetDisplacedVertexPosition(Positions, Edge.X, 0.25f));
            EdgeLines->SetLineEnd(EdgeIndex, Accel.GetDisplacedVertexPosition(Positions, Edge.Y, 0.25f));
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
            // only update the normal-displaced positions of existing SoftInfluencePoints.
            if (SoftInfluencePoints && SoftInfluencePoints->IsVisible() &&
                Accel.CachedSoftPointCount == SoftInfluences.Num())
            {
                for (int32 idx = 0; idx < SoftInfluences.Num(); ++idx)
                {
                    const int32 VertId = SoftInfluences[idx].Vertex;
                    if (Positions.IsValidIndex(VertId))
                    {
                        SoftInfluencePoints->SetPointPosition(idx, Accel.GetDisplacedVertexPosition(Positions, VertId, 1.35f));
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

            if (SurfaceDisplay == ESurfaceDisplay::Heatmap && !ColorElements.IsEmpty())
            {
                FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
                for (const int32 LogicalId : Accel.DirtyVertices)
                {
                    const int32 StartC = Accel.LogicalToColorOffsets[LogicalId];
                    const int32 EndC = Accel.LogicalToColorOffsets[LogicalId + 1];
                    for (int32 i = StartC; i < EndC; ++i)
                    {
                        const FColorElement& Element = ColorElements[Accel.LogicalToColorElements[i]];
                        Colors->SetElement(Element.OverlayId, GetOverlayColor(Element));
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
        return;
    }
    SoftSession.End();
    if (!SelectedIds.IsEmpty())
    {
        GetSceneAccel(this).bBoneGizmoActive = false;
    }
    RebuildSoftSelection();
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
        RefreshBoneDeformation();
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
    Accel.RecomputeAllVertexNormals(Positions);
    UpdateOverlayPositions();
    SoftSession.Cancel();
    if (!SelectedIds.IsEmpty())
    {
        GetSceneAccel(this).bBoneGizmoActive = false;
    }
    RebuildSoftSelection();
}

int32 FNGonPreviewScene::PickHoverVertex(const FSceneView& View, const FVector2D& Cursor, bool bAllowThroughMesh) const
{
    if (Positions.IsEmpty())
    {
        return INDEX_NONE;
    }
    if (!bAllowThroughMesh)
    {
        // Exact BVH raycast occlusion test: never detect vertices on the back of the mesh in shaded modes
        return Picker.Pick(ENGonElement::Vertex, View, Cursor, Topology, Positions);
    }

    // Wireframe / X-Ray mode: allow through-mesh vertex hover detection
    const FVector CamOrigin = View.ViewMatrices.GetViewOrigin();
    const FVector CamForward = View.ViewMatrices.GetWorldToView().GetColumn(2).GetSafeNormal();
    const double MaxHoverDistSq = FMath::Square(15.0);
    int32 BestVert = INDEX_NONE;
    double BestScore = TNumericLimits<double>::Max();

    for (int32 v = 0; v < Positions.Num(); ++v)
    {
        const FVector ToVert = Positions[v] - CamOrigin;
        if (FVector::DotProduct(ToVert, CamForward) <= 0.0)
        {
            continue;
        }
        FVector2D Px;
        if (!View.WorldToPixel(Positions[v], Px))
        {
            continue;
        }
        const double DistSq = FVector2D::DistSquared(Px, Cursor);
        if (DistSq <= MaxHoverDistSq)
        {
            const double Score = DistSq + ToVert.Size() * 0.0005;
            if (Score < BestScore)
            {
                BestScore = Score;
                BestVert = v;
            }
        }
    }
    return BestVert;
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
    if (bHadFaces || (Element == ENGonElement::Face && !Ids.IsEmpty()))
    {
        UpdateFaceHighlight();
    }
    DrawSelection();
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
    TArray<int32> RemainingMaterials;
    RemainingMaterials.Reserve(RemainingFaces.Max());
    const FDynamicMeshMaterialAttribute* OldMaterials = Surface->GetDynamicMesh()->GetMeshRef().Attributes()->GetMaterialID();
    for (const TPair<int32, FNGonFace>& FacePair : Topology.Faces)
    {
        if (!ValidSelectedIds.Contains(FacePair.Key) && FacePair.Value.Perimeter.Num() >= 3)
        {
            RemainingFaces.Add(FacePair.Value.Perimeter);
            RemainingMaterials.Add(OldMaterials && !FacePair.Value.Triangles.IsEmpty()
                ? OldMaterials->GetValue(FacePair.Value.Triangles[0]) : 0);
        }
    }
    if (RemainingFaces.IsEmpty()) return false;

    FNGonMeshData Replacement;
    FText Error;
    if (!NGon::BuildPolygonMesh(Positions, RemainingFaces, Replacement, Error)) return false;
    if (!Replacement.Mesh.Attributes()->HasMaterialID())
    {
        Replacement.Mesh.Attributes()->EnableMaterialID();
    }
    FDynamicMeshMaterialAttribute* ReplacementMaterials = Replacement.Mesh.Attributes()->GetMaterialID();
    for (const TPair<int32, FNGonFace>& Face : Replacement.Topology.Faces)
    {
        const int32 Material = RemainingMaterials.IsValidIndex(Face.Key) ? RemainingMaterials[Face.Key] : 0;
        for (const int32 TriangleId : Face.Value.Triangles)
        {
            ReplacementMaterials->SetValue(TriangleId, Material);
        }
    }

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

    SetMesh(MoveTemp(Replacement), Surface->GetMaterials());
    SoftTransformMode = SavedSoftMode;
    SoftSettings = SavedSoftSettings;
    HeatBias = SavedHeatBias;
    UpdateSurfaceDisplay(true);
    RebuildSoftSelection();
    return true;
}

bool FNGonPreviewScene::ReplacePolygonMesh(
    const TArray<FVector>& NewPositions,
    const TArray<TArray<int32>>& NewFaces,
    FText& OutError)
{
    FNGonMeshData Replacement;
    if (!NGon::BuildPolygonMesh(NewPositions, NewFaces, Replacement, OutError))
    {
        return false;
    }

    const ENGonSoftTransformMode SavedSoftMode = SoftTransformMode;
    const FNGonSoftSelectionSettings SavedSoftSettings = SoftSettings;
    const float SavedHeatBias = HeatBias;
    const TArray<UMaterialInterface*> SavedMaterials = Surface ? Surface->GetMaterials() : TArray<UMaterialInterface*>();

    SetMesh(MoveTemp(Replacement), SavedMaterials);
    SoftTransformMode = SavedSoftMode;
    SoftSettings = SavedSoftSettings;
    HeatBias = SavedHeatBias;
    UpdateSurfaceDisplay(true);
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
    if (bHadFaces || (Element == ENGonElement::Face && Id != INDEX_NONE))
    {
        UpdateFaceHighlight();
    }
    DrawSelection();
}

void FNGonPreviewScene::DrawSelection()
{
    FNGonSceneAccel& Accel = GetSceneAccel(this);
    SelectedEdge->Clear();

    if (SelectedElement == ENGonElement::Face && !SelectedIds.IsEmpty())
    {
        SelectedVertex->Clear();
        Accel.CachedSelectedVertexCount = 0;
        TArray<FVector> Starts;
        TArray<FVector> Ends;
        for (const int32 Id : SelectedIds)
        {
            if (const FNGonFace* Face = Topology.Faces.Find(Id))
            {
                for (int32 Corner = 0; Corner < Face->Perimeter.Num(); ++Corner)
                {
                    const int32 V0 = Face->Perimeter[Corner];
                    const int32 V1 = Face->Perimeter[(Corner + 1) % Face->Perimeter.Num()];
                    Starts.Add(Accel.GetDisplacedVertexPosition(Positions, V0, 0.55f));
                    Ends.Add(Accel.GetDisplacedVertexPosition(Positions, V1, 0.55f));
                }
            }
        }
        SelectedEdge->AddLines(Starts, Ends, SelectionColor, 2.6f, 2.5f);
    }
    else if (SelectedElement == ENGonElement::Edge && !SelectedIds.IsEmpty())
    {
        SelectedVertex->Clear();
        Accel.CachedSelectedVertexCount = 0;
        TArray<FVector> Starts;
        TArray<FVector> Ends;
        Starts.Reserve(SelectedIds.Num());
        Ends.Reserve(SelectedIds.Num());
        for (const int32 Id : SelectedIds)
        {
            if (!Topology.Edges.IsValidIndex(Id)) continue;
            const FIntPoint Edge = Topology.Edges[Id];
            if (!Positions.IsValidIndex(Edge.X) || !Positions.IsValidIndex(Edge.Y)) continue;
            Starts.Add(Accel.GetDisplacedVertexPosition(Positions, Edge.X, 0.65f));
            Ends.Add(Accel.GetDisplacedVertexPosition(Positions, Edge.Y, 0.65f));
        }
        SelectedEdge->AddLines(Starts, Ends, SelectionColor, 3.6f, 3.0f);
    }
    else if (SelectedElement == ENGonElement::Vertex && !SelectedIds.IsEmpty())
    {
        SelectedVertex->Clear();
        int32 AddedSel = 0;
        for (const int32 Id : SelectedIds)
        {
            if (Positions.IsValidIndex(Id))
            {
                const float DepthScale = Accel.GetVertexCameraDepthScale(Positions[Id]);
                SelectedVertex->AddPoint(
                    Accel.GetDisplacedVertexPosition(Positions, Id, 1.35f),
                    SelectedVertexColor,
                    NGonSelectedVertexNearSize * DepthScale,
                    NGonSelectedVertexDepthBias);
                ++AddedSel;
            }
        }
        Accel.CachedSelectedVertexCount = AddedSel;
    }
    else
    {
        SelectedVertex->Clear();
        Accel.CachedSelectedVertexCount = 0;
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
    if (SoftSettings.bEnabled)
    {
        return FText::Format(LOCTEXT("SoftStatusCompact", "{0}  |  Soft R={1}"),
            MeshInfo, FText::AsNumber(FMath::RoundToInt(SoftSettings.Radius)));
    }
    return MeshInfo;
}

FText FNGonPreviewScene::DescribeUpdates() const
{
    return FText::Format(LOCTEXT("Updates", "Updates   Topology {0}    Positions {1}    Colors {2}"),
        FText::AsNumber(TopologyUpdates), FText::AsNumber(PositionUpdates), FText::AsNumber(ColorUpdates));
}




namespace NGon
{
void SetShowSoftVertexWeights(FNGonPreviewScene* Scene, bool bShow)
{
    if (!Scene) return;
    GetDisplayState(Scene).bShowSoftVertexWeights = bShow;
    Scene->SetSoftSelection(Scene->GetSoftSelectionSettings());
}

bool IsShowingSoftVertexWeights(const FNGonPreviewScene* Scene)
{
    return Scene ? GetDisplayState(Scene).bShowSoftVertexWeights : true;
}

void SetXRayEnabled(FNGonPreviewScene* Scene, bool bEnable)
{
    if (!Scene) return;
    GetDisplayState(Scene).bXRayEnabled = bEnable;
    // Force surface material refresh
    Scene->SetSoftSelection(Scene->GetSoftSelectionSettings());
}

bool IsXRayEnabled(const FNGonPreviewScene* Scene)
{
    return Scene ? GetDisplayState(Scene).bXRayEnabled : false;
}

void SetXRayOpacity(FNGonPreviewScene* Scene, float Opacity)
{
    if (!Scene) return;
    const float Clamped = FMath::Clamp(Opacity, 0.0f, 1.0f);
    GetDisplayState(Scene).XRayOpacity = Clamped;
    if (TStrongObjectPtr<UMaterialInstanceDynamic>* XRayRef = GXRayMaterialMap.Find(Scene); XRayRef && XRayRef->IsValid())
    {
        (*XRayRef)->SetScalarParameterValue(TEXT("XRayOpacity"), Clamped);
    }
    if (GetDisplayState(Scene).bXRayEnabled)
    {
        Scene->SetSoftSelection(Scene->GetSoftSelectionSettings());
    }
}

float GetXRayOpacity(const FNGonPreviewScene* Scene)
{
    return Scene ? GetDisplayState(Scene).XRayOpacity : 0.35f;
}

void SetWireframeEdgesOnly(FNGonPreviewScene* Scene, bool bEdgesOnly)
{
    if (!Scene) return;
    GetDisplayState(Scene).bWireframeEdgesOnly = bEdgesOnly;
    if (bEdgesOnly)
    {
        Scene->SetShowEdges(true);
    }
    Scene->SetSoftSelection(Scene->GetSoftSelectionSettings());
}

bool IsWireframeEdgesOnly(const FNGonPreviewScene* Scene)
{
    return Scene ? GetDisplayState(Scene).bWireframeEdgesOnly : false;
}
} // namespace NGon

#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2

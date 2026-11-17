#include "NGonPreviewScene.h"

#include "Components/DynamicMeshComponent.h"
#include "Drawing/LineSetComponent.h"
#include "Drawing/PointSetComponent.h"
#include "Drawing/PreviewGeometryActor.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "ToolSetupUtil.h"
#include "UDynamicMesh.h"
#include "BaseGizmos/CombinedTransformGizmo.h"
#include "BaseGizmos/TransformGizmoUtil.h"
#include "BaseGizmos/TransformProxy.h"
#include "InteractiveGizmoManager.h"

using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "NGonPreviewScene"

namespace
{
const FColor EdgeColor(80, 224, 246);
const FColor PointColor(230, 249, 255);
const FColor SelectionColor(255, 181, 65);

UMaterial* MakeSurfaceMaterial()
{
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
    Material->SetShadingModel(MSM_Unlit);
    Material->TwoSided = true;

    auto* VertexColor = UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionVertexColor::StaticClass(), -400, 0);
    auto* Shade = CastChecked<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(
        Material, UMaterialExpressionCustom::StaticClass(), -180, 0));
    Shade->OutputType = CMOT_Float3;
    Shade->Inputs.SetNum(1);
    Shade->Inputs[0].InputName = TEXT("MeshData");
    Shade->Inputs[0].Input.Connect(0, VertexColor);
    // R is the scalar. G selects the entire polygon and B enables the ramp.
    // Evaluate the ramp after interpolation so it also works on large triangles.
    Shade->Code = TEXT(
        "float t = saturate(MeshData.r);\n"
        "float3 cold = float3(0.025, 0.11, 0.85);\n"
        "float3 mid = float3(0.055, 0.76, 0.34);\n"
        "float3 hot = float3(0.98, 0.08, 0.018);\n"
        "float3 ramp = lerp(lerp(cold, mid, saturate(2 * t)), hot, saturate(2 * t - 1));\n"
        "float3 surface = lerp(float3(0.16, 0.22, 0.29), ramp, MeshData.b);\n"
        "float3 color = lerp(surface, float3(1, 0.48, 0.075), 0.65 * MeshData.g);\n"
        "float ndotl = abs(dot(Parameters.WorldNormal, Parameters.CameraVector));\n"
        "return color * lerp(0.62, 1.0, ndotl);");

    Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Shade);
    Material->PostEditChange();
    return Material;
}
}

FNGonPreviewScene::FNGonPreviewScene()
    : Scene(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(false))
{
    Surface = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
    Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Surface->SetCastShadow(false);
    Surface->SetEnableFlatShading(true);
    Surface->SetVertexColorSpaceTransformMode(EDynamicMeshVertexColorTransformMode::NoTransform);
    Surface->SetMaterial(0, MakeSurfaceMaterial());
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
    Picker.SetMesh(nullptr);
    Overlay->Disconnect();
    Scene.RemoveComponent(Surface);
}

void FNGonPreviewScene::SetupGizmo(UInteractiveGizmoManager* InGizmoManager, IToolContextTransactionProvider* TransactionProvider)
{
    TeardownGizmo();
    GizmoManager = InGizmoManager;
    if (!GizmoManager) return;
    TransformProxy = NewObject<UTransformProxy>(GizmoManager);
    // UInteractiveGizmoManager registers its built-in builders during tools
    // context initialization. Use that manager API directly; the namespace
    // utility is for the separate UCombinedTransformGizmoContextObject path
    // and can assert when the editor context has not installed that helper.
    TransformGizmo = GizmoManager->Create3AxisTransformGizmo(this, TEXT("NGonPreview"));
    if (!TransformGizmo) { TransformProxy = nullptr; return; }
    TransformProxy->SetTransform(GetSelectionGizmoTransform());
    TransformProxy->OnBeginTransformEdit.AddLambda([this](UTransformProxy*) {
        if (!bUpdatingGizmo) BeginSoftTransform(TransformProxy->GetTransform(), SoftTransformMode);
    });
    TransformProxy->OnTransformChanged.AddLambda([this](UTransformProxy*, FTransform Transform) {
        if (!bUpdatingGizmo) ApplyGizmoTransform(Transform);
    });
    TransformProxy->OnEndTransformEdit.AddLambda([this](UTransformProxy*) {
        if (!bUpdatingGizmo) EndSoftTransform();
    });
    TransformGizmo->SetActiveTarget(TransformProxy, TransactionProvider);
    UpdateGizmo();
}

void FNGonPreviewScene::TeardownGizmo()
{
    if (TransformGizmo && GizmoManager) GizmoManager->DestroyGizmo(TransformGizmo);
    TransformGizmo = nullptr; TransformProxy = nullptr; GizmoManager = nullptr;
}

void FNGonPreviewScene::UpdateGizmo()
{
    if (!TransformProxy || !TransformGizmo) return;
    bUpdatingGizmo = true;
    const FTransform Frame = GetSelectionGizmoTransform();
    TransformGizmo->SetVisibility(!SelectedIds.IsEmpty());
    TransformProxy->SetTransform(Frame);
    TransformGizmo->ReinitializeGizmoTransform(Frame);
    bUpdatingGizmo = false;
}

FBox FNGonPreviewScene::GetSelectionOrRestBounds() const
{
    if (SelectedIds.IsEmpty())
    {
        return Topology.RestBounds;
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
    return Bounds.IsValid ? Bounds.ExpandBy(FMath::Max(Bounds.GetSize().GetMax() * 0.05, 1.0)) : Topology.RestBounds;
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

void FNGonPreviewScene::WriteColors(FDynamicMesh3& Mesh) const
{
    FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
    const bool bUseSoftHeat = bHeatmapVisible && SoftSettings.bEnabled && !SoftInfluences.IsEmpty();
    for (const FColorElement& Element : ColorElements)
    {
        const float Sample = bUseSoftHeat && SoftHeatSamples.IsValidIndex(Element.LogicalVertex)
            ? SoftHeatSamples[Element.LogicalVertex] : 0.0f;
        const bool bSelected = SelectedElement == ENGonElement::Face && SelectedIds.Contains(Element.Face);
        Colors->SetElement(Element.OverlayId, FVector4f(
            Sample, bSelected ? 1.0f : 0.0f, bUseSoftHeat ? 1.0f : 0.0f, 1.0f));
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
    DrawSelection();
    DrawSoftSelection();
}

void FNGonPreviewScene::UpdateOverlayPositions()
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
    for (int32 VertexIndex = 0; VertexIndex < Positions.Num(); ++VertexIndex)
    {
        VertexPoints->SetPointPosition(VertexIndex, Positions[VertexIndex]);
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
    SoftInfluencePoints->Clear();
    const bool bVisible = SoftSettings.bEnabled && !SoftInfluences.IsEmpty();
    SoftInfluencePoints->SetVisibility(bVisible);
    if (!bVisible)
    {
        return;
    }

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
    }
}

void FNGonPreviewScene::SetShowEdges(bool bShow)
{
    EdgeLines->SetVisibility(bShow);
}

void FNGonPreviewScene::SetShowVertices(bool bShow)
{
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

    Surface->FastNotifyPositionsUpdated(false, false, false);
    Picker.PositionsChanged();
    UpdateOverlayPositions();
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
    return BuildComponentGizmoTransform();
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
    return SoftSession.Begin(Topology, Positions, Seeds, FTransform::Identity, GizmoTransform, SoftSettings, Mode);
}

bool FNGonPreviewScene::ApplyGizmoTransform(const FTransform& GizmoTransform)
{
    TArray<FVector> Updated;
    if (!SoftSession.BuildPositions(Topology, Positions, GizmoTransform, SoftSettings, Updated)) return false;
    if (Updated.Num() != Positions.Num()) return false;
    Positions = MoveTemp(Updated);
    FDynamicMesh3& Mesh = Surface->GetDynamicMesh()->GetMeshRef();
    for (const int32 Vertex : Mesh.VertexIndicesItr())
    {
        const int32 LogicalId = Topology.DynamicToLogicalVertex.IsValidIndex(Vertex) ? Topology.DynamicToLogicalVertex[Vertex] : INDEX_NONE;
        if (Positions.IsValidIndex(LogicalId)) Mesh.SetVertex(Vertex, Positions[LogicalId]);
    }
    Surface->FastNotifyPositionsUpdated(false, false, false);
    Picker.PositionsChanged();
    UpdateOverlayPositions();
    ++PositionUpdates;
    ActiveGizmoTransform = GizmoTransform;
    SoftInfluences = SoftSession.Influences();
    RebuildSoftHeatSamples();
    DrawSoftSelection();
    UpdateColors();
    return true;
}

void FNGonPreviewScene::EndSoftTransform()
{
    SoftSession.End();
}

void FNGonPreviewScene::CancelSoftTransform()
{
    if (!SoftSession.IsActive()) return;
    Positions = SoftSession.OriginalPositions();
    FDynamicMesh3& Mesh = Surface->GetDynamicMesh()->GetMeshRef();
    for (const int32 Vertex : Mesh.VertexIndicesItr())
    {
        const int32 LogicalId = Topology.DynamicToLogicalVertex.IsValidIndex(Vertex) ? Topology.DynamicToLogicalVertex[Vertex] : INDEX_NONE;
        if (Positions.IsValidIndex(LogicalId)) Mesh.SetVertex(Vertex, Positions[LogicalId]);
    }
    Surface->FastNotifyPositionsUpdated(false, false, false);
    Picker.PositionsChanged();
    UpdateOverlayPositions();
    SoftSession.Cancel();
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
            float Influence = 0.0f;
            for (const FNGonVertexInfluence& Entry : SoftInfluences)
            {
                if (Entry.Vertex == Id)
                {
                    Influence = FMath::Clamp(static_cast<float>(Entry.Weight), 0.0f, 1.0f);
                    break;
                }
            }
            return FText::Format(LOCTEXT("VertexInfo", "Vertex {0}     soft influence {1}"),
                FText::AsNumber(Topology.SourceVertexIds[Id]), FText::AsNumber(Influence));
        }
        break;
    }
    return LOCTEXT("NoSelection", "No selection");
}

FText FNGonPreviewScene::DescribeMesh() const
{
    int32 NGonCount = 0;
    for (const TPair<int32, FNGonFace>& Pair : Topology.Faces)
    {
        NGonCount += Pair.Value.Perimeter.Num() > 4 ? 1 : 0;
    }
    const FText MeshInfo = FText::Format(LOCTEXT("MeshInfo", "{0} faces     {1} triangles     {2} edges     {3} vertices     {4} N-Gons (max {5} sides)"),
        FText::AsNumber(Topology.Faces.Num()), FText::AsNumber(Topology.TriangleCount),
        FText::AsNumber(Topology.Edges.Num()), FText::AsNumber(Topology.RestPositions.Num()),
        FText::AsNumber(NGonCount), FText::AsNumber(Topology.MaxFaceCorners));
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

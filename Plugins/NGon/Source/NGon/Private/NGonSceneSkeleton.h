#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "NGonTopology.h"

class FNGonPreviewScene;
class FPrimitiveDrawInterface;
class FSceneView;
class FViewport;
class FCanvas;

#ifndef NGON_SCENE_BONE_BRIDGE_DECLARED
#define NGON_SCENE_BONE_BRIDGE_DECLARED
namespace NGon
{
struct FNGonSceneBoneInit
{
    int32 BoneIndex = INDEX_NONE;
    int32 ParentIndex = INDEX_NONE;
    FName Name = NAME_None;
    FTransform RefLocalTransform = FTransform::Identity;
    FTransform RefComponentTransform = FTransform::Identity;
};

struct FNGonVertexBoneWeight
{
    int32 BoneIndex = INDEX_NONE;
    float Weight = 0.0f;
};

void SetSceneSkeleton(
    FNGonPreviewScene* Scene,
    const TArray<FNGonSceneBoneInit>& InBones,
    const TArray<TArray<FNGonVertexBoneWeight>>& InVertexWeights);
void ClearSceneSkeleton(FNGonPreviewScene* Scene);
bool HasSceneBones(const FNGonPreviewScene* Scene);
int32 GetSceneBoneCount(const FNGonPreviewScene* Scene);
void SelectSceneBone(FNGonPreviewScene* Scene, int32 BoneIndex, bool bActivateBoneGizmo);
int32 GetSelectedSceneBone(const FNGonPreviewScene* Scene);
bool IsBoneGizmoActive(const FNGonPreviewScene* Scene);
void SetBoneGizmoActive(FNGonPreviewScene* Scene, bool bActive);
bool IsBoneTransformActive(const FNGonPreviewScene* Scene);
bool GetSceneBoneTransforms(
    const FNGonPreviewScene* Scene,
    int32 BoneIndex,
    FTransform& OutLocal,
    FTransform& OutComponent,
    FTransform& OutRefLocal,
    FTransform& OutRefComponent);
void SetSceneBoneLocalTransform(FNGonPreviewScene* Scene, int32 BoneIndex, const FTransform& NewLocalTransform);
bool ResetSelectedSceneBoneTransform(FNGonPreviewScene* Scene);
bool ResetAllSceneBonesTransforms(FNGonPreviewScene* Scene);
int32 PickSceneBone(
    FNGonPreviewScene* Scene,
    const FSceneView& View,
    const FVector2D& ScreenCursor,
    float MaxPixelDist,
    bool bSelectOnHit);
void DrawSceneSkeleton(
    const FNGonPreviewScene* Scene,
    const FSceneView* View,
    FPrimitiveDrawInterface* PDI,
    int32 BoneDrawMode);
void DrawSceneSkeletonLabels(
    const FNGonPreviewScene* Scene,
    const FSceneView* View,
    FViewport* InViewport,
    FCanvas* Canvas,
    bool bShowBoneNames);
void SetSceneCoordinateSystem(FNGonPreviewScene* Scene, bool bUseWorldSpace);
bool IsSceneWorldCoordinateSystem(const FNGonPreviewScene* Scene);
void SetSceneSnapSettings(
    FNGonPreviewScene* Scene,
    bool bSnapGrid,
    float GridStep,
    bool bSnapRot,
    float RotStepDeg,
    bool bSnapScale,
    float ScaleStep);
void GetSceneSnapSettings(
    const FNGonPreviewScene* Scene,
    bool& OutSnapGrid,
    float& OutGridStep,
    bool& OutSnapRot,
    float& OutRotStepDeg,
    bool& OutSnapScale,
    float& OutScaleStep);
bool IsUtilityNonDeformingBoneName(const FName& Name);
void UpdateSceneCameraView(FNGonPreviewScene* Scene, const FVector& CameraOrigin, const FVector& CameraDir);
} // namespace NGon
#endif // NGON_SCENE_BONE_BRIDGE_DECLARED

namespace NGon
{
using UE::Geometry::FDynamicMesh3;

struct FNGonSceneAccel
{
    int32 CachedNGonCount = 0;
    bool bShowEdges = true;
    bool bShowVertices = true;
    bool bEdgesOverlayDirty = false;
    bool bVerticesOverlayDirty = false;
    int32 CachedSoftPointCount = 0;

    // CSR mapping: logical vertex -> dynamic mesh vertex IDs
    TArray<int32> LogicalToDynamicOffsets;
    TArray<int32> LogicalToDynamicVertices;

    // CSR mapping: logical vertex -> incident edge indices
    TArray<int32> LogicalToEdgeOffsets;
    TArray<int32> LogicalToEdges;

    // CSR mapping: logical vertex -> ColorElements indices
    TArray<int32> LogicalToColorOffsets;
    TArray<int32> LogicalToColorElements;

    // Flat triangle logical vertex indices (3 ints per triangle) & CSR vertex->triangles for fast mesh-space vertex normals
    TArray<int32> FlatTriLogicalVertices;
    TArray<int32> LogicalToTriOffsets;
    TArray<int32> LogicalToTris;

    // Mesh-space unit vertex normals, tangents, and normal-displaced display positions per logical vertex
    TArray<FVector3f> LogicalVertexNormals;
    TArray<FVector3f> LogicalVertexTangents;
    TArray<FVector> DisplacedVertexPositions;
    float VertexNormalOffset = 0.0f;
    int32 CachedSelectedVertexCount = 0;

    // Camera-distance vertex size scaling (1.0x at near camera point -> 0.5x (1/2) at far camera point)
    FVector LastCameraOrigin = FVector::ZeroVector;
    FVector LastCameraDir = FVector::ForwardVector;
    float NearCameraDist = 10.0f;
    float FarCameraDist = 200.0f;
    float CameraZoomScale = 1.0f;
    bool bHasCameraView = false;
    uint64 LastVertexSizeUpdateFrame = 0;

    // Stamp buffers for zero-allocation partial updates during gizmo drags
    int32 Stamp = 1;
    TArray<int32> VertexStamp;
    TArray<int32> EdgeStamp;
    TArray<int32> DirtyVertices;
    TArray<int32> DirtyEdges;
    TArray<int32> PreviousSessionVertices;

    // Skeleton & Joint Gizmo Skinning State
    struct FSceneBoneState
    {
        int32 BoneIndex = INDEX_NONE;
        int32 ParentIndex = INDEX_NONE;
        FName Name = NAME_None;
        FTransform RefLocalTransform = FTransform::Identity;
        FTransform RefComponentTransform = FTransform::Identity;
        FTransform CurrentLocalTransform = FTransform::Identity;
        FTransform CurrentComponentTransform = FTransform::Identity;
        FMatrix InvRefComponentMatrix = FMatrix::Identity;
        FMatrix SkinMatrix = FMatrix::Identity;
        TArray<int32> ChildBones;
    };

    TArray<FSceneBoneState> Bones;
    TArray<FVector> SkinnedBasePositions;
    TArray<int32> VertexSkinOffsets;
    TArray<NGon::FNGonVertexBoneWeight> VertexSkinWeights;
    TArray<int32> BoneToVertexOffsets;
    TArray<int32> BoneToVertices;

    int32 SelectedBoneIndex = INDEX_NONE;
    bool bBoneGizmoActive = false;
    bool bBoneDragActive = false;
    bool bPendingSkinningUpdate = false;
    bool bPendingFullSkinReset = false;

    FTransform DragStartGizmoTransform = FTransform::Identity;
    FTransform DragStartBoneLocalTransform = FTransform::Identity;
    FTransform DragStartBoneCompTransform = FTransform::Identity;
    TArray<int32> ActiveSubtreeBones;
    TArray<int32> ActiveSubtreeVertices;

    // Toolbar Coordinate Space & Snapping State
    bool bUseWorldCoordinateSystem = false;
    bool bSnapGrid = false;
    float GridSnapStep = 10.0f;
    bool bSnapRotation = false;
    float RotationSnapStep = 10.0f;
    bool bSnapScale = false;
    float ScaleSnapStep = 0.25f;

    void RecomputeBoneHierarchy()
    {
        for (int32 i = 0; i < Bones.Num(); ++i)
        {
            FSceneBoneState& Bone = Bones[i];
            if (Bone.ParentIndex >= 0 && Bones.IsValidIndex(Bone.ParentIndex) && Bone.ParentIndex != i)
            {
                Bone.CurrentComponentTransform = Bone.CurrentLocalTransform * Bones[Bone.ParentIndex].CurrentComponentTransform;
            }
            else
            {
                Bone.CurrentComponentTransform = Bone.CurrentLocalTransform;
            }
            Bone.SkinMatrix = Bone.InvRefComponentMatrix * Bone.CurrentComponentTransform.ToMatrixWithScale();
        }
    }

    void CollectSubtreeBonesAndVertices(int32 RootBoneIndex)
    {
        ActiveSubtreeBones.Reset();
        ActiveSubtreeVertices.Reset();
        if (!Bones.IsValidIndex(RootBoneIndex))
        {
            return;
        }

        TArray<int32> Stack;
        Stack.Add(RootBoneIndex);
        while (!Stack.IsEmpty())
        {
            const int32 Curr = Stack.Pop(EAllowShrinking::No);
            if (!Bones.IsValidIndex(Curr))
            {
                continue;
            }
            ActiveSubtreeBones.AddUnique(Curr);
            for (const int32 ChildIdx : Bones[Curr].ChildBones)
            {
                Stack.Add(ChildIdx);
            }
        }

        const int32 LogicalCount = SkinnedBasePositions.Num();
        if (VertexStamp.Num() < LogicalCount)
        {
            VertexStamp.Init(0, LogicalCount);
        }
        const int32 Mark = NextStamp();
        for (const int32 BoneIdx : ActiveSubtreeBones)
        {
            if (BoneIdx >= 0 && BoneIdx + 1 < BoneToVertexOffsets.Num())
            {
                const int32 StartV = BoneToVertexOffsets[BoneIdx];
                const int32 EndV = BoneToVertexOffsets[BoneIdx + 1];
                for (int32 i = StartV; i < EndV; ++i)
                {
                    const int32 LogicalId = BoneToVertices[i];
                    if (LogicalId >= 0 && LogicalId < LogicalCount && VertexStamp[LogicalId] != Mark)
                    {
                        VertexStamp[LogicalId] = Mark;
                        ActiveSubtreeVertices.Add(LogicalId);
                    }
                }
            }
        }
    }

    FTransform ApplySnappingToGizmo(const FTransform& StartGizmo, const FTransform& RawGizmo) const
    {
        FTransform Snapped = RawGizmo;
        if (bSnapGrid && GridSnapStep > 0.0f)
        {
            if (bUseWorldCoordinateSystem)
            {
                const FVector Delta = RawGizmo.GetLocation() - StartGizmo.GetLocation();
                const FVector SnappedDelta(
                    FMath::GridSnap(Delta.X, static_cast<double>(GridSnapStep)),
                    FMath::GridSnap(Delta.Y, static_cast<double>(GridSnapStep)),
                    FMath::GridSnap(Delta.Z, static_cast<double>(GridSnapStep)));
                Snapped.SetLocation(StartGizmo.GetLocation() + SnappedDelta);
            }
            else
            {
                const FVector LocalDelta = StartGizmo.GetRotation().UnrotateVector(RawGizmo.GetLocation() - StartGizmo.GetLocation());
                const FVector SnappedLocal(
                    FMath::GridSnap(LocalDelta.X, static_cast<double>(GridSnapStep)),
                    FMath::GridSnap(LocalDelta.Y, static_cast<double>(GridSnapStep)),
                    FMath::GridSnap(LocalDelta.Z, static_cast<double>(GridSnapStep)));
                Snapped.SetLocation(StartGizmo.GetLocation() + StartGizmo.GetRotation().RotateVector(SnappedLocal));
            }
        }
        if (bSnapRotation && RotationSnapStep > 0.0f)
        {
            const FQuat LocalDeltaRot = StartGizmo.GetRotation().Inverse() * RawGizmo.GetRotation();
            FVector Axis = FVector::UpVector;
            double AngleRad = 0.0;
            LocalDeltaRot.ToAxisAndAngle(Axis, AngleRad);
            const double SnappedDeg = FMath::GridSnap(FMath::RadiansToDegrees(AngleRad), static_cast<double>(RotationSnapStep));
            const FQuat SnappedDeltaRot(Axis, FMath::DegreesToRadians(SnappedDeg));
            Snapped.SetRotation((StartGizmo.GetRotation() * SnappedDeltaRot).GetNormalized());
        }
        if (bSnapScale && ScaleSnapStep > 0.0f)
        {
            const FVector DeltaScale = RawGizmo.GetScale3D() - FVector::OneVector;
            const FVector SnappedScale(
                FMath::Max(0.05, 1.0 + FMath::GridSnap(DeltaScale.X, static_cast<double>(ScaleSnapStep))),
                FMath::Max(0.05, 1.0 + FMath::GridSnap(DeltaScale.Y, static_cast<double>(ScaleSnapStep))),
                FMath::Max(0.05, 1.0 + FMath::GridSnap(DeltaScale.Z, static_cast<double>(ScaleSnapStep))));
            Snapped.SetScale3D(SnappedScale);
        }
        return Snapped;
    }

    int32 NextStamp()
    {
        ++Stamp;
        if (Stamp <= 0)
        {
            Stamp = 1;
            FMemory::Memzero(VertexStamp.GetData(), VertexStamp.Num() * sizeof(int32));
            FMemory::Memzero(EdgeStamp.GetData(), EdgeStamp.Num() * sizeof(int32));
        }
        return Stamp;
    }

    void BuildTopologyTables(const FNGonTopology& Topology, const FDynamicMesh3& Mesh)
    {
        const int32 LogicalCount = Topology.RestPositions.Num();
        CachedNGonCount = 0;
        for (const TPair<int32, FNGonFace>& Pair : Topology.Faces)
        {
            if (Pair.Value.Perimeter.Num() > 4)
            {
                ++CachedNGonCount;
            }
        }

        // 1. Logical -> Dynamic vertices CSR
        LogicalToDynamicOffsets.Init(0, LogicalCount + 1);
        for (const int32 DynVert : Mesh.VertexIndicesItr())
        {
            const int32 LogicalId = Topology.DynamicToLogicalVertex.IsValidIndex(DynVert)
                ? Topology.DynamicToLogicalVertex[DynVert] : INDEX_NONE;
            if (LogicalId >= 0 && LogicalId < LogicalCount)
            {
                ++LogicalToDynamicOffsets[LogicalId + 1];
            }
        }
        for (int32 i = 0; i < LogicalCount; ++i)
        {
            LogicalToDynamicOffsets[i + 1] += LogicalToDynamicOffsets[i];
        }
        LogicalToDynamicVertices.SetNumUninitialized(LogicalToDynamicOffsets[LogicalCount]);
        {
            TArray<int32> Cursor = LogicalToDynamicOffsets;
            for (const int32 DynVert : Mesh.VertexIndicesItr())
            {
                const int32 LogicalId = Topology.DynamicToLogicalVertex.IsValidIndex(DynVert)
                    ? Topology.DynamicToLogicalVertex[DynVert] : INDEX_NONE;
                if (LogicalId >= 0 && LogicalId < LogicalCount)
                {
                    LogicalToDynamicVertices[Cursor[LogicalId]++] = DynVert;
                }
            }
        }

        // 2. Logical -> Incident Edges CSR
        const int32 EdgeCount = Topology.Edges.Num();
        LogicalToEdgeOffsets.Init(0, LogicalCount + 1);
        for (int32 EdgeIdx = 0; EdgeIdx < EdgeCount; ++EdgeIdx)
        {
            const FIntPoint Edge = Topology.Edges[EdgeIdx];
            if (Edge.X >= 0 && Edge.X < LogicalCount)
            {
                ++LogicalToEdgeOffsets[Edge.X + 1];
            }
            if (Edge.Y >= 0 && Edge.Y < LogicalCount && Edge.Y != Edge.X)
            {
                ++LogicalToEdgeOffsets[Edge.Y + 1];
            }
        }
        for (int32 i = 0; i < LogicalCount; ++i)
        {
            LogicalToEdgeOffsets[i + 1] += LogicalToEdgeOffsets[i];
        }
        LogicalToEdges.SetNumUninitialized(LogicalToEdgeOffsets[LogicalCount]);
        {
            TArray<int32> Cursor = LogicalToEdgeOffsets;
            for (int32 EdgeIdx = 0; EdgeIdx < EdgeCount; ++EdgeIdx)
            {
                const FIntPoint Edge = Topology.Edges[EdgeIdx];
                if (Edge.X >= 0 && Edge.X < LogicalCount)
                {
                    LogicalToEdges[Cursor[Edge.X]++] = EdgeIdx;
                }
                if (Edge.Y >= 0 && Edge.Y < LogicalCount && Edge.Y != Edge.X)
                {
                    LogicalToEdges[Cursor[Edge.Y]++] = EdgeIdx;
                }
            }
        }

        // 3. Flat triangle logical vertices & Logical -> Incident Triangles CSR for fast mesh-space vertex normals
        const int32 MaxTris = Mesh.TriangleCount();
        FlatTriLogicalVertices.Reset(MaxTris * 3);
        LogicalToTriOffsets.Init(0, LogicalCount + 1);
        for (const int32 TriId : Mesh.TriangleIndicesItr())
        {
            const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(TriId);
            const int32 L0 = Topology.DynamicToLogicalVertex.IsValidIndex(Tri.A) ? Topology.DynamicToLogicalVertex[Tri.A] : INDEX_NONE;
            const int32 L1 = Topology.DynamicToLogicalVertex.IsValidIndex(Tri.B) ? Topology.DynamicToLogicalVertex[Tri.B] : INDEX_NONE;
            const int32 L2 = Topology.DynamicToLogicalVertex.IsValidIndex(Tri.C) ? Topology.DynamicToLogicalVertex[Tri.C] : INDEX_NONE;
            if (L0 >= 0 && L0 < LogicalCount && L1 >= 0 && L1 < LogicalCount && L2 >= 0 && L2 < LogicalCount)
            {
                FlatTriLogicalVertices.Add(L0);
                FlatTriLogicalVertices.Add(L1);
                FlatTriLogicalVertices.Add(L2);
                ++LogicalToTriOffsets[L0 + 1];
                if (L1 != L0) ++LogicalToTriOffsets[L1 + 1];
                if (L2 != L0 && L2 != L1) ++LogicalToTriOffsets[L2 + 1];
            }
        }
        const int32 ValidTriCount = FlatTriLogicalVertices.Num() / 3;
        for (int32 i = 0; i < LogicalCount; ++i)
        {
            LogicalToTriOffsets[i + 1] += LogicalToTriOffsets[i];
        }
        LogicalToTris.SetNumUninitialized(LogicalToTriOffsets[LogicalCount]);
        {
            TArray<int32> Cursor = LogicalToTriOffsets;
            for (int32 t = 0; t < ValidTriCount; ++t)
            {
                const int32 L0 = FlatTriLogicalVertices[t * 3 + 0];
                const int32 L1 = FlatTriLogicalVertices[t * 3 + 1];
                const int32 L2 = FlatTriLogicalVertices[t * 3 + 2];
                LogicalToTris[Cursor[L0]++] = t;
                if (L1 != L0) LogicalToTris[Cursor[L1]++] = t;
                if (L2 != L0 && L2 != L1) LogicalToTris[Cursor[L2]++] = t;
            }
        }

        // Compute adaptive normal offset from mesh bounds and average edge length so vertices sit cleanly above the mesh surface
        double EdgeSum = 0.0;
        int32 SampledEdges = 0;
        const int32 Step = FMath::Max(1, EdgeCount / 512);
        for (int32 e = 0; e < EdgeCount; e += Step)
        {
            const FIntPoint Edge = Topology.Edges[e];
            if (Topology.RestPositions.IsValidIndex(Edge.X) && Topology.RestPositions.IsValidIndex(Edge.Y))
            {
                EdgeSum += FVector::Distance(Topology.RestPositions[Edge.X], Topology.RestPositions[Edge.Y]);
                ++SampledEdges;
            }
        }
        const float AvgEdge = SampledEdges > 0 ? static_cast<float>(EdgeSum / SampledEdges) : 1.0f;
        const float BoundsMax = Topology.RestBounds.IsValid ? static_cast<float>(Topology.RestBounds.GetSize().GetMax()) : 100.0f;
        VertexNormalOffset = 0.0f; // Keep overlays flush on mesh; rely on shader DepthBias for Z-ordering

        // Precompute initial mesh-space vertex tangents from incident polygon perimeters
        LogicalVertexTangents.Init(FVector3f(1.0f, 0.0f, 0.0f), LogicalCount);
        for (const TPair<int32, FNGonFace>& Pair : Topology.Faces)
        {
            const TArray<int32>& Perim = Pair.Value.Perimeter;
            const int32 NumCorners = Perim.Num();
            for (int32 c = 0; c < NumCorners; ++c)
            {
                const int32 CurrV = Perim[c];
                const int32 NextV = Perim[(c + 1) % NumCorners];
                if (Topology.RestPositions.IsValidIndex(CurrV) && Topology.RestPositions.IsValidIndex(NextV))
                {
                    LogicalVertexTangents[CurrV] += FVector3f(Topology.RestPositions[NextV] - Topology.RestPositions[CurrV]).GetSafeNormal();
                }
            }
        }

        RecomputeAllVertexNormals(Topology.RestPositions);

        VertexStamp.Init(0, LogicalCount);
        EdgeStamp.Init(0, EdgeCount);
        DirtyVertices.Reset();
        DirtyEdges.Reset();
        PreviousSessionVertices.Reset();
        bEdgesOverlayDirty = false;
        bVerticesOverlayDirty = false;
        CachedSoftPointCount = 0;
        CachedSelectedVertexCount = 0;
    }

    void RecomputeAllVertexNormals(const TArray<FVector>& Positions)
    {
        const int32 LogicalCount = Positions.Num();
        LogicalVertexNormals.SetNumUninitialized(LogicalCount);
        DisplacedVertexPositions.SetNumUninitialized(LogicalCount);
        if (LogicalCount == 0)
        {
            return;
        }

        FMemory::Memzero(LogicalVertexNormals.GetData(), LogicalCount * sizeof(FVector3f));
        const FVector* RESTRICT PosData = Positions.GetData();
        FVector3f* RESTRICT NormData = LogicalVertexNormals.GetData();
        const int32* RESTRICT TriData = FlatTriLogicalVertices.GetData();
        const int32 NumTris = FlatTriLogicalVertices.Num() / 3;

        for (int32 t = 0; t < NumTris; ++t)
        {
            const int32 I0 = TriData[t * 3 + 0];
            const int32 I1 = TriData[t * 3 + 1];
            const int32 I2 = TriData[t * 3 + 2];
            const FVector3f P0(PosData[I0]);
            const FVector3f P1(PosData[I1]);
            const FVector3f P2(PosData[I2]);
            const FVector3f FaceN = FVector3f::CrossProduct(P1 - P0, P2 - P0);
            NormData[I0] += FaceN;
            NormData[I1] += FaceN;
            NormData[I2] += FaceN;
        }

        FVector* RESTRICT DispData = DisplacedVertexPositions.GetData();
        const float Offset = VertexNormalOffset;
        for (int32 v = 0; v < LogicalCount; ++v)
        {
            FVector3f N = NormData[v];
            if (!N.Normalize(1.e-12f))
            {
                N = FVector3f(0.0f, 0.0f, 1.0f);
            }
            NormData[v] = N;
            DispData[v] = PosData[v] + FVector(N) * Offset;
        }
    }

    void RecomputeDirtyVertexNormals(TConstArrayView<int32> DirtyVerts, const TArray<FVector>& Positions)
    {
        const int32 LogicalCount = Positions.Num();
        if (LogicalVertexNormals.Num() != LogicalCount || DisplacedVertexPositions.Num() != LogicalCount)
        {
            RecomputeAllVertexNormals(Positions);
            return;
        }

        const FVector* RESTRICT PosData = Positions.GetData();
        FVector3f* RESTRICT NormData = LogicalVertexNormals.GetData();
        FVector* RESTRICT DispData = DisplacedVertexPositions.GetData();
        const int32* RESTRICT TriData = FlatTriLogicalVertices.GetData();
        const int32* RESTRICT Offsets = LogicalToTriOffsets.GetData();
        const int32* RESTRICT Tris = LogicalToTris.GetData();
        const float Offset = VertexNormalOffset;

        for (const int32 v : DirtyVerts)
        {
            if (v < 0 || v >= LogicalCount)
            {
                continue;
            }
            FVector3f SumN = FVector3f::ZeroVector;
            if (v + 1 < LogicalToTriOffsets.Num())
            {
                const int32 StartT = Offsets[v];
                const int32 EndT = Offsets[v + 1];
                for (int32 i = StartT; i < EndT; ++i)
                {
                    const int32 t = Tris[i];
                    const int32 I0 = TriData[t * 3 + 0];
                    const int32 I1 = TriData[t * 3 + 1];
                    const int32 I2 = TriData[t * 3 + 2];
                    const FVector3f P0(PosData[I0]);
                    const FVector3f P1(PosData[I1]);
                    const FVector3f P2(PosData[I2]);
                    SumN += FVector3f::CrossProduct(P1 - P0, P2 - P0);
                }
            }
            if (!SumN.Normalize(1.e-12f))
            {
                SumN = FVector3f(0.0f, 0.0f, 1.0f);
            }
            NormData[v] = SumN;
            DispData[v] = PosData[v] + FVector(SumN) * Offset;
        }
    }

    FORCEINLINE FVector GetVertexNormal(int32 LogicalId) const
    {
        return LogicalVertexNormals.IsValidIndex(LogicalId)
            ? FVector(LogicalVertexNormals[LogicalId])
            : FVector::UpVector;
    }

    FORCEINLINE FVector GetVertexTangent(int32 LogicalId) const
    {
        return LogicalVertexTangents.IsValidIndex(LogicalId)
            ? FVector(LogicalVertexTangents[LogicalId])
            : FVector::ForwardVector;
    }

    FORCEINLINE FVector GetDisplacedVertexPosition(const TArray<FVector>& Positions, int32 LogicalId, float Scale = 1.0f) const
    {
        if (!Positions.IsValidIndex(LogicalId))
        {
            return FVector::ZeroVector;
        }
        if (FMath::IsNearlyEqual(Scale, 1.0f) && DisplacedVertexPositions.IsValidIndex(LogicalId))
        {
            return DisplacedVertexPositions[LogicalId];
        }
        return Positions[LogicalId] + GetVertexNormal(LogicalId) * (VertexNormalOffset * Scale);
    }

    // Returns 1.0f at the near point to the camera and scales smoothly down to 2/3 (0.6667f) at the far point or when zoomed out
    FORCEINLINE float GetVertexCameraDepthScale(const FVector& VertexPos) const
    {
        if (!bHasCameraView)
        {
            return 1.0f;
        }
        const float Dist = static_cast<float>(FVector::Distance(VertexPos, LastCameraOrigin));
        const float Span = FMath::Max(FarCameraDist - NearCameraDist, 1.0f);
        const float DepthT = FMath::Clamp((Dist - NearCameraDist) / Span, 0.0f, 1.0f);
        constexpr float FarMinScale = 2.0f / 3.0f;
        const float DepthScale = FMath::Lerp(1.0f, FarMinScale, DepthT);
        return FMath::Clamp(DepthScale * CameraZoomScale, FarMinScale, 1.0f);
    }

    void BuildColorTables(int32 LogicalCount, int32 NumColorElements, TFunctionRef<int32(int32)> GetLogicalVertex)
    {
        LogicalToColorOffsets.Init(0, LogicalCount + 1);
        for (int32 ElemIdx = 0; ElemIdx < NumColorElements; ++ElemIdx)
        {
            const int32 LogicalId = GetLogicalVertex(ElemIdx);
            if (LogicalId >= 0 && LogicalId < LogicalCount)
            {
                ++LogicalToColorOffsets[LogicalId + 1];
            }
        }
        for (int32 i = 0; i < LogicalCount; ++i)
        {
            LogicalToColorOffsets[i + 1] += LogicalToColorOffsets[i];
        }
        LogicalToColorElements.SetNumUninitialized(LogicalToColorOffsets[LogicalCount]);
        TArray<int32> Cursor = LogicalToColorOffsets;
        for (int32 ElemIdx = 0; ElemIdx < NumColorElements; ++ElemIdx)
        {
            const int32 LogicalId = GetLogicalVertex(ElemIdx);
            if (LogicalId >= 0 && LogicalId < LogicalCount)
            {
                LogicalToColorElements[Cursor[LogicalId]++] = ElemIdx;
            }
        }
    }
};


FNGonSceneAccel& GetSceneAccel(const FNGonPreviewScene* Scene);
void RemoveSceneAccel(const FNGonPreviewScene* Scene);
} // namespace NGon

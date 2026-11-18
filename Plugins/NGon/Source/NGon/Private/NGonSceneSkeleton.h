#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "NGonTopology.h"

class FNGonPreviewScene;
class FPrimitiveDrawInterface;
class FSceneView;
class FViewport;
class FCanvas;
class UCombinedTransformGizmo;
class UInteractiveGizmoManager;

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
void SetSceneFlatShading(FNGonPreviewScene* Scene, bool bFlatShading);
bool IsSceneFlatShading(const FNGonPreviewScene* Scene);
bool HasActiveSceneGizmoTarget(const FNGonPreviewScene* Scene);
bool IsUtilityNonDeformingBoneName(const FName& Name);
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
    bool bFlatShading = true;
    bool bPendingShadingUpdate = false;
    bool bHasActiveGizmoTarget = false;

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

        VertexStamp.Init(0, LogicalCount);
        EdgeStamp.Init(0, EdgeCount);
        DirtyVertices.Reset();
        DirtyEdges.Reset();
        PreviousSessionVertices.Reset();
        bEdgesOverlayDirty = false;
        bVerticesOverlayDirty = false;
        CachedSoftPointCount = 0;
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
void BindGizmoActorViewContext(
    UCombinedTransformGizmo* TransformGizmo,
    UInteractiveGizmoManager* GizmoManager,
    bool bMarkRenderStateDirty);
} // namespace NGon

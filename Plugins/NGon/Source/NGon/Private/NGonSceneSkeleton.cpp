#include "NGonSceneSkeleton.h"
#include "NGonPreviewScene.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "UnrealClient.h"

namespace
{
TMap<const FNGonPreviewScene*, NGon::FNGonSceneAccel> GSceneAccelMap;
}

namespace NGon
{
FNGonSceneAccel& GetSceneAccel(const FNGonPreviewScene* Scene)
{
    return GSceneAccelMap.FindOrAdd(Scene);
}

void RemoveSceneAccel(const FNGonPreviewScene* Scene)
{
    GSceneAccelMap.Remove(Scene);
}

} // namespace NGon

namespace
{
double NGonBonePointToSegment3DDistSq(const FVector& P, const FVector& A, const FVector& B)
{
    const FVector AB = B - A;
    const double LenSq = AB.SizeSquared();
    if (LenSq <= 1e-8)
    {
        return FVector::DistSquared(P, A);
    }
    const double T = FMath::Clamp(FVector::DotProduct(P - A, AB) / LenSq, 0.0, 1.0);
    return FVector::DistSquared(P, A + AB * T);
}

double NGonBonePointToSegment2DDistSq(const FVector2D& P, const FVector2D& A, const FVector2D& B)
{
    const FVector2D AB = B - A;
    const double LenSq = AB.SizeSquared();
    if (LenSq <= 1e-6)
    {
        return FVector2D::DistSquared(P, A);
    }
    const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / LenSq, 0.0, 1.0);
    return FVector2D::DistSquared(P, A + AB * T);
}

bool NGonIsUtilityNonDeformingBoneName(const FName& Name)
{
    const FString S = Name.ToString().ToLower();
    return S == TEXT("root")
        || S == TEXT("center_of_mass")
        || S == TEXT("interaction")
        || S.StartsWith(TEXT("ik_"))
        || S.StartsWith(TEXT("vb_"))
        || S.Contains(TEXT("_ik"));
}

void NGonDrawWireJointSphere(
    FPrimitiveDrawInterface* PDI,
    const FVector& Center,
    const FQuat& Rotation,
    float Radius,
    const FLinearColor& Color,
    float Thickness)
{
    constexpr int32 Segments = 16;
    const FVector AxisX = Rotation.RotateVector(FVector::UnitX());
    const FVector AxisY = Rotation.RotateVector(FVector::UnitY());
    const FVector AxisZ = Rotation.RotateVector(FVector::UnitZ());

    for (int32 i = 0; i < Segments; ++i)
    {
        const float A0 = (static_cast<float>(i) / static_cast<float>(Segments)) * 2.0f * PI;
        const float A1 = (static_cast<float>(i + 1) / static_cast<float>(Segments)) * 2.0f * PI;
        const float C0 = FMath::Cos(A0) * Radius;
        const float S0 = FMath::Sin(A0) * Radius;
        const float C1 = FMath::Cos(A1) * Radius;
        const float S1 = FMath::Sin(A1) * Radius;

        PDI->DrawLine(Center + AxisX * C0 + AxisY * S0, Center + AxisX * C1 + AxisY * S1, Color, SDPG_Foreground, Thickness, 0.5f, true);
        PDI->DrawLine(Center + AxisY * C0 + AxisZ * S0, Center + AxisY * C1 + AxisZ * S1, Color, SDPG_Foreground, Thickness, 0.5f, true);
        PDI->DrawLine(Center + AxisX * C0 + AxisZ * S0, Center + AxisX * C1 + AxisZ * S1, Color, SDPG_Foreground, Thickness, 0.5f, true);
    }
}

void NGonDrawWireBoneOctahedron(
    FPrimitiveDrawInterface* PDI,
    const FVector& StartPos,
    const FVector& EndPos,
    const FLinearColor& Color,
    float Thickness,
    float BaseRadiusOverride = 0.0f)
{
    const FVector Delta = EndPos - StartPos;
    const double Length = Delta.Size();
    if (Length <= 1e-3)
    {
        return;
    }

    const FVector Dir = Delta / Length;
    const FVector Up = FMath::Abs(Dir.Z) < 0.92 ? FVector::UpVector : FVector::RightVector;
    const FVector SideA = FVector::CrossProduct(Dir, Up).GetSafeNormal();
    const FVector SideB = FVector::CrossProduct(Dir, SideA).GetSafeNormal();

    // Match native Unreal Engine FAnimationViewportClient::DrawBones / SkeletalDebugRendering::DrawWireBone:
    // A 4-sided pyramid whose base ring sits right on the joint sphere at StartPos and tapers to a sharp tip at EndPos.
    const double BaseRadius = (BaseRadiusOverride > 0.0f)
        ? static_cast<double>(BaseRadiusOverride)
        : FMath::Clamp(Length * 0.055, 0.45, 1.35);
    const FVector BaseCenter = StartPos + Dir * FMath::Min(BaseRadius * 0.35, Length * 0.15);

    const FVector C0 = BaseCenter + (SideA + SideB) * (BaseRadius * 0.7071);
    const FVector C1 = BaseCenter + (SideA - SideB) * (BaseRadius * 0.7071);
    const FVector C2 = BaseCenter + (-SideA - SideB) * (BaseRadius * 0.7071);
    const FVector C3 = BaseCenter + (-SideA + SideB) * (BaseRadius * 0.7071);

    // Base struts from joint origin to pyramid base corners
    PDI->DrawLine(StartPos, C0, Color, SDPG_Foreground, Thickness, 0.5f, true);
    PDI->DrawLine(StartPos, C1, Color, SDPG_Foreground, Thickness, 0.5f, true);
    PDI->DrawLine(StartPos, C2, Color, SDPG_Foreground, Thickness, 0.5f, true);
    PDI->DrawLine(StartPos, C3, Color, SDPG_Foreground, Thickness, 0.5f, true);

    // Base square ring around the joint sphere
    PDI->DrawLine(C0, C1, Color, SDPG_Foreground, Thickness, 0.5f, true);
    PDI->DrawLine(C1, C2, Color, SDPG_Foreground, Thickness, 0.5f, true);
    PDI->DrawLine(C2, C3, Color, SDPG_Foreground, Thickness, 0.5f, true);
    PDI->DrawLine(C3, C0, Color, SDPG_Foreground, Thickness, 0.5f, true);

    // Long sleek pyramid edges tapering directly to the child joint tip (EndPos)
    PDI->DrawLine(C0, EndPos, Color, SDPG_Foreground, Thickness, 0.5f, true);
    PDI->DrawLine(C1, EndPos, Color, SDPG_Foreground, Thickness, 0.5f, true);
    PDI->DrawLine(C2, EndPos, Color, SDPG_Foreground, Thickness, 0.5f, true);
    PDI->DrawLine(C3, EndPos, Color, SDPG_Foreground, Thickness, 0.5f, true);
}
} // anonymous namespace

namespace NGon
{
void SetSceneSkeleton(
    FNGonPreviewScene* Scene,
    const TArray<FNGonSceneBoneInit>& InBones,
    const TArray<TArray<FNGonVertexBoneWeight>>& InVertexWeights)
{
    if (!Scene)
    {
        return;
    }

    FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    Accel.Bones.Reset(InBones.Num());
    Accel.SelectedBoneIndex = INDEX_NONE;
    Accel.bBoneGizmoActive = false;
    Accel.bBoneDragActive = false;
    Accel.bPendingSkinningUpdate = false;
    Accel.bPendingFullSkinReset = false;

    for (int32 i = 0; i < InBones.Num(); ++i)
    {
        FNGonSceneAccel::FSceneBoneState State;
        State.BoneIndex = i;
        State.ParentIndex = InBones[i].ParentIndex;
        State.Name = InBones[i].Name;
        State.RefLocalTransform = InBones[i].RefLocalTransform;
        State.RefComponentTransform = InBones[i].RefComponentTransform;
        State.CurrentLocalTransform = InBones[i].RefLocalTransform;
        State.CurrentComponentTransform = InBones[i].RefComponentTransform;
        State.InvRefComponentMatrix = State.RefComponentTransform.ToInverseMatrixWithScale();
        State.SkinMatrix = FMatrix::Identity;
        Accel.Bones.Add(State);
    }

    for (int32 i = 0; i < Accel.Bones.Num(); ++i)
    {
        const int32 ParentIdx = Accel.Bones[i].ParentIndex;
        if (ParentIdx >= 0 && Accel.Bones.IsValidIndex(ParentIdx) && ParentIdx != i)
        {
            Accel.Bones[ParentIdx].ChildBones.Add(i);
        }
    }

    const FNGonTopology& Topo = Scene->GetTopology();
    const int32 LogicalCount = Topo.RestPositions.Num();
    Accel.SkinnedBasePositions = Topo.RestPositions;

    // Build per-vertex skin weights (using authored weights when provided, and smooth bone-segment envelope fallback otherwise)
    TArray<int32> DeformingBones;
    DeformingBones.Reserve(Accel.Bones.Num());
    for (int32 b = 0; b < Accel.Bones.Num(); ++b)
    {
        if (!NGonIsUtilityNonDeformingBoneName(Accel.Bones[b].Name))
        {
            DeformingBones.Add(b);
        }
    }
    if (DeformingBones.IsEmpty())
    {
        for (int32 b = 0; b < Accel.Bones.Num(); ++b)
        {
            DeformingBones.Add(b);
        }
    }

    Accel.VertexSkinOffsets.Init(0, LogicalCount + 1);
    Accel.VertexSkinWeights.Reset();
    Accel.VertexSkinWeights.Reserve(LogicalCount * 4);

    for (int32 v = 0; v < LogicalCount; ++v)
    {
        bool bAddedAuthored = false;
        if (InVertexWeights.IsValidIndex(v) && !InVertexWeights[v].IsEmpty())
        {
            float SumW = 0.0f;
            for (const FNGonVertexBoneWeight& BW : InVertexWeights[v])
            {
                if (Accel.Bones.IsValidIndex(BW.BoneIndex) && BW.Weight > 1e-4f)
                {
                    SumW += BW.Weight;
                }
            }
            if (SumW > 1e-4f)
            {
                for (const FNGonVertexBoneWeight& BW : InVertexWeights[v])
                {
                    if (Accel.Bones.IsValidIndex(BW.BoneIndex) && BW.Weight > 1e-4f)
                    {
                        Accel.VertexSkinWeights.Add({ BW.BoneIndex, BW.Weight / SumW });
                    }
                }
                bAddedAuthored = true;
            }
        }

        if (!bAddedAuthored && !DeformingBones.IsEmpty())
        {
            const FVector& Pos = Topo.RestPositions[v];
            struct FCandidate
            {
                int32 BoneIndex = INDEX_NONE;
                double DistSq = TNumericLimits<double>::Max();
            };
            constexpr int32 MaxInf = 4;
            FCandidate Best[MaxInf];

            for (const int32 b : DeformingBones)
            {
                const FVector BoneStart = Accel.Bones[b].RefComponentTransform.GetLocation();
                double BoneDistSq = FVector::DistSquared(Pos, BoneStart);
                for (const int32 ChildIdx : Accel.Bones[b].ChildBones)
                {
                    if (!NGonIsUtilityNonDeformingBoneName(Accel.Bones[ChildIdx].Name))
                    {
                        const FVector BoneEnd = Accel.Bones[ChildIdx].RefComponentTransform.GetLocation();
                        BoneDistSq = FMath::Min(BoneDistSq, NGonBonePointToSegment3DDistSq(Pos, BoneStart, BoneEnd));
                    }
                }

                for (int32 Slot = 0; Slot < MaxInf; ++Slot)
                {
                    if (BoneDistSq < Best[Slot].DistSq)
                    {
                        for (int32 Shift = MaxInf - 1; Shift > Slot; --Shift)
                        {
                            Best[Shift] = Best[Shift - 1];
                        }
                        Best[Slot] = { b, BoneDistSq };
                        break;
                    }
                }
            }

            float SumW = 0.0f;
            FNGonVertexBoneWeight LocalWeights[MaxInf];
            int32 ValidCount = 0;
            for (int32 Slot = 0; Slot < MaxInf; ++Slot)
            {
                if (Best[Slot].BoneIndex != INDEX_NONE)
                {
                    const double Dist = FMath::Sqrt(FMath::Max(0.0, Best[Slot].DistSq));
                    const float W = static_cast<float>(1.0 / FMath::Pow(Dist + 2.0, 3.0));
                    LocalWeights[ValidCount++] = { Best[Slot].BoneIndex, W };
                    SumW += W;
                }
            }
            if (SumW > 1e-6f)
            {
                for (int32 i = 0; i < ValidCount; ++i)
                {
                    Accel.VertexSkinWeights.Add({ LocalWeights[i].BoneIndex, LocalWeights[i].Weight / SumW });
                }
            }
        }

        Accel.VertexSkinOffsets[v + 1] = Accel.VertexSkinWeights.Num();
    }

    // Build reverse CSR mapping: Bone -> Influenced Logical Vertices
    const int32 NumBones = Accel.Bones.Num();
    Accel.BoneToVertexOffsets.Init(0, NumBones + 1);
    for (int32 v = 0; v < LogicalCount; ++v)
    {
        const int32 StartW = Accel.VertexSkinOffsets[v];
        const int32 EndW = Accel.VertexSkinOffsets[v + 1];
        for (int32 wIdx = StartW; wIdx < EndW; ++wIdx)
        {
            const int32 BoneIdx = Accel.VertexSkinWeights[wIdx].BoneIndex;
            if (BoneIdx >= 0 && BoneIdx < NumBones)
            {
                ++Accel.BoneToVertexOffsets[BoneIdx + 1];
            }
        }
    }
    for (int32 b = 0; b < NumBones; ++b)
    {
        Accel.BoneToVertexOffsets[b + 1] += Accel.BoneToVertexOffsets[b];
    }
    Accel.BoneToVertices.SetNumUninitialized(Accel.BoneToVertexOffsets[NumBones]);
    {
        TArray<int32> Cursor = Accel.BoneToVertexOffsets;
        for (int32 v = 0; v < LogicalCount; ++v)
        {
            const int32 StartW = Accel.VertexSkinOffsets[v];
            const int32 EndW = Accel.VertexSkinOffsets[v + 1];
            for (int32 wIdx = StartW; wIdx < EndW; ++wIdx)
            {
                const int32 BoneIdx = Accel.VertexSkinWeights[wIdx].BoneIndex;
                if (BoneIdx >= 0 && BoneIdx < NumBones)
                {
                    Accel.BoneToVertices[Cursor[BoneIdx]++] = v;
                }
            }
        }
    }
}

void ClearSceneSkeleton(FNGonPreviewScene* Scene)
{
    if (!Scene)
    {
        return;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    Accel.Bones.Reset();
    Accel.SkinnedBasePositions.Reset();
    Accel.VertexSkinOffsets.Reset();
    Accel.VertexSkinWeights.Reset();
    Accel.BoneToVertexOffsets.Reset();
    Accel.BoneToVertices.Reset();
    Accel.SelectedBoneIndex = INDEX_NONE;
    Accel.bBoneGizmoActive = false;
    Accel.bBoneDragActive = false;
    Accel.bPendingSkinningUpdate = false;
    Accel.bPendingFullSkinReset = false;
}

bool HasSceneBones(const FNGonPreviewScene* Scene)
{
    return Scene && !GetSceneAccel(Scene).Bones.IsEmpty();
}

int32 GetSceneBoneCount(const FNGonPreviewScene* Scene)
{
    return Scene ? GetSceneAccel(Scene).Bones.Num() : 0;
}

void SelectSceneBone(FNGonPreviewScene* Scene, int32 BoneIndex, bool bActivateBoneGizmo)
{
    if (!Scene)
    {
        return;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    if (Accel.Bones.IsValidIndex(BoneIndex))
    {
        Accel.SelectedBoneIndex = BoneIndex;
        Accel.bBoneGizmoActive = bActivateBoneGizmo;
    }
    else
    {
        Accel.SelectedBoneIndex = INDEX_NONE;
        Accel.bBoneGizmoActive = false;
    }
}

int32 GetSelectedSceneBone(const FNGonPreviewScene* Scene)
{
    return Scene ? GetSceneAccel(Scene).SelectedBoneIndex : INDEX_NONE;
}

bool IsBoneGizmoActive(const FNGonPreviewScene* Scene)
{
    if (!Scene)
    {
        return false;
    }
    const FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    return Accel.bBoneGizmoActive && Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex);
}

void SetBoneGizmoActive(FNGonPreviewScene* Scene, bool bActive)
{
    if (!Scene)
    {
        return;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    if (bActive && !Accel.Bones.IsEmpty() && !Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex))
    {
        Accel.SelectedBoneIndex = 0;
    }
    Accel.bBoneGizmoActive = bActive && Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex);
}

bool IsBoneTransformActive(const FNGonPreviewScene* Scene)
{
    return Scene && GetSceneAccel(Scene).bBoneDragActive;
}

bool GetSceneBoneTransforms(
    const FNGonPreviewScene* Scene,
    int32 BoneIndex,
    FTransform& OutLocal,
    FTransform& OutComponent,
    FTransform& OutRefLocal,
    FTransform& OutRefComponent)
{
    if (!Scene)
    {
        return false;
    }
    const FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    if (!Accel.Bones.IsValidIndex(BoneIndex))
    {
        return false;
    }
    const FNGonSceneAccel::FSceneBoneState& Bone = Accel.Bones[BoneIndex];
    OutLocal = Bone.CurrentLocalTransform;
    OutComponent = Bone.CurrentComponentTransform;
    OutRefLocal = Bone.RefLocalTransform;
    OutRefComponent = Bone.RefComponentTransform;
    return true;
}

void SetSceneBoneLocalTransform(FNGonPreviewScene* Scene, int32 BoneIndex, const FTransform& NewLocalTransform)
{
    if (!Scene)
    {
        return;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    if (!Accel.Bones.IsValidIndex(BoneIndex))
    {
        return;
    }
    Accel.Bones[BoneIndex].CurrentLocalTransform = NewLocalTransform;
    Accel.bPendingSkinningUpdate = true;
    Scene->RefreshBoneDeformation();
}

bool ResetSelectedSceneBoneTransform(FNGonPreviewScene* Scene)
{
    if (!Scene)
    {
        return false;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    if (!Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex))
    {
        return false;
    }
    Accel.Bones[Accel.SelectedBoneIndex].CurrentLocalTransform = Accel.Bones[Accel.SelectedBoneIndex].RefLocalTransform;
    Accel.bPendingSkinningUpdate = true;
    Scene->RefreshBoneDeformation();
    return true;
}

bool ResetAllSceneBonesTransforms(FNGonPreviewScene* Scene)
{
    if (!Scene)
    {
        return false;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    if (Accel.Bones.IsEmpty())
    {
        return false;
    }
    for (FNGonSceneAccel::FSceneBoneState& Bone : Accel.Bones)
    {
        Bone.CurrentLocalTransform = Bone.RefLocalTransform;
        Bone.CurrentComponentTransform = Bone.RefComponentTransform;
        Bone.SkinMatrix = FMatrix::Identity;
    }
    Accel.bPendingFullSkinReset = true;
    Accel.bPendingSkinningUpdate = true;
    Scene->RefreshBoneDeformation();
    return true;
}

int32 PickSceneBone(
    FNGonPreviewScene* Scene,
    const FSceneView& View,
    const FVector2D& ScreenCursor,
    float MaxPixelDist,
    bool bSelectOnHit)
{
    if (!Scene)
    {
        return INDEX_NONE;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    if (Accel.Bones.IsEmpty())
    {
        return INDEX_NONE;
    }

    int32 BestJointBone = INDEX_NONE;
    double BestJointDistSq = FMath::Square(static_cast<double>(MaxPixelDist));

    int32 BestSegmentBone = INDEX_NONE;
    double BestSegmentDistSq = FMath::Square(static_cast<double>(MaxPixelDist * 0.72f));

    for (int32 b = 0; b < Accel.Bones.Num(); ++b)
    {
        const FVector JointWorld = Accel.Bones[b].CurrentComponentTransform.GetLocation();
        const FPlane Proj = View.Project(JointWorld);
        if (Proj.W <= 0.0f)
        {
            continue;
        }
        FVector2D JointScreen;
        if (!View.WorldToPixel(JointWorld, JointScreen))
        {
            continue;
        }

        const double JointDistSq = FVector2D::DistSquared(ScreenCursor, JointScreen);
        // Slightly prioritize deforming body bones over coinciding root/IK utility joints
        const double Penalty = NGonIsUtilityNonDeformingBoneName(Accel.Bones[b].Name) ? 12.0 : 0.0;
        if (JointDistSq + Penalty < BestJointDistSq)
        {
            BestJointDistSq = JointDistSq + Penalty;
            BestJointBone = b;
        }

        for (const int32 ChildIdx : Accel.Bones[b].ChildBones)
        {
            if (Accel.Bones.IsValidIndex(ChildIdx) && !NGonIsUtilityNonDeformingBoneName(Accel.Bones[ChildIdx].Name))
            {
                const FVector ChildWorld = Accel.Bones[ChildIdx].CurrentComponentTransform.GetLocation();
                if (View.Project(ChildWorld).W > 0.0f)
                {
                    FVector2D ChildScreen;
                    if (View.WorldToPixel(ChildWorld, ChildScreen))
                    {
                        const double SegDistSq = NGonBonePointToSegment2DDistSq(ScreenCursor, JointScreen, ChildScreen) + Penalty;
                        if (SegDistSq < BestSegmentDistSq)
                        {
                            BestSegmentDistSq = SegDistSq;
                            BestSegmentBone = b;
                        }
                    }
                }
            }
        }
    }

    const int32 HitBone = (BestJointBone != INDEX_NONE) ? BestJointBone : BestSegmentBone;
    if (HitBone != INDEX_NONE && bSelectOnHit)
    {
        SelectSceneBone(Scene, HitBone, true);
    }
    return HitBone;
}

namespace
{
TMap<const FNGonPreviewScene*, int32> GSceneBoneDrawModeMap;

bool ProjectWorldToCanvas(const FSceneView& View, double InvDPI, const FVector& WorldPos, FVector2D& OutCanvasPos)
{
    const FVector4 Clip = View.WorldToScreen(WorldPos);
    if (Clip.W <= 0.01 || !FMath::IsFinite(Clip.W))
    {
        return false;
    }
    FVector2D Px;
    if (!View.ScreenToPixel(Clip, Px) || !FMath::IsFinite(Px.X) || !FMath::IsFinite(Px.Y))
    {
        return false;
    }
    OutCanvasPos = Px * InvDPI;
    return true;
}

void DrawCanvasLine3D(
    FCanvas& Canvas,
    const FSceneView& View,
    double InvDPI,
    const FVector& A,
    const FVector& B,
    const FLinearColor& Color,
    float Thickness)
{
    FVector2D PA, PB;
    if (!ProjectWorldToCanvas(View, InvDPI, A, PA) || !ProjectWorldToCanvas(View, InvDPI, B, PB))
    {
        return;
    }
    FCanvasLineItem Line(PA, PB);
    Line.SetColor(Color);
    Line.LineThickness = Thickness;
    Canvas.DrawItem(Line);
}

void DrawCanvasJointSphere(
    FCanvas& Canvas,
    const FSceneView& View,
    double InvDPI,
    const FVector& Center,
    const FQuat& Rot,
    float Radius,
    const FLinearColor& Color,
    float Thickness)
{
    const FVector AxisX = Rot.RotateVector(FVector::UnitX()) * Radius;
    const FVector AxisY = Rot.RotateVector(FVector::UnitY()) * Radius;
    const FVector AxisZ = Rot.RotateVector(FVector::UnitZ()) * Radius;
    constexpr int32 Segs = 12;
    for (int32 i = 0; i < Segs; ++i)
    {
        const float A0 = (2.0f * UE_PI * i) / Segs;
        const float A1 = (2.0f * UE_PI * (i + 1)) / Segs;
        const float C0 = FMath::Cos(A0), S0 = FMath::Sin(A0);
        const float C1 = FMath::Cos(A1), S1 = FMath::Sin(A1);
        DrawCanvasLine3D(Canvas, View, InvDPI, Center + AxisX * C0 + AxisY * S0, Center + AxisX * C1 + AxisY * S1, Color, Thickness);
        DrawCanvasLine3D(Canvas, View, InvDPI, Center + AxisY * C0 + AxisZ * S0, Center + AxisY * C1 + AxisZ * S1, Color, Thickness);
        DrawCanvasLine3D(Canvas, View, InvDPI, Center + AxisX * C0 + AxisZ * S0, Center + AxisX * C1 + AxisZ * S1, Color, Thickness);
    }
}

void DrawCanvasBoneOctahedron(
    FCanvas& Canvas,
    const FSceneView& View,
    double InvDPI,
    const FVector& StartPos,
    const FVector& EndPos,
    const FLinearColor& Color,
    float Thickness,
    float BaseRadius)
{
    const FVector Delta = EndPos - StartPos;
    const double Len = Delta.Size();
    if (Len <= 0.25) return;
    const FVector Dir = Delta / Len;
    const FVector UpRef = FMath::Abs(Dir.Z) < 0.92 ? FVector::UpVector : FVector::RightVector;
    const FVector SideA = FVector::CrossProduct(Dir, UpRef).GetSafeNormal();
    const FVector SideB = FVector::CrossProduct(Dir, SideA).GetSafeNormal();
    const double R = FMath::Clamp(Len * 0.085, static_cast<double>(BaseRadius * 0.65f), static_cast<double>(BaseRadius * 1.45f));
    const FVector Mid = StartPos + Dir * FMath::Min(Len * 0.16, R * 2.2);

    const FVector C0 = Mid + SideA * R;
    const FVector C1 = Mid + SideB * R;
    const FVector C2 = Mid - SideA * R;
    const FVector C3 = Mid - SideB * R;

    DrawCanvasLine3D(Canvas, View, InvDPI, StartPos, C0, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, StartPos, C1, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, StartPos, C2, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, StartPos, C3, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, C0, C1, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, C1, C2, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, C2, C3, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, C3, C0, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, C0, EndPos, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, C1, EndPos, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, C2, EndPos, Color, Thickness);
    DrawCanvasLine3D(Canvas, View, InvDPI, C3, EndPos, Color, Thickness);
}

void DrawSkeletonOverlayOnCanvas(
    const FNGonPreviewScene* Scene,
    const FSceneView& View,
    FCanvas& Canvas,
    int32 BoneDrawMode)
{
    if (!Scene || BoneDrawMode == 3) return;
    const FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    if (Accel.Bones.IsEmpty()) return;

    const double InvDPI = 1.0 / FMath::Max(static_cast<double>(Canvas.GetDPIScale()), 0.01);
    const int32 SelectedIdx = Accel.SelectedBoneIndex;
    TSet<int32> SubtreeSet;
    if (Accel.Bones.IsValidIndex(SelectedIdx))
    {
        TArray<int32> Stack{ SelectedIdx };
        while (!Stack.IsEmpty())
        {
            const int32 Curr = Stack.Pop(EAllowShrinking::No);
            SubtreeSet.Add(Curr);
            for (const int32 Child : Accel.Bones[Curr].ChildBones)
            {
                Stack.Add(Child);
            }
        }
    }

    if (BoneDrawMode == 0 || BoneDrawMode == 1)
    {
        for (int32 b = 0; b < Accel.Bones.Num(); ++b)
        {
            if (b == SelectedIdx) continue;
            const bool bInSubtree = SubtreeSet.Contains(b);
            if (BoneDrawMode == 1 && !bInSubtree && b != (Accel.Bones.IsValidIndex(SelectedIdx) ? Accel.Bones[SelectedIdx].ParentIndex : INDEX_NONE))
            {
                continue;
            }
            const FNGonSceneAccel::FSceneBoneState& Bone = Accel.Bones[b];
            const FVector StartPos = Bone.CurrentComponentTransform.GetLocation();
            const bool bIsNonWeighted = NGonIsUtilityNonDeformingBoneName(Bone.Name);
            const FLinearColor WireColor = bInSubtree
                ? FLinearColor(0.96f, 0.68f, 0.18f, 0.95f)
                : (bIsNonWeighted
                    ? FLinearColor(0.42f, 0.48f, 0.56f, 0.55f)
                    : FLinearColor(0.76f, 0.66f, 0.34f, 0.85f));
            const float SphereRad = bInSubtree ? 0.95f : 0.70f;
            DrawCanvasJointSphere(Canvas, View, InvDPI, StartPos, Bone.CurrentComponentTransform.GetRotation(), SphereRad, WireColor, 1.1f);
            for (const int32 ChildIdx : Bone.ChildBones)
            {
                if (Accel.Bones.IsValidIndex(ChildIdx))
                {
                    DrawCanvasBoneOctahedron(Canvas, View, InvDPI, StartPos, Accel.Bones[ChildIdx].CurrentComponentTransform.GetLocation(), WireColor, 1.1f, SphereRad);
                }
            }
        }
    }

    if (Accel.Bones.IsValidIndex(SelectedIdx))
    {
        const FNGonSceneAccel::FSceneBoneState& SelBone = Accel.Bones[SelectedIdx];
        const FVector JointPos = SelBone.CurrentComponentTransform.GetLocation();
        const FQuat JointRot = SelBone.CurrentComponentTransform.GetRotation();
        const FLinearColor SelectedGreen(0.18f, 1.0f, 0.24f, 1.0f);

        float RefLen = 10.0f;
        for (const int32 ChildIdx : SelBone.ChildBones)
        {
            if (Accel.Bones.IsValidIndex(ChildIdx) && !NGonIsUtilityNonDeformingBoneName(Accel.Bones[ChildIdx].Name))
            {
                const float D = FVector::Distance(JointPos, Accel.Bones[ChildIdx].CurrentComponentTransform.GetLocation());
                if (D > 1.0f) { RefLen = D; break; }
            }
        }
        const float JointRadius = FMath::Clamp(RefLen * 0.085f, 0.85f, 1.65f);
        DrawCanvasJointSphere(Canvas, View, InvDPI, JointPos, JointRot, JointRadius, SelectedGreen, 1.5f);

        const float AxisLen = JointRadius * 0.92f;
        DrawCanvasLine3D(Canvas, View, InvDPI, JointPos, JointPos + JointRot.RotateVector(FVector::UnitX()) * AxisLen, FLinearColor(1.0f, 0.18f, 0.18f, 1.0f), 1.5f);
        DrawCanvasLine3D(Canvas, View, InvDPI, JointPos, JointPos + JointRot.RotateVector(FVector::UnitY()) * AxisLen, FLinearColor(0.18f, 1.0f, 0.18f, 1.0f), 1.5f);
        DrawCanvasLine3D(Canvas, View, InvDPI, JointPos, JointPos + JointRot.RotateVector(FVector::UnitZ()) * AxisLen, FLinearColor(0.22f, 0.52f, 1.0f, 1.0f), 1.5f);

        for (const int32 ChildIdx : SelBone.ChildBones)
        {
            if (Accel.Bones.IsValidIndex(ChildIdx))
            {
                const FVector ChildPos = Accel.Bones[ChildIdx].CurrentComponentTransform.GetLocation();
                if (FVector::DistSquared(JointPos, ChildPos) > 0.25f)
                {
                    DrawCanvasBoneOctahedron(Canvas, View, InvDPI, JointPos, ChildPos, SelectedGreen, 1.4f, JointRadius);
                }
            }
        }
    }
}
} // anonymous namespace

void DrawSceneSkeleton(
    const FNGonPreviewScene* Scene,
    const FSceneView* View,
    FPrimitiveDrawInterface* PDI,
    int32 BoneDrawMode)
{
    // Record BoneDrawMode and defer actual bone rendering to FCanvas in DrawSceneSkeletonLabels
    // so bones render cleanly post-3D/post-XRay/post-TSR with zero edge intersection flicker.
    if (Scene)
    {
        GSceneBoneDrawModeMap.Add(Scene, BoneDrawMode);
    }
}

void DrawSceneSkeletonLabels(
    const FNGonPreviewScene* Scene,
    const FSceneView* View,
    FViewport* InViewport,
    FCanvas* Canvas,
    bool bShowBoneNames)
{
    if (!Scene || !View || !InViewport || !Canvas)
    {
        return;
    }
    const int32* StoredDrawMode = GSceneBoneDrawModeMap.Find(Scene);
    const int32 BoneDrawMode = StoredDrawMode ? *StoredDrawMode : 0;
    DrawSkeletonOverlayOnCanvas(Scene, *View, *Canvas, BoneDrawMode);
    if (!bShowBoneNames || BoneDrawMode == 3)
    {
        return;
    }

    const FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    if (!Accel.Bones.IsValidIndex(Accel.SelectedBoneIndex))
    {
        return;
    }

    const FNGonSceneAccel::FSceneBoneState& SelBone = Accel.Bones[Accel.SelectedBoneIndex];
    const FVector JointPos = SelBone.CurrentComponentTransform.GetLocation();
    if (View->Project(JointPos).W <= 0.0f)
    {
        return;
    }

    FVector2D PixelPos;
    if (View->WorldToPixel(JointPos, PixelPos))
    {
        const float DPIScale = FMath::Max(Canvas->GetDPIScale(), 0.01f);
        const FVector2D CanvasPos = (PixelPos / DPIScale) + FVector2D(6.0f, 1.0f);
        if (GEngine && GEngine->GetSmallFont())
        {
            FCanvasTextItem TextItem(
                CanvasPos,
                FText::FromName(SelBone.Name),
                GEngine->GetSmallFont(),
                FLinearColor(0.95f, 0.96f, 0.98f, 1.0f));
            TextItem.EnableShadow(FLinearColor(0.0f, 0.0f, 0.0f, 0.95f), FVector2D(1.0f, 1.0f));
            Canvas->DrawItem(TextItem);
        }
    }
}

void SetSceneCoordinateSystem(FNGonPreviewScene* Scene, bool bUseWorldSpace)
{
    if (!Scene)
    {
        return;
    }
    GetSceneAccel(Scene).bUseWorldCoordinateSystem = bUseWorldSpace;
}

bool IsSceneWorldCoordinateSystem(const FNGonPreviewScene* Scene)
{
    return Scene && GetSceneAccel(Scene).bUseWorldCoordinateSystem;
}

void SetSceneSnapSettings(
    FNGonPreviewScene* Scene,
    bool bSnapGrid,
    float GridStep,
    bool bSnapRot,
    float RotStepDeg,
    bool bSnapScale,
    float ScaleStep)
{
    if (!Scene)
    {
        return;
    }
    FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    Accel.bSnapGrid = bSnapGrid;
    Accel.GridSnapStep = GridStep;
    Accel.bSnapRotation = bSnapRot;
    Accel.RotationSnapStep = RotStepDeg;
    Accel.bSnapScale = bSnapScale;
    Accel.ScaleSnapStep = ScaleStep;
}

void GetSceneSnapSettings(
    const FNGonPreviewScene* Scene,
    bool& OutSnapGrid,
    float& OutGridStep,
    bool& OutSnapRot,
    float& OutRotStepDeg,
    bool& OutSnapScale,
    float& OutScaleStep)
{
    if (!Scene)
    {
        OutSnapGrid = false;
        OutGridStep = 10.0f;
        OutSnapRot = false;
        OutRotStepDeg = 10.0f;
        OutSnapScale = false;
        OutScaleStep = 0.25f;
        return;
    }
    const FNGonSceneAccel& Accel = GetSceneAccel(Scene);
    OutSnapGrid = Accel.bSnapGrid;
    OutGridStep = Accel.GridSnapStep;
    OutSnapRot = Accel.bSnapRotation;
    OutRotStepDeg = Accel.RotationSnapStep;
    OutSnapScale = Accel.bSnapScale;
    OutScaleStep = Accel.ScaleSnapStep;
}

bool IsUtilityNonDeformingBoneName(const FName& Name)
{
    return NGonIsUtilityNonDeformingBoneName(Name);
}
} // namespace NGon
// NGon Unified ABI Sync v5.8.2

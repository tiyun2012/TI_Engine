#pragma once

#include "CoreMinimal.h"
#include "NGonSceneSkeleton.h"

struct FNGonMeshData;

namespace NGon
{
    struct FNGonFbxMorphInfo
    {
        FName Name = NAME_None;
        int32 VertexDeltaCount = 0;
    };

    struct FNGonFbxSkeletonData
    {
        TArray<FNGonSceneBoneInit> Bones;
        TArray<TArray<FNGonVertexBoneWeight>> VertexSkinWeights;
        TArray<FNGonFbxMorphInfo> MorphTargets;
    };

    bool LoadFbx(const FString& Filename, FNGonMeshData& Out, FText& Error);
    bool LoadFbxWithSkeleton(
        const FString& Filename,
        FNGonMeshData& OutMesh,
        FNGonFbxSkeletonData& OutSkeleton,
        FText& Error);
}

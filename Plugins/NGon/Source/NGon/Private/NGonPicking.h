#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"

class FSceneView;
struct FNGonTopology;

enum class ENGonElement : uint8
{
    Face,
    Edge,
    Vertex
};

class FNGonPicker
{
public:
    void SetMesh(const UE::Geometry::FDynamicMesh3* InMesh);
    void PositionsChanged();
    int32 PickFace(const FVector& Origin, const FVector& Direction) const;
    int32 Pick(
        ENGonElement Element,
        const FSceneView& View,
        const FVector2D& Cursor,
        const FNGonTopology& Topology,
        const TArray<FVector>& Positions) const;

private:
    const UE::Geometry::FDynamicMesh3* Mesh = nullptr;
    TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Spatial;
};

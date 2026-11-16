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

    /** Project and select all elements touched by a screen-space marquee. */
    void BoxPick(
        ENGonElement Element,
        const FSceneView& View,
        const FBox2D& ScreenRect,
        const FNGonTopology& Topology,
        const TArray<FVector>& Positions,
        TSet<int32>& OutIds,
        bool bFullyContained) const;

private:
    void EnsureSpatialBuilt() const;
    const UE::Geometry::FDynamicMesh3* Mesh = nullptr;
    mutable TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Spatial;
    mutable bool bSpatialDirty = false;
};

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

enum class ENGonSelectionMask : uint8
{
    None   = 0,
    Vertex = 1 << 0,
    Edge   = 1 << 1,
    Face   = 1 << 2,
    All    = Vertex | Edge | Face
};
ENUM_CLASS_FLAGS(ENGonSelectionMask);

inline ENGonSelectionMask NGonElementToMask(ENGonElement Element)
{
    switch (Element)
    {
    case ENGonElement::Vertex: return ENGonSelectionMask::Vertex;
    case ENGonElement::Edge:   return ENGonSelectionMask::Edge;
    case ENGonElement::Face:   return ENGonSelectionMask::Face;
    default:                   return ENGonSelectionMask::None;
    }
}

struct FNGonSurfaceHit
{
    bool bHit = false;
    int32 TriangleId = INDEX_NONE;
    int32 FaceId = INDEX_NONE;
    int32 SubMeshIndex = 0;
    FVector WorldPos = FVector::ZeroVector;
    FVector WorldNormal = FVector::UpVector;
    FVector2f UV = FVector2f::ZeroVector;
};

class FNGonPicker
{
public:
    void SetMesh(const UE::Geometry::FDynamicMesh3* InMesh);
    void PositionsChanged();
    int32 PickFace(const FVector& Origin, const FVector& Direction) const;
    FNGonSurfaceHit PickSurfaceHit(const FSceneView& View, const FVector2D& Cursor, const FNGonTopology& Topology) const;
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

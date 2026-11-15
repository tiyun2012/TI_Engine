#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

struct FMeshDescription;

struct FNGonFace
{
    TArray<int32> Perimeter;
    TArray<int32> Triangles;
};

// Logical vertices use dense indices. Source IDs remain available for inspection.
// Vertex instances for UVs and normals can share one logical vertex.
struct FNGonTopology
{
    TArray<FVector> RestPositions;
    TArray<int32> SourceVertexIds;
    TArray<int32> DynamicToLogicalVertex;
    TArray<FIntPoint> Edges;
    TMap<int32, FNGonFace> Faces;
    FBox RestBounds{ForceInit};
    int32 TriangleCount = 0;
    int32 MaxFaceCorners = 0;
};

struct FNGonMeshData
{
    FNGonTopology Topology;
    UE::Geometry::FDynamicMesh3 Mesh;
    // Indexed by logical vertex. Negative values mean the source has no sample.
    TArray<float> ImportedHeatSamples;
};

namespace NGon
{
    bool ConvertMeshDescription(const FMeshDescription& Source, FNGonMeshData& Out, FText& Error);
    bool BuildPolygonMesh(
        const TArray<FVector>& Positions, const TArray<TArray<int32>>& Faces,
        FNGonMeshData& Out, FText& Error);
    bool MakeDemo(FNGonMeshData& Out, FText& Error);
}

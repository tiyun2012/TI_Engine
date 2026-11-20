#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

struct FMeshDescription;

struct FNGonFace
{
    TArray<int32> Perimeter;
    TArray<int32> Triangles;
};

struct FNGonSubMeshInfo
{
    int32 SubMeshIndex = 0;
    FName Name = NAME_None;
    FName ParentName = NAME_None;
    int32 FirstFaceId = 0;
    int32 FaceCount = 0;
    int32 FirstVertexId = 0;
    int32 VertexCount = 0;
    bool bVisible = true;
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
    TArray<FNGonSubMeshInfo> SubMeshes;
    TMap<int32, int32> FaceToSubMesh;
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

#pragma once

#include "CoreMinimal.h"

/**
 * Unified configuration for NGon Bevel operations (Edges, Vertices, and Face Boundaries).
 * Modeled after Blender/TiWeb3D joint VMesh bevel with superellipse profiles and clamp-overlap safety.
 */
struct FNGonBevelOptions
{
    /** Offset distance in world units (automatically clamped when bClampOverlap is true). */
    double Width = 5.0;

    /** Number of profile subdivisions (1 = chamfer, 2..32 = curved superellipse + ADJ corner grids). */
    int32 Segments = 1;

    /** Superellipse profile shape in [0.05, 1.0]: 0.25 = flat chamfer, 0.5 = circular arc, >0.5 = squared, <0.25 = concave. */
    double Profile = 0.5;

    /** Automatically clamp Width (and binary-search on collision) so faces never invert or self-intersect. */
    bool bClampOverlap = true;

    /** Skip boundary or non-manifold edges/vertices instead of aborting the entire bevel operation. */
    bool bSkipNonManifoldEdges = true;

    /** Select the newly created bevel and corner faces after committing. */
    bool bSelectResultFaces = true;
};

/**
 * Pure geometry output from the NGon Bevel kernel before committing to FNGonPreviewScene.
 */
struct FNGonBevelOutput
{
    bool bOk = false;
    FString Message;

    TArray<FVector> Positions;
    TArray<TArray<int32>> Faces;

    TArray<int32> CreatedVertices;
    TArray<int32> BevelFaceIndices;
    TArray<int32> CornerFaceIndices;

    double RequestedWidth = 0.0;
    double AppliedWidth = 0.0;
    bool bWasClamped = false;
    int32 SkippedEdges = 0;
    int32 SkippedVertices = 0;
};

namespace NGon::Bevel
{
    /** Joint multi-edge bevel with VMesh corner planning, superellipse profiles, ADJ quad grids, and clamp-overlap safety. */
    FNGonBevelOutput BevelEdges(
        const TArray<FVector>& InPositions,
        const TArray<TArray<int32>>& InFaces,
        const TArray<FIntPoint>& SelectedEdges,
        const FNGonBevelOptions& Options);

    /** Safe multi-vertex bevel with edge-length clamping, optional segmented dome/chamfer, and manifold winding. */
    FNGonBevelOutput BevelVertices(
        const TArray<FVector>& InPositions,
        const TArray<TArray<int32>>& InFaces,
        const TArray<int32>& SelectedVertices,
        const FNGonBevelOptions& Options);

    /** Bevels the outer perimeter edges of a selected face region using the joint edge bevel kernel. */
    FNGonBevelOutput BevelFaceBoundaries(
        const TArray<FVector>& InPositions,
        const TArray<TArray<int32>>& InFaces,
        const TSet<int32>& SelectedFaceIndices,
        const FNGonBevelOptions& Options);
}

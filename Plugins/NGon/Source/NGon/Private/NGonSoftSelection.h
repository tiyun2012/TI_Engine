#pragma once

#include "CoreMinimal.h"
#include "NGonTopology.h"

enum class ENGonSoftMetric : uint8
{
    EdgeSurface,
    FaceSurface,
    Hybrid,
    Volume
};

enum class ENGonSoftConnectivity : uint8
{
    SameIsland,
    Unrestricted,
    FloodWithinRadius
};

enum class ENGonSoftTransformMode : uint8
{
    Fixed,
    LiveFalloff,
    SlideSculpt
};

struct FNGonSoftSelectionSettings
{
    bool bEnabled = false;
    double Radius = 100.0;
    ENGonSoftMetric Metric = ENGonSoftMetric::EdgeSurface;
    double SurfaceBlend = 0.5;
    ENGonSoftConnectivity Connectivity = ENGonSoftConnectivity::SameIsland;
};

struct FNGonVertexInfluence
{
    int32 Vertex = INDEX_NONE;
    double Weight = 1.0;
};

// Reusable editor-thread solver. It mirrors TiGUI's contract: seeds always have
// weight 1, all other weights are finite and clamped to [0,1], and a new solve
// replaces the previous result.
class FNGonSoftSelectionSolver final
{
public:
    const TArray<FNGonVertexInfluence>& Solve(const FNGonTopology& Topology,
        const TArray<FVector>& Positions, TConstArrayView<int32> Seeds,
        const FTransform& ObjectTransform, const FNGonSoftSelectionSettings& Settings);
    void Reset();
    const TArray<FNGonVertexInfluence>& Result() const { return Influences; }

private:
    TArray<FNGonVertexInfluence> Influences;
};

// A total-transform session stores the immutable gesture-start positions.
// Preview updates therefore do not accumulate floating-point drift, and the
// same session supports fixed, live, and slide falloff behavior.
class FNGonSoftTransformSession final
{
public:
    bool Begin(const FNGonTopology& Topology, const TArray<FVector>& Positions,
        TConstArrayView<int32> Seeds, const FTransform& ObjectTransform,
        const FTransform& InitialGizmo, const FNGonSoftSelectionSettings& Settings,
        ENGonSoftTransformMode Mode);
    bool BuildPositions(const FNGonTopology& Topology, const TArray<FVector>& CurrentPositions,
        const FTransform& NewGizmo, const FNGonSoftSelectionSettings& Settings,
        TArray<FVector>& OutPositions);
    void End();
    void Cancel();
    bool IsActive() const { return bActive; }
    const TArray<FNGonVertexInfluence>& Influences() const { return CurrentInfluences; }
    const TArray<FVector>& OriginalPositions() const { return StartPositions; }

private:
    FNGonSoftSelectionSolver Solver;
    TArray<FNGonVertexInfluence> StartInfluences;
    TArray<FNGonVertexInfluence> CurrentInfluences;
    TArray<FVector> StartPositions;
    TArray<FVector> WorkingPositions;
    TArray<int32> Seeds;
    FTransform InitialGizmo = FTransform::Identity;
    FTransform LastGizmo = FTransform::Identity;
    FTransform ObjectTransform = FTransform::Identity;
    ENGonSoftTransformMode Mode = ENGonSoftTransformMode::Fixed;
    bool bActive = false;
};

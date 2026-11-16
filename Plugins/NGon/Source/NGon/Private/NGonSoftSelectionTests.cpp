#if WITH_DEV_AUTOMATION_TESTS

#include "NGonMeshFiles.h"
#include "NGonSoftSelection.h"

#include "Misc/AutomationTest.h"

namespace
{
bool FindSeedAndNeighbor(const FNGonTopology& Topology, int32& OutSeed, int32& OutNeighbor, double& OutLength)
{
    for (const FIntPoint& Edge : Topology.Edges)
    {
        if (Topology.RestPositions.IsValidIndex(Edge.X) && Topology.RestPositions.IsValidIndex(Edge.Y))
        {
            OutSeed = Edge.X;
            OutNeighbor = Edge.Y;
            OutLength = FVector::Distance(Topology.RestPositions[Edge.X], Topology.RestPositions[Edge.Y]);
            return OutLength > 0.0;
        }
    }
    return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonSoftSelectionTest, "NGon.SoftSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonSoftSelectionTest::RunTest(const FString& Parameters)
{
    FNGonMeshData Data;
    FText Error;
    if (!TestTrue(TEXT("Build demo mesh"), NGon::MakeDemo(Data, Error)))
    {
        AddError(Error.ToString());
        return false;
    }

    int32 Seed = INDEX_NONE;
    int32 Neighbor = INDEX_NONE;
    double EdgeLength = 0.0;
    if (!TestTrue(TEXT("Demo has a usable authored edge"),
        FindSeedAndNeighbor(Data.Topology, Seed, Neighbor, EdgeLength)))
    {
        return false;
    }

    FNGonSoftSelectionSolver Solver;
    FNGonSoftSelectionSettings Settings;
    Settings.bEnabled = false;
    TArray<int32> SeedArray{Seed};
    const TArray<FNGonVertexInfluence>& Hard =
        Solver.Solve(Data.Topology, Data.Topology.RestPositions, SeedArray,
            FTransform::Identity, Settings);
    TestEqual(TEXT("Disabled soft selection keeps only the seed"), Hard.Num(), 1);
    if (!Hard.IsEmpty())
    {
        TestEqual(TEXT("Seed id is preserved"), Hard[0].Vertex, Seed);
        TestEqual(TEXT("Seed weight is one"), Hard[0].Weight, 1.0);
    }

    Settings.bEnabled = true;
    Settings.Radius = EdgeLength * 2.0;
    const TArray<FNGonVertexInfluence>& Surface =
        Solver.Solve(Data.Topology, Data.Topology.RestPositions, SeedArray,
            FTransform::Identity, Settings);
    bool bFoundNeighbor = false;
    double NeighborWeight = 0.0;
    for (const FNGonVertexInfluence& Influence : Surface)
    {
        TestTrue(TEXT("Influence weights are finite and clamped"),
            FMath::IsFinite(Influence.Weight) && Influence.Weight >= 0.0 && Influence.Weight <= 1.0);
        if (Influence.Vertex == Neighbor)
        {
            bFoundNeighbor = true;
            NeighborWeight = Influence.Weight;
        }
    }
    TestTrue(TEXT("Surface metric includes an authored neighbor"), bFoundNeighbor);
    TestTrue(TEXT("Neighbor has a partial falloff weight"), NeighborWeight > 0.0 && NeighborWeight < 1.0);

    const ENGonSoftMetric Metrics[] = {
        ENGonSoftMetric::EdgeSurface, ENGonSoftMetric::FaceSurface,
        ENGonSoftMetric::Volume, ENGonSoftMetric::Hybrid };
    for (const ENGonSoftMetric Metric : Metrics)
    {
        Settings.Metric = Metric;
        Settings.Connectivity = ENGonSoftConnectivity::Unrestricted;
        const TArray<FNGonVertexInfluence>& Result = Solver.Solve(
            Data.Topology, Data.Topology.RestPositions, SeedArray, FTransform::Identity, Settings);
        TestTrue(TEXT("Every soft metric keeps the hard seed"), Result.ContainsByPredicate(
            [Seed](const FNGonVertexInfluence& Influence) { return Influence.Vertex == Seed && Influence.Weight == 1.0; }));
        for (const FNGonVertexInfluence& Influence : Result)
        {
            TestTrue(TEXT("Every metric produces finite clamped weights"),
                FMath::IsFinite(Influence.Weight) && Influence.Weight >= 0.0 && Influence.Weight <= 1.0);
        }
    }
    const ENGonSoftConnectivity ConnectivityModes[] = {
        ENGonSoftConnectivity::SameIsland, ENGonSoftConnectivity::Unrestricted,
        ENGonSoftConnectivity::FloodWithinRadius };
    for (const ENGonSoftConnectivity Connectivity : ConnectivityModes)
    {
        Settings.Metric = ENGonSoftMetric::EdgeSurface;
        Settings.Connectivity = Connectivity;
        const TArray<FNGonVertexInfluence>& Result = Solver.Solve(
            Data.Topology, Data.Topology.RestPositions, SeedArray, FTransform::Identity, Settings);
        TestTrue(TEXT("Every connectivity mask preserves the hard seed"), Result.ContainsByPredicate(
            [Seed](const FNGonVertexInfluence& Influence) { return Influence.Vertex == Seed && Influence.Weight == 1.0; }));
    }
    Settings.Metric = ENGonSoftMetric::EdgeSurface;
    Settings.Connectivity = ENGonSoftConnectivity::SameIsland;
    FNGonSoftSelectionSettings Invalid = Settings;
    Invalid.SurfaceBlend = 2.0;
    const TArray<FNGonVertexInfluence>& InvalidResult = Solver.Solve(
        Data.Topology, Data.Topology.RestPositions, SeedArray, FTransform::Identity, Invalid);
    TestEqual(TEXT("Invalid settings reject falloff while preserving seeds"), InvalidResult.Num(), 1);

    FNGonSoftTransformSession Fixed;
    const FTransform Initial = FTransform::Identity;
    TestTrue(TEXT("Begin fixed transform"), Fixed.Begin(Data.Topology, Data.Topology.RestPositions,
        SeedArray, FTransform::Identity, Initial, Settings, ENGonSoftTransformMode::Fixed));
    TArray<FVector> First;
    TArray<FVector> Second;
    TestTrue(TEXT("Apply first fixed gesture update"), Fixed.BuildPositions(Data.Topology, Data.Topology.RestPositions,
        FTransform(FQuat::Identity, FVector(10, 0, 0)), Settings, First));
    TestTrue(TEXT("Apply second fixed gesture update from the original pose"), Fixed.BuildPositions(Data.Topology, First,
        FTransform(FQuat::Identity, FVector(20, 0, 0)), Settings, Second));
    TestTrue(TEXT("Fixed transform does not accumulate preview deltas"),
        Second[Seed].Equals(Data.Topology.RestPositions[Seed] + FVector(20, 0, 0), 0.001));
    Fixed.Cancel();

    FNGonSoftTransformSession Slide;
    TestTrue(TEXT("Begin slide sculpt transform"), Slide.Begin(Data.Topology, Data.Topology.RestPositions,
        SeedArray, FTransform::Identity, Initial, Settings, ENGonSoftTransformMode::SlideSculpt));
    TArray<FVector> SlideFirst;
    TArray<FVector> SlideSecond;
    TestTrue(TEXT("Apply first slide update"), Slide.BuildPositions(Data.Topology, Data.Topology.RestPositions,
        FTransform(FQuat::Identity, FVector(10, 0, 0)), Settings, SlideFirst));
    TestTrue(TEXT("Apply incremental slide update"), Slide.BuildPositions(Data.Topology, SlideFirst,
        FTransform(FQuat::Identity, FVector(20, 0, 0)), Settings, SlideSecond));
    TestTrue(TEXT("Slide mode accumulates incremental deltas"),
        SlideSecond[Seed].Equals(Data.Topology.RestPositions[Seed] + FVector(20, 0, 0), 0.001));

    FNGonSoftTransformSession Live;
    TestTrue(TEXT("Begin live falloff transform"), Live.Begin(Data.Topology, Data.Topology.RestPositions,
        SeedArray, FTransform::Identity, Initial, Settings, ENGonSoftTransformMode::LiveFalloff));
    TArray<FVector> LivePositions;
    TestTrue(TEXT("Apply live falloff update"), Live.BuildPositions(Data.Topology, Data.Topology.RestPositions,
        FTransform(FQuat::Identity, FVector(5, 0, 0)), Settings, LivePositions));
    TestTrue(TEXT("Live falloff moves the hard seed"), LivePositions[Seed].Equals(
        Data.Topology.RestPositions[Seed] + FVector(5, 0, 0), 0.001));
    Live.Cancel();
    return true;
}

#endif

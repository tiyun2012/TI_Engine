#include "NGonSoftSelection.h"

#include <queue>
#include <functional>
#include <limits>
#include <numeric>

namespace
{
double Distance(const FVector& A, const FVector& B)
{
    return FVector::Distance(A, B);
}

double SmoothWeight(double DistanceValue, double Radius)
{
    const double T = FMath::Clamp(DistanceValue / Radius, 0.0, 1.0);
    const double OneMinus = 1.0 - T;
    return OneMinus * OneMinus * (1.0 + 2.0 * T);
}

void AddSeed(TSet<int32>& Seen, TArray<int32>& Out, int32 Id, int32 Count)
{
    if (Id >= 0 && Id < Count && !Seen.Contains(Id))
    {
        Seen.Add(Id);
        Out.Add(Id);
    }
}
}

const TArray<FNGonVertexInfluence>& FNGonSoftSelectionSolver::Solve(
    const FNGonTopology& Topology, const TArray<FVector>& Positions, TConstArrayView<int32> Seeds,
    const FTransform& ObjectTransform, const FNGonSoftSelectionSettings& Settings)
{
    Influences.Reset();
    if (Positions.IsEmpty()) return Influences;
    if (!ObjectTransform.IsValid()) return Influences;
    for (const FVector& Position : Positions)
    {
        if (Position.ContainsNaN()) return Influences;
    }

    TSet<int32> Unique; TArray<int32> OrderedSeeds;
    for (const int32 Seed : Seeds) AddSeed(Unique, OrderedSeeds, Seed, Positions.Num());
    if (OrderedSeeds.IsEmpty()) return Influences;
    for (const int32 Seed : OrderedSeeds) Influences.Add({Seed, 1.0});

    if (Settings.Metric < ENGonSoftMetric::EdgeSurface || Settings.Metric > ENGonSoftMetric::Volume ||
        Settings.Connectivity < ENGonSoftConnectivity::SameIsland || Settings.Connectivity > ENGonSoftConnectivity::FloodWithinRadius ||
        !FMath::IsFinite(Settings.SurfaceBlend) || Settings.SurfaceBlend < 0.0 || Settings.SurfaceBlend > 1.0)
    {
        Influences.Reset();
        for (const int32 Seed : OrderedSeeds) Influences.Add({Seed, 1.0});
        return Influences;
    }
    if (!Settings.bEnabled || Settings.Radius <= 0.0 || !FMath::IsFinite(Settings.Radius) ||
        OrderedSeeds.Num() == Positions.Num()) return Influences;

    TSet<int32> SeedSet;
    for (const int32 Seed : OrderedSeeds) SeedSet.Add(Seed);

    const double Radius = Settings.Radius;
    const int32 VertexCount = Positions.Num();
    TArray<FVector> WorldPositions;
    WorldPositions.Reserve(VertexCount);
    for (const FVector& Position : Positions) WorldPositions.Add(ObjectTransform.TransformPosition(Position));

    // Authored connectivity is shared by edge, face-surface and flood masks.
    TArray<TArray<int32>> Neighbors; Neighbors.SetNum(VertexCount);
    for (const FIntPoint& Edge : Topology.Edges)
    {
        if (Neighbors.IsValidIndex(Edge.X) && Neighbors.IsValidIndex(Edge.Y))
        {
            Neighbors[Edge.X].AddUnique(Edge.Y);
            Neighbors[Edge.Y].AddUnique(Edge.X);
        }
    }

    TArray<int32> Island; Island.SetNum(VertexCount);
    TArray<int32> IslandSize; IslandSize.Init(1, VertexCount);
    for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex) Island[Vertex] = Vertex;
    TFunction<int32(int32)> FindRoot = [&Island, &FindRoot](int32 Id) -> int32
    {
        while (Island[Id] != Id)
        {
            Island[Id] = Island[Island[Id]];
            Id = Island[Id];
        }
        return Id;
    };
    for (const FIntPoint& Edge : Topology.Edges)
    {
        if (!Island.IsValidIndex(Edge.X) || !Island.IsValidIndex(Edge.Y)) continue;
        int32 A = FindRoot(Edge.X), B = FindRoot(Edge.Y);
        if (A == B) continue;
        if (IslandSize[A] < IslandSize[B]) Swap(A, B);
        Island[B] = A; IslandSize[A] += IslandSize[B];
    }
    for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex) Island[Vertex] = FindRoot(Vertex);

    TSet<int32> Allowed;
    if (Settings.Connectivity == ENGonSoftConnectivity::SameIsland)
    {
        TSet<int32> Roots;
        for (const int32 Seed : OrderedSeeds) Roots.Add(Island[Seed]);
        for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex) if (Roots.Contains(Island[Vertex])) Allowed.Add(Vertex);
    }
    else if (Settings.Connectivity == ENGonSoftConnectivity::FloodWithinRadius)
    {
        FVector Center = FVector::ZeroVector;
        for (const int32 Seed : OrderedSeeds) Center += WorldPositions[Seed];
        Center /= static_cast<double>(OrderedSeeds.Num());
        TArray<int32> Flood = OrderedSeeds;
        for (const int32 Seed : OrderedSeeds) Allowed.Add(Seed);
        for (int32 Head = 0; Head < Flood.Num(); ++Head)
        {
            const int32 Vertex = Flood[Head];
            for (const int32 Next : Neighbors[Vertex])
            {
                if (Allowed.Contains(Next) || FVector::Distance(WorldPositions[Next], Center) > Radius) continue;
                Allowed.Add(Next); Flood.Add(Next);
            }
        }
    }

    TArray<double> SurfaceDistances;
    SurfaceDistances.Init(TNumericLimits<double>::Max(), VertexCount);
    const bool bNeedSurface = Settings.Metric == ENGonSoftMetric::EdgeSurface
        || Settings.Metric == ENGonSoftMetric::FaceSurface
        || Settings.Metric == ENGonSoftMetric::Hybrid;
    if (bNeedSurface)
    {
        using FQueueItem = TPair<double, int32>;
        const auto HeapLess = [](const FQueueItem& A, const FQueueItem& B) { return A.Key < B.Key; };

        if (Settings.Metric == ENGonSoftMetric::FaceSurface)
        {
            // Face-center surface approximation: vertices connect through the
            // authored polygon centers, matching TiGUI's face metric.
            const int32 FaceCount = Topology.Faces.Num();
            TArray<int32> FaceIds; Topology.Faces.GetKeys(FaceIds);
            TArray<FVector> Centers; Centers.SetNumZeroed(FaceCount);
            TArray<TArray<int32>> VertexFaces; VertexFaces.SetNum(VertexCount);

            for (int32 Index = 0; Index < FaceIds.Num(); ++Index)
            {
                const FNGonFace& Face = Topology.Faces.FindChecked(FaceIds[Index]);
                for (const int32 Vertex : Face.Perimeter)
                {
                    if (WorldPositions.IsValidIndex(Vertex))
                    {
                        Centers[Index] += WorldPositions[Vertex];
                        VertexFaces[Vertex].Add(Index);
                    }
                }
                Centers[Index] /= FMath::Max(1, Face.Perimeter.Num());
            }

            const int32 NodeCount = VertexCount + FaceCount;
            TArray<double> Distances; Distances.Init(TNumericLimits<double>::Max(), NodeCount);
            TArray<FQueueItem> Queue;
            for (const int32 Seed : OrderedSeeds)
            {
                Distances[Seed] = 0.0;
                Queue.HeapPush({0.0, Seed}, HeapLess);
            }

            while (!Queue.IsEmpty())
            {
                FQueueItem Item;
                Queue.HeapPop(Item, HeapLess, EAllowShrinking::No);
                const double Current = Item.Key;
                const int32 Node = Item.Value;
                if (Current > Distances[Node] || Current > Radius) continue;

                if (Node < VertexCount)
                {
                    for (const int32 Next : Neighbors[Node])
                    {
                        const double Candidate = Current + Distance(WorldPositions[Node], WorldPositions[Next]);
                        if (Candidate < Distances[Next] && Candidate <= Radius)
                        {
                            Distances[Next] = Candidate;
                            Queue.HeapPush({Candidate, Next}, HeapLess);
                        }
                    }
                    for (const int32 FaceIndex : VertexFaces[Node])
                    {
                        const int32 FaceNodeId = VertexCount + FaceIndex;
                        const double Candidate = Current + Distance(WorldPositions[Node], Centers[FaceIndex]);
                        if (Candidate < Distances[FaceNodeId] && Candidate <= Radius)
                        {
                            Distances[FaceNodeId] = Candidate;
                            Queue.HeapPush({Candidate, FaceNodeId}, HeapLess);
                        }
                    }
                }
                else
                {
                    const int32 FaceIndex = Node - VertexCount;
                    const FNGonFace& Face = Topology.Faces.FindChecked(FaceIds[FaceIndex]);
                    for (const int32 Next : Face.Perimeter)
                    {
                        const double Candidate = Current + Distance(Centers[FaceIndex], WorldPositions[Next]);
                        if (Candidate < Distances[Next] && Candidate <= Radius)
                        {
                            Distances[Next] = Candidate;
                            Queue.HeapPush({Candidate, Next}, HeapLess);
                        }
                    }
                }
            }
            for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex) SurfaceDistances[Vertex] = Distances[Vertex];
        }
        else
        {
            TArray<FQueueItem> Queue;
            for (const int32 Seed : OrderedSeeds)
            {
                SurfaceDistances[Seed] = 0.0;
                Queue.HeapPush({0.0, Seed}, HeapLess);
            }
            while (!Queue.IsEmpty())
            {
                FQueueItem Item;
                Queue.HeapPop(Item, HeapLess, EAllowShrinking::No);
                const double Current = Item.Key;
                const int32 Vertex = Item.Value;
                if (Current > SurfaceDistances[Vertex] || Current > Radius) continue;
                for (const int32 Next : Neighbors[Vertex])
                {
                    const double Candidate = Current + Distance(WorldPositions[Vertex], WorldPositions[Next]);
                    if (Candidate < SurfaceDistances[Next] && Candidate <= Radius)
                    {
                        SurfaceDistances[Next] = Candidate;
                        Queue.HeapPush({Candidate, Next}, HeapLess);
                    }
                }
            }
        }
    }

    TArray<double> VolumeDistances;
    const bool bNeedVolume = Settings.Metric == ENGonSoftMetric::Volume || Settings.Metric == ENGonSoftMetric::Hybrid;
    if (bNeedVolume)
    {
        FVector Center = FVector::ZeroVector;
        for (const int32 Seed : OrderedSeeds) Center += WorldPositions[Seed];
        Center /= static_cast<double>(OrderedSeeds.Num());
        VolumeDistances.Init(TNumericLimits<double>::Max(), VertexCount);
        for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex) VolumeDistances[Vertex] = FVector::Distance(WorldPositions[Vertex], Center);
    }

    for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex)
    {
        if (SeedSet.Contains(Vertex)) continue;
        if (Settings.Connectivity != ENGonSoftConnectivity::Unrestricted && !Allowed.Contains(Vertex)) continue;
        const double SurfaceWeight = bNeedSurface && SurfaceDistances[Vertex] <= Radius ? SmoothWeight(SurfaceDistances[Vertex], Radius) : 0.0;
        const double VolumeWeight = bNeedVolume && VolumeDistances[Vertex] <= Radius ? SmoothWeight(VolumeDistances[Vertex], Radius) : 0.0;
        double Weight = 0.0;
        switch (Settings.Metric)
        {
        case ENGonSoftMetric::Volume: Weight = VolumeWeight; break;
        case ENGonSoftMetric::Hybrid: Weight = (1.0 - FMath::Clamp(Settings.SurfaceBlend, 0.0, 1.0)) * VolumeWeight + FMath::Clamp(Settings.SurfaceBlend, 0.0, 1.0) * SurfaceWeight; break;
        default: Weight = SurfaceWeight; break;
        }
        if (Weight > 0.0 && FMath::IsFinite(Weight)) Influences.Add({Vertex, FMath::Clamp(Weight, 0.0, 1.0)});
    }
    return Influences;
}

void FNGonSoftSelectionSolver::Reset() { Influences.Reset(); }

bool FNGonSoftTransformSession::Begin(const FNGonTopology& Topology, const TArray<FVector>& Positions,
    TConstArrayView<int32> InSeeds, const FTransform& InObjectTransform, const FTransform& InInitialGizmo,
    const FNGonSoftSelectionSettings& Settings, ENGonSoftTransformMode InMode)
{
    Cancel();
    if (Positions.IsEmpty()) return false;
    Seeds.Reset(); for (const int32 Seed : InSeeds) if (Positions.IsValidIndex(Seed)) Seeds.AddUnique(Seed);
    if (Seeds.IsEmpty()) return false;
    StartPositions = Positions; WorkingPositions = Positions; ObjectTransform = InObjectTransform;
    InitialGizmo = InInitialGizmo; LastGizmo = InInitialGizmo; Mode = InMode;
    StartInfluences = Solver.Solve(Topology, StartPositions, Seeds, ObjectTransform, Settings);
    CurrentInfluences = StartInfluences; bActive = !StartInfluences.IsEmpty();
    return bActive;
}

bool FNGonSoftTransformSession::BuildPositions(const FNGonTopology& Topology, const TArray<FVector>& CurrentPositions,
    const FTransform& NewGizmo, const FNGonSoftSelectionSettings& Settings, TArray<FVector>& OutPositions)
{
    if (!bActive || CurrentPositions.Num() != StartPositions.Num()) return false;
    FTransform Delta = NewGizmo * InitialGizmo.Inverse();
    if (Mode == ENGonSoftTransformMode::SlideSculpt)
    {
        // Slide mode applies the incremental gizmo delta to its working mesh,
        // retaining the accumulated sculpt after the cursor changes direction.
        Delta = NewGizmo * LastGizmo.Inverse();
        OutPositions = WorkingPositions;
    }
    else
    {
        OutPositions = StartPositions;
    }

    const TArray<FNGonVertexInfluence>& Influences =
        (Mode == ENGonSoftTransformMode::LiveFalloff && !CurrentInfluences.IsEmpty())
            ? CurrentInfluences : StartInfluences;
    for (const FNGonVertexInfluence& Influence : Influences)
    {
        if (!StartPositions.IsValidIndex(Influence.Vertex)) continue;
        const FVector BaseLocal = (Mode == ENGonSoftTransformMode::SlideSculpt)
            ? OutPositions[Influence.Vertex] : StartPositions[Influence.Vertex];
        const FVector StartWorld = ObjectTransform.TransformPosition(BaseLocal);
        const FVector TargetWorld = Delta.TransformPosition(StartWorld);
        const FVector WorldPosition = FMath::Lerp(StartWorld, TargetWorld, static_cast<float>(Influence.Weight));
        OutPositions[Influence.Vertex] = ObjectTransform.InverseTransformPosition(WorldPosition);
    }

    if (Mode == ENGonSoftTransformMode::SlideSculpt)
    {
        WorkingPositions = OutPositions;
    }
    if ((Mode == ENGonSoftTransformMode::LiveFalloff || Mode == ENGonSoftTransformMode::SlideSculpt) && Settings.bEnabled)
    {
        CurrentInfluences = Solver.Solve(Topology, OutPositions, Seeds, ObjectTransform, Settings);
    }
    LastGizmo = NewGizmo;
    return true;
}

void FNGonSoftTransformSession::End() { bActive = false; }

void FNGonSoftTransformSession::Cancel()
{
    bActive = false; StartInfluences.Reset(); CurrentInfluences.Reset(); StartPositions.Reset();
    WorkingPositions.Reset(); Seeds.Reset(); InitialGizmo = FTransform::Identity; LastGizmo = FTransform::Identity;
}
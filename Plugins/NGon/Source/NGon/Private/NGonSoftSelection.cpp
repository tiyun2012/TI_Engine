#include "NGonSoftSelection.h"

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

uint64 ComputeTopologySignature(const FNGonTopology& Topology)
{
    uint64 Hash = 1469598103934665603ULL;
    auto Mix = [&Hash](uint64 Value)
    {
        Hash ^= Value;
        Hash *= 1099511628211ULL;
    };

    Mix(static_cast<uint64>(Topology.RestPositions.Num()));
    Mix(static_cast<uint64>(Topology.Edges.Num()));
    Mix(static_cast<uint64>(Topology.Faces.Num()));
    Mix(reinterpret_cast<uintptr_t>(Topology.Edges.GetData()));
    Mix(reinterpret_cast<uintptr_t>(Topology.RestPositions.GetData()));

    const int32 EdgeSamples = FMath::Min(Topology.Edges.Num(), 8);
    for (int32 i = 0; i < EdgeSamples; ++i)
    {
        Mix(static_cast<uint64>(static_cast<uint32>(Topology.Edges[i].X)));
        Mix(static_cast<uint64>(static_cast<uint32>(Topology.Edges[i].Y)));
    }
    if (!Topology.Edges.IsEmpty())
    {
        const FIntPoint LastEdge = Topology.Edges.Last();
        Mix(static_cast<uint64>(static_cast<uint32>(LastEdge.X)));
        Mix(static_cast<uint64>(static_cast<uint32>(LastEdge.Y)));
    }
    return Hash;
}

struct FSoftTopologyCache
{
    const FNGonTopology* TopologyPtr = nullptr;
    uint64 Signature = 0;
    int32 VertexCount = 0;
    int32 EdgeCount = 0;
    int32 FaceCount = 0;

    // CSR vertex->vertex adjacency
    TArray<int32> NeighborOffsets;
    TArray<int32> NeighborIndices;

    // Connected component ID per vertex
    TArray<int32> Island;

    // Lazily built CSR structures for FaceSurface metric
    bool bFaceGraphBuilt = false;
    TArray<int32> FaceIds;
    TArray<int32> FacePerimeterOffsets;
    TArray<int32> FacePerimeterVertices;
    TArray<int32> VertexFaceOffsets;
    TArray<int32> VertexFaceIndices;

    void EnsureBuilt(const FNGonTopology& Topology, int32 InVertexCount, bool bNeedFaceGraph)
    {
        const uint64 NewSig = ComputeTopologySignature(Topology);
        if (TopologyPtr != &Topology || Signature != NewSig || VertexCount != InVertexCount ||
            EdgeCount != Topology.Edges.Num() || FaceCount != Topology.Faces.Num())
        {
            TopologyPtr = &Topology;
            Signature = NewSig;
            VertexCount = InVertexCount;
            EdgeCount = Topology.Edges.Num();
            FaceCount = Topology.Faces.Num();
            bFaceGraphBuilt = false;

            NeighborOffsets.Init(0, VertexCount + 1);
            for (const FIntPoint& Edge : Topology.Edges)
            {
                if (Edge.X >= 0 && Edge.X < VertexCount && Edge.Y >= 0 && Edge.Y < VertexCount && Edge.X != Edge.Y)
                {
                    ++NeighborOffsets[Edge.X + 1];
                    ++NeighborOffsets[Edge.Y + 1];
                }
            }
            for (int32 i = 0; i < VertexCount; ++i)
            {
                NeighborOffsets[i + 1] += NeighborOffsets[i];
            }

            NeighborIndices.SetNumUninitialized(NeighborOffsets[VertexCount]);
            TArray<int32> WriteCursor = NeighborOffsets;

            Island.SetNumUninitialized(VertexCount);
            TArray<int32> IslandSize;
            IslandSize.Init(1, VertexCount);
            for (int32 v = 0; v < VertexCount; ++v)
            {
                Island[v] = v;
            }

            auto FindRoot = [this](int32 Id) -> int32
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
                if (Edge.X >= 0 && Edge.X < VertexCount && Edge.Y >= 0 && Edge.Y < VertexCount && Edge.X != Edge.Y)
                {
                    NeighborIndices[WriteCursor[Edge.X]++] = Edge.Y;
                    NeighborIndices[WriteCursor[Edge.Y]++] = Edge.X;

                    int32 A = FindRoot(Edge.X);
                    int32 B = FindRoot(Edge.Y);
                    if (A != B)
                    {
                        if (IslandSize[A] < IslandSize[B])
                        {
                            Swap(A, B);
                        }
                        Island[B] = A;
                        IslandSize[A] += IslandSize[B];
                    }
                }
            }

            for (int32 v = 0; v < VertexCount; ++v)
            {
                Island[v] = FindRoot(v);
            }
        }

        if (bNeedFaceGraph && !bFaceGraphBuilt)
        {
            bFaceGraphBuilt = true;
            Topology.Faces.GetKeys(FaceIds);
            const int32 NumFaces = FaceIds.Num();

            FacePerimeterOffsets.Init(0, NumFaces + 1);
            VertexFaceOffsets.Init(0, VertexCount + 1);

            for (int32 f = 0; f < NumFaces; ++f)
            {
                const FNGonFace& Face = Topology.Faces.FindChecked(FaceIds[f]);
                int32 ValidCorners = 0;
                for (const int32 v : Face.Perimeter)
                {
                    if (v >= 0 && v < VertexCount)
                    {
                        ++ValidCorners;
                        ++VertexFaceOffsets[v + 1];
                    }
                }
                FacePerimeterOffsets[f + 1] = FacePerimeterOffsets[f] + ValidCorners;
            }

            for (int32 v = 0; v < VertexCount; ++v)
            {
                VertexFaceOffsets[v + 1] += VertexFaceOffsets[v];
            }

            FacePerimeterVertices.SetNumUninitialized(FacePerimeterOffsets[NumFaces]);
            VertexFaceIndices.SetNumUninitialized(VertexFaceOffsets[VertexCount]);
            TArray<int32> VertexFaceCursor = VertexFaceOffsets;

            for (int32 f = 0; f < NumFaces; ++f)
            {
                const FNGonFace& Face = Topology.Faces.FindChecked(FaceIds[f]);
                int32 CornerWrite = FacePerimeterOffsets[f];
                for (const int32 v : Face.Perimeter)
                {
                    if (v >= 0 && v < VertexCount)
                    {
                        FacePerimeterVertices[CornerWrite++] = v;
                        VertexFaceIndices[VertexFaceCursor[v]++] = f;
                    }
                }
            }
        }
    }
};

struct FSoftSolverScratch
{
    int32 Stamp = 1;
    TArray<int32> SeedStamp;
    TArray<int32> IslandStamp;
    TArray<int32> AllowedStamp;
    TArray<double> SurfaceDistances;
    TArray<double> FaceNodeDistances;
    TArray<int32> TouchedVertices;
    TArray<int32> TouchedFaceNodes;
    TArray<int32> FloodQueue;
    TArray<TPair<double, int32>> DijkstraQueue;
    TArray<FVector> TransformedPositions;
    TArray<FVector> FaceCenters;

    int32 NextStamp()
    {
        ++Stamp;
        if (Stamp <= 0)
        {
            Stamp = 1;
            FMemory::Memzero(SeedStamp.GetData(), SeedStamp.Num() * sizeof(int32));
            FMemory::Memzero(IslandStamp.GetData(), IslandStamp.Num() * sizeof(int32));
            FMemory::Memzero(AllowedStamp.GetData(), AllowedStamp.Num() * sizeof(int32));
        }
        return Stamp;
    }

    void EnsureVertexCapacity(int32 VertexCount)
    {
        if (SeedStamp.Num() < VertexCount)
        {
            SeedStamp.SetNumZeroed(VertexCount);
            IslandStamp.SetNumZeroed(VertexCount);
            AllowedStamp.SetNumZeroed(VertexCount);
        }
        if (SurfaceDistances.Num() < VertexCount)
        {
            const int32 OldNum = SurfaceDistances.Num();
            SurfaceDistances.SetNumUninitialized(VertexCount);
            for (int32 i = OldNum; i < VertexCount; ++i)
            {
                SurfaceDistances[i] = TNumericLimits<double>::Max();
            }
        }
    }

    void EnsureFaceCapacity(int32 FaceCount)
    {
        if (FaceNodeDistances.Num() < FaceCount)
        {
            const int32 OldNum = FaceNodeDistances.Num();
            FaceNodeDistances.SetNumUninitialized(FaceCount);
            for (int32 i = OldNum; i < FaceCount; ++i)
            {
                FaceNodeDistances[i] = TNumericLimits<double>::Max();
            }
        }
    }
};

FSoftTopologyCache GTopologyCache;
FSoftSolverScratch GScratch;
}

const TArray<FNGonVertexInfluence>& FNGonSoftSelectionSolver::Solve(
    const FNGonTopology& Topology, const TArray<FVector>& Positions, TConstArrayView<int32> Seeds,
    const FTransform& ObjectTransform, const FNGonSoftSelectionSettings& Settings)
{
    Influences.Reset();
    const int32 VertexCount = Positions.Num();
    if (VertexCount == 0 || !ObjectTransform.IsValid())
    {
        return Influences;
    }

    const int32 SampleCheckCount = FMath::Min(VertexCount, 64);
    for (int32 i = 0; i < SampleCheckCount; ++i)
    {
        if (Positions[i].ContainsNaN())
        {
            return Influences;
        }
    }

    GScratch.EnsureVertexCapacity(VertexCount);
    const int32 SeedMark = GScratch.NextStamp();

    TArray<int32, TInlineAllocator<64>> OrderedSeeds;
    OrderedSeeds.Reserve(Seeds.Num());
    for (const int32 Seed : Seeds)
    {
        if (Seed >= 0 && Seed < VertexCount && GScratch.SeedStamp[Seed] != SeedMark)
        {
            if (Positions[Seed].ContainsNaN())
            {
                Influences.Reset();
                return Influences;
            }
            GScratch.SeedStamp[Seed] = SeedMark;
            OrderedSeeds.Add(Seed);
        }
    }
    if (OrderedSeeds.IsEmpty())
    {
        return Influences;
    }

    Influences.Reserve(OrderedSeeds.Num() + 256);
    for (const int32 Seed : OrderedSeeds)
    {
        Influences.Add({Seed, 1.0});
    }

    if (Settings.Metric < ENGonSoftMetric::EdgeSurface || Settings.Metric > ENGonSoftMetric::Volume ||
        Settings.Connectivity < ENGonSoftConnectivity::SameIsland || Settings.Connectivity > ENGonSoftConnectivity::FloodWithinRadius ||
        !FMath::IsFinite(Settings.SurfaceBlend) || Settings.SurfaceBlend < 0.0 || Settings.SurfaceBlend > 1.0)
    {
        return Influences;
    }
    if (!Settings.bEnabled || Settings.Radius <= 0.0 || !FMath::IsFinite(Settings.Radius) ||
        OrderedSeeds.Num() == VertexCount)
    {
        return Influences;
    }

    const double Radius = Settings.Radius;
    const double RadiusSquared = Radius * Radius;
    const bool bNeedSurface = Settings.Metric == ENGonSoftMetric::EdgeSurface
        || Settings.Metric == ENGonSoftMetric::FaceSurface
        || Settings.Metric == ENGonSoftMetric::Hybrid;
    const bool bNeedVolume = Settings.Metric == ENGonSoftMetric::Volume
        || Settings.Metric == ENGonSoftMetric::Hybrid;
    const bool bNeedFaceGraph = Settings.Metric == ENGonSoftMetric::FaceSurface;

    GTopologyCache.EnsureBuilt(Topology, VertexCount, bNeedFaceGraph);

    const FVector* WorldPositions = Positions.GetData();
    if (!ObjectTransform.Equals(FTransform::Identity, 0.0))
    {
        GScratch.TransformedPositions.SetNumUninitialized(VertexCount, EAllowShrinking::No);
        for (int32 v = 0; v < VertexCount; ++v)
        {
            GScratch.TransformedPositions[v] = ObjectTransform.TransformPosition(Positions[v]);
        }
        WorldPositions = GScratch.TransformedPositions.GetData();
    }

    FVector SeedCenter = FVector::ZeroVector;
    if (bNeedVolume || Settings.Connectivity == ENGonSoftConnectivity::FloodWithinRadius)
    {
        for (const int32 Seed : OrderedSeeds)
        {
            SeedCenter += WorldPositions[Seed];
        }
        SeedCenter /= static_cast<double>(OrderedSeeds.Num());
    }

    const int32 IslandMark = GScratch.NextStamp();
    if (Settings.Connectivity == ENGonSoftConnectivity::SameIsland)
    {
        for (const int32 Seed : OrderedSeeds)
        {
            GScratch.IslandStamp[GTopologyCache.Island[Seed]] = IslandMark;
        }
    }

    const int32 AllowedMark = GScratch.NextStamp();
    if (Settings.Connectivity == ENGonSoftConnectivity::FloodWithinRadius)
    {
        GScratch.FloodQueue.Reset();
        for (const int32 Seed : OrderedSeeds)
        {
            GScratch.AllowedStamp[Seed] = AllowedMark;
            GScratch.FloodQueue.Add(Seed);
        }
        for (int32 Head = 0; Head < GScratch.FloodQueue.Num(); ++Head)
        {
            const int32 Vertex = GScratch.FloodQueue[Head];
            const int32 StartEdge = GTopologyCache.NeighborOffsets[Vertex];
            const int32 EndEdge = GTopologyCache.NeighborOffsets[Vertex + 1];
            for (int32 EdgeIdx = StartEdge; EdgeIdx < EndEdge; ++EdgeIdx)
            {
                const int32 Next = GTopologyCache.NeighborIndices[EdgeIdx];
                if (GScratch.AllowedStamp[Next] == AllowedMark ||
                    FVector::DistSquared(WorldPositions[Next], SeedCenter) > RadiusSquared)
                {
                    continue;
                }
                GScratch.AllowedStamp[Next] = AllowedMark;
                GScratch.FloodQueue.Add(Next);
            }
        }
    }

    auto IsVertexAllowed = [&](int32 Vertex) -> bool
    {
        if (Settings.Connectivity == ENGonSoftConnectivity::SameIsland)
        {
            return GScratch.IslandStamp[GTopologyCache.Island[Vertex]] == IslandMark;
        }
        if (Settings.Connectivity == ENGonSoftConnectivity::FloodWithinRadius)
        {
            return GScratch.AllowedStamp[Vertex] == AllowedMark;
        }
        return true;
    };

    GScratch.TouchedVertices.Reset();
    GScratch.TouchedFaceNodes.Reset();

    if (bNeedSurface)
    {
        using FQueueItem = TPair<double, int32>;
        const auto HeapLess = [](const FQueueItem& A, const FQueueItem& B) { return A.Key < B.Key; };
        GScratch.DijkstraQueue.Reset();

        if (Settings.Metric == ENGonSoftMetric::FaceSurface)
        {
            const int32 NumFaces = GTopologyCache.FaceIds.Num();
            GScratch.EnsureFaceCapacity(NumFaces);
            GScratch.FaceCenters.SetNumUninitialized(NumFaces, EAllowShrinking::No);

            for (const int32 Seed : OrderedSeeds)
            {
                if (GScratch.SurfaceDistances[Seed] == TNumericLimits<double>::Max())
                {
                    GScratch.TouchedVertices.Add(Seed);
                }
                GScratch.SurfaceDistances[Seed] = 0.0;
                GScratch.DijkstraQueue.HeapPush({0.0, Seed}, HeapLess);
            }

            const int32 FaceCenterMark = GScratch.NextStamp();
            if (GScratch.AllowedStamp.Num() < VertexCount + NumFaces)
            {
                GScratch.AllowedStamp.SetNumZeroed(VertexCount + NumFaces);
            }

            auto GetFaceCenter = [&](int32 FaceIndex) -> const FVector&
            {
                int32& CenterStamp = GScratch.AllowedStamp[VertexCount + FaceIndex];
                if (CenterStamp != FaceCenterMark)
                {
                    CenterStamp = FaceCenterMark;
                    FVector Sum = FVector::ZeroVector;
                    const int32 StartCorner = GTopologyCache.FacePerimeterOffsets[FaceIndex];
                    const int32 EndCorner = GTopologyCache.FacePerimeterOffsets[FaceIndex + 1];
                    for (int32 c = StartCorner; c < EndCorner; ++c)
                    {
                        Sum += WorldPositions[GTopologyCache.FacePerimeterVertices[c]];
                    }
                    GScratch.FaceCenters[FaceIndex] = Sum / static_cast<double>(FMath::Max(1, EndCorner - StartCorner));
                }
                return GScratch.FaceCenters[FaceIndex];
            };

            while (!GScratch.DijkstraQueue.IsEmpty())
            {
                FQueueItem Item;
                GScratch.DijkstraQueue.HeapPop(Item, HeapLess, EAllowShrinking::No);
                const double Current = Item.Key;
                const int32 Node = Item.Value;
                if (Current > Radius)
                {
                    continue;
                }

                if (Node < VertexCount)
                {
                    if (Current > GScratch.SurfaceDistances[Node])
                    {
                        continue;
                    }
                    const int32 StartN = GTopologyCache.NeighborOffsets[Node];
                    const int32 EndN = GTopologyCache.NeighborOffsets[Node + 1];
                    for (int32 i = StartN; i < EndN; ++i)
                    {
                        const int32 Next = GTopologyCache.NeighborIndices[i];
                        const double Candidate = Current + Distance(WorldPositions[Node], WorldPositions[Next]);
                        if (Candidate < GScratch.SurfaceDistances[Next] && Candidate <= Radius)
                        {
                            if (GScratch.SurfaceDistances[Next] == TNumericLimits<double>::Max())
                            {
                                GScratch.TouchedVertices.Add(Next);
                            }
                            GScratch.SurfaceDistances[Next] = Candidate;
                            GScratch.DijkstraQueue.HeapPush({Candidate, Next}, HeapLess);
                        }
                    }

                    const int32 StartF = GTopologyCache.VertexFaceOffsets[Node];
                    const int32 EndF = GTopologyCache.VertexFaceOffsets[Node + 1];
                    for (int32 i = StartF; i < EndF; ++i)
                    {
                        const int32 FaceIndex = GTopologyCache.VertexFaceIndices[i];
                        const double Candidate = Current + Distance(WorldPositions[Node], GetFaceCenter(FaceIndex));
                        if (Candidate < GScratch.FaceNodeDistances[FaceIndex] && Candidate <= Radius)
                        {
                            if (GScratch.FaceNodeDistances[FaceIndex] == TNumericLimits<double>::Max())
                            {
                                GScratch.TouchedFaceNodes.Add(FaceIndex);
                            }
                            GScratch.FaceNodeDistances[FaceIndex] = Candidate;
                            GScratch.DijkstraQueue.HeapPush({Candidate, VertexCount + FaceIndex}, HeapLess);
                        }
                    }
                }
                else
                {
                    const int32 FaceIndex = Node - VertexCount;
                    if (Current > GScratch.FaceNodeDistances[FaceIndex])
                    {
                        continue;
                    }
                    const FVector& FaceCenter = GetFaceCenter(FaceIndex);
                    const int32 StartC = GTopologyCache.FacePerimeterOffsets[FaceIndex];
                    const int32 EndC = GTopologyCache.FacePerimeterOffsets[FaceIndex + 1];
                    for (int32 c = StartC; c < EndC; ++c)
                    {
                        const int32 Next = GTopologyCache.FacePerimeterVertices[c];
                        const double Candidate = Current + Distance(FaceCenter, WorldPositions[Next]);
                        if (Candidate < GScratch.SurfaceDistances[Next] && Candidate <= Radius)
                        {
                            if (GScratch.SurfaceDistances[Next] == TNumericLimits<double>::Max())
                            {
                                GScratch.TouchedVertices.Add(Next);
                            }
                            GScratch.SurfaceDistances[Next] = Candidate;
                            GScratch.DijkstraQueue.HeapPush({Candidate, Next}, HeapLess);
                        }
                    }
                }
            }
        }
        else
        {
            for (const int32 Seed : OrderedSeeds)
            {
                if (GScratch.SurfaceDistances[Seed] == TNumericLimits<double>::Max())
                {
                    GScratch.TouchedVertices.Add(Seed);
                }
                GScratch.SurfaceDistances[Seed] = 0.0;
                GScratch.DijkstraQueue.HeapPush({0.0, Seed}, HeapLess);
            }
            while (!GScratch.DijkstraQueue.IsEmpty())
            {
                FQueueItem Item;
                GScratch.DijkstraQueue.HeapPop(Item, HeapLess, EAllowShrinking::No);
                const double Current = Item.Key;
                const int32 Vertex = Item.Value;
                if (Current > GScratch.SurfaceDistances[Vertex] || Current > Radius)
                {
                    continue;
                }
                const int32 StartN = GTopologyCache.NeighborOffsets[Vertex];
                const int32 EndN = GTopologyCache.NeighborOffsets[Vertex + 1];
                for (int32 i = StartN; i < EndN; ++i)
                {
                    const int32 Next = GTopologyCache.NeighborIndices[i];
                    const double Candidate = Current + Distance(WorldPositions[Vertex], WorldPositions[Next]);
                    if (Candidate < GScratch.SurfaceDistances[Next] && Candidate <= Radius)
                    {
                        if (GScratch.SurfaceDistances[Next] == TNumericLimits<double>::Max())
                        {
                            GScratch.TouchedVertices.Add(Next);
                        }
                        GScratch.SurfaceDistances[Next] = Candidate;
                        GScratch.DijkstraQueue.HeapPush({Candidate, Next}, HeapLess);
                    }
                }
            }
        }
    }

    const double ClampedBlend = FMath::Clamp(Settings.SurfaceBlend, 0.0, 1.0);
    if (!bNeedVolume)
    {
        GScratch.TouchedVertices.Sort();
        for (const int32 Vertex : GScratch.TouchedVertices)
        {
            const double SurfDist = GScratch.SurfaceDistances[Vertex];
            GScratch.SurfaceDistances[Vertex] = TNumericLimits<double>::Max();

            if (GScratch.SeedStamp[Vertex] == SeedMark || !IsVertexAllowed(Vertex))
            {
                continue;
            }
            if (SurfDist <= Radius)
            {
                const double Weight = SmoothWeight(SurfDist, Radius);
                if (Weight > 0.0 && FMath::IsFinite(Weight))
                {
                    Influences.Add({Vertex, FMath::Clamp(Weight, 0.0, 1.0)});
                }
            }
        }
    }
    else if (Settings.Connectivity == ENGonSoftConnectivity::FloodWithinRadius)
    {
        GScratch.FloodQueue.Sort();
        for (const int32 Vertex : GScratch.FloodQueue)
        {
            if (GScratch.SeedStamp[Vertex] == SeedMark)
            {
                continue;
            }
            const double SurfDist = GScratch.SurfaceDistances[Vertex];
            const double SurfaceWeight = (bNeedSurface && SurfDist <= Radius) ? SmoothWeight(SurfDist, Radius) : 0.0;
            const double VolDistSq = FVector::DistSquared(WorldPositions[Vertex], SeedCenter);
            const double VolumeWeight = (VolDistSq <= RadiusSquared) ? SmoothWeight(FMath::Sqrt(VolDistSq), Radius) : 0.0;
            const double Weight = (Settings.Metric == ENGonSoftMetric::Volume)
                ? VolumeWeight
                : ((1.0 - ClampedBlend) * VolumeWeight + ClampedBlend * SurfaceWeight);
            if (Weight > 0.0 && FMath::IsFinite(Weight))
            {
                Influences.Add({Vertex, FMath::Clamp(Weight, 0.0, 1.0)});
            }
        }
        for (const int32 Vertex : GScratch.TouchedVertices)
        {
            GScratch.SurfaceDistances[Vertex] = TNumericLimits<double>::Max();
        }
    }
    else
    {
        for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex)
        {
            if (GScratch.SeedStamp[Vertex] == SeedMark || !IsVertexAllowed(Vertex))
            {
                continue;
            }
            const double SurfDist = GScratch.SurfaceDistances[Vertex];
            const double SurfaceWeight = (bNeedSurface && SurfDist <= Radius) ? SmoothWeight(SurfDist, Radius) : 0.0;
            const double VolDistSq = FVector::DistSquared(WorldPositions[Vertex], SeedCenter);
            const double VolumeWeight = (VolDistSq <= RadiusSquared) ? SmoothWeight(FMath::Sqrt(VolDistSq), Radius) : 0.0;
            const double Weight = (Settings.Metric == ENGonSoftMetric::Volume)
                ? VolumeWeight
                : ((1.0 - ClampedBlend) * VolumeWeight + ClampedBlend * SurfaceWeight);
            if (Weight > 0.0 && FMath::IsFinite(Weight))
            {
                Influences.Add({Vertex, FMath::Clamp(Weight, 0.0, 1.0)});
            }
        }
        for (const int32 Vertex : GScratch.TouchedVertices)
        {
            GScratch.SurfaceDistances[Vertex] = TNumericLimits<double>::Max();
        }
    }

    for (const int32 FaceIdx : GScratch.TouchedFaceNodes)
    {
        GScratch.FaceNodeDistances[FaceIdx] = TNumericLimits<double>::Max();
    }

    return Influences;
}

void FNGonSoftSelectionSolver::Reset()
{
    Influences.Reset();
}

bool FNGonSoftTransformSession::Begin(const FNGonTopology& Topology, const TArray<FVector>& Positions,
    TConstArrayView<int32> InSeeds, const FTransform& InObjectTransform, const FTransform& InInitialGizmo,
    const FNGonSoftSelectionSettings& Settings, ENGonSoftTransformMode InMode)
{
    Cancel();
    if (Positions.IsEmpty())
    {
        return false;
    }
    Seeds.Reset();
    for (const int32 Seed : InSeeds)
    {
        if (Positions.IsValidIndex(Seed))
        {
            Seeds.AddUnique(Seed);
        }
    }
    if (Seeds.IsEmpty())
    {
        return false;
    }
    StartPositions = Positions;
    WorkingPositions = Positions;
    ObjectTransform = InObjectTransform;
    InitialGizmo = InInitialGizmo;
    LastGizmo = InInitialGizmo;
    Mode = InMode;
    StartInfluences = Solver.Solve(Topology, StartPositions, Seeds, ObjectTransform, Settings);
    CurrentInfluences = StartInfluences;
    bActive = !StartInfluences.IsEmpty();
    return bActive;
}

bool FNGonSoftTransformSession::BuildPositions(const FNGonTopology& Topology, const TArray<FVector>& CurrentPositions,
    const FTransform& NewGizmo, const FNGonSoftSelectionSettings& Settings, TArray<FVector>& OutPositions)
{
    if (!bActive || CurrentPositions.Num() != StartPositions.Num())
    {
        return false;
    }

    const bool bInPlaceUpdate = (&OutPositions == &CurrentPositions);
    const FTransform& ReferenceGizmo = (Mode == ENGonSoftTransformMode::SlideSculpt) ? LastGizmo : InitialGizmo;
    const FMatrix CombinedLocalMatrix =
        ObjectTransform.ToMatrixWithScale() *
        ReferenceGizmo.ToInverseMatrixWithScale() *
        NewGizmo.ToMatrixWithScale() *
        ObjectTransform.ToInverseMatrixWithScale();

    const TArray<FNGonVertexInfluence>& ActiveInfluences =
        ((Mode == ENGonSoftTransformMode::LiveFalloff || Mode == ENGonSoftTransformMode::SlideSculpt) && !CurrentInfluences.IsEmpty())
            ? CurrentInfluences
            : StartInfluences;

    if (Mode == ENGonSoftTransformMode::SlideSculpt)
    {
        if (!bInPlaceUpdate || OutPositions.Num() != WorkingPositions.Num())
        {
            OutPositions = WorkingPositions;
        }
        for (const FNGonVertexInfluence& Influence : ActiveInfluences)
        {
            if (!WorkingPositions.IsValidIndex(Influence.Vertex))
            {
                continue;
            }
            const FVector BaseLocal = WorkingPositions[Influence.Vertex];
            const FVector TargetLocal = CombinedLocalMatrix.TransformPosition(BaseLocal);
            const FVector UpdatedPos = FMath::Lerp(BaseLocal, TargetLocal, static_cast<float>(Influence.Weight));
            WorkingPositions[Influence.Vertex] = UpdatedPos;
            OutPositions[Influence.Vertex] = UpdatedPos;
        }
    }
    else
    {
        if (!bInPlaceUpdate || OutPositions.Num() != StartPositions.Num())
        {
            OutPositions = StartPositions;
        }
        else if (Mode == ENGonSoftTransformMode::LiveFalloff)
        {
            for (const FNGonVertexInfluence& Prev : StartInfluences)
            {
                if (StartPositions.IsValidIndex(Prev.Vertex))
                {
                    OutPositions[Prev.Vertex] = StartPositions[Prev.Vertex];
                }
            }
            for (const FNGonVertexInfluence& Prev : CurrentInfluences)
            {
                if (StartPositions.IsValidIndex(Prev.Vertex))
                {
                    OutPositions[Prev.Vertex] = StartPositions[Prev.Vertex];
                }
            }
        }

        for (const FNGonVertexInfluence& Influence : ActiveInfluences)
        {
            if (!StartPositions.IsValidIndex(Influence.Vertex))
            {
                continue;
            }
            const FVector BaseLocal = StartPositions[Influence.Vertex];
            const FVector TargetLocal = CombinedLocalMatrix.TransformPosition(BaseLocal);
            OutPositions[Influence.Vertex] = FMath::Lerp(BaseLocal, TargetLocal, static_cast<float>(Influence.Weight));
        }
    }

    if ((Mode == ENGonSoftTransformMode::LiveFalloff || Mode == ENGonSoftTransformMode::SlideSculpt) && Settings.bEnabled)
    {
        CurrentInfluences = Solver.Solve(Topology, OutPositions, Seeds, ObjectTransform, Settings);
    }
    LastGizmo = NewGizmo;
    return true;
}

void FNGonSoftTransformSession::End()
{
    bActive = false;
}

void FNGonSoftTransformSession::Cancel()
{
    bActive = false;
    StartInfluences.Reset();
    CurrentInfluences.Reset();
    StartPositions.Reset();
    WorkingPositions.Reset();
    Seeds.Reset();
    InitialGizmo = FTransform::Identity;
    LastGizmo = FTransform::Identity;
}
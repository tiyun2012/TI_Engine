#include "NGonBevel.h"
#include "Algo/Reverse.h"

namespace
{
constexpr double BEVEL_EPSILON = 1e-8;
constexpr double BEVEL_SMALL_ANGLE_RAD = 10.0 * PI / 180.0;   // Blender small-angle fallback for warped n-gon local normal
constexpr double BEVEL_PARALLEL_ANGLE_RAD = 2.0 * PI / 180.0; // Blender offset_meet near-parallel branch

FORCEINLINE uint64 MakeEdgeKey(int32 A, int32 B)
{
    const uint32 Lo = static_cast<uint32>(FMath::Min(A, B));
    const uint32 Hi = static_cast<uint32>(FMath::Max(A, B));
    return (static_cast<uint64>(Lo) << 32) | static_cast<uint64>(Hi);
}

FORCEINLINE uint64 MakeCornerKey(int32 FaceIdx, int32 VertIdx)
{
    return (static_cast<uint64>(static_cast<uint32>(FaceIdx)) << 32) | static_cast<uint64>(static_cast<uint32>(VertIdx));
}

FVector ComputeNewellNormal(const TArray<FVector>& Positions, const TArray<int32>& Loop)
{
    const int32 N = Loop.Num();
    if (N < 3)
    {
        return FVector::UpVector;
    }
    FVector Normal = FVector::ZeroVector;
    for (int32 i = 0; i < N; ++i)
    {
        const FVector& A = Positions[Loop[i]];
        const FVector& B = Positions[Loop[(i + 1) % N]];
        Normal.X += (A.Y - B.Y) * (A.Z + B.Z);
        Normal.Y += (A.Z - B.Z) * (A.X + B.X);
        Normal.Z += (A.X - B.X) * (A.Y + B.Y);
    }
    const double LenSq = Normal.SizeSquared();
    if (!FMath::IsFinite(LenSq) || LenSq <= 1e-18)
    {
        return FVector::UpVector;
    }
    return Normal / FMath::Sqrt(LenSq);
}

FVector2D ProjectDominant(const FVector& P, const FVector& Normal)
{
    const double Ax = FMath::Abs(Normal.X);
    const double Ay = FMath::Abs(Normal.Y);
    const double Az = FMath::Abs(Normal.Z);
    if (Az >= Ax && Az >= Ay)
    {
        return Normal.Z >= 0.0 ? FVector2D(P.X, P.Y) : FVector2D(P.Y, P.X);
    }
    if (Ax >= Ay)
    {
        return Normal.X >= 0.0 ? FVector2D(P.Y, P.Z) : FVector2D(P.Z, P.Y);
    }
    return Normal.Y >= 0.0 ? FVector2D(P.Z, P.X) : FVector2D(P.X, P.Z);
}

double PolygonSignedArea2D(const TArray<FVector2D>& Poly)
{
    const int32 N = Poly.Num();
    if (N < 3)
    {
        return 0.0;
    }
    double Sum = 0.0;
    for (int32 i = 0; i < N; ++i)
    {
        const FVector2D& A = Poly[i];
        const FVector2D& B = Poly[(i + 1) % N];
        Sum += A.X * B.Y - B.X * A.Y;
    }
    return 0.5 * Sum;
}

double Cross2D(const FVector2D& A, const FVector2D& B, const FVector2D& C)
{
    return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
}

bool SegmentsProperlyIntersectOrOverlap2D(
    const FVector2D& A, const FVector2D& B,
    const FVector2D& C, const FVector2D& D,
    double Eps)
{
    if (FMath::Max(A.X, B.X) < FMath::Min(C.X, D.X) - Eps ||
        FMath::Max(C.X, D.X) < FMath::Min(A.X, B.X) - Eps ||
        FMath::Max(A.Y, B.Y) < FMath::Min(C.Y, D.Y) - Eps ||
        FMath::Max(C.Y, D.Y) < FMath::Min(A.Y, B.Y) - Eps)
    {
        return false;
    }

    const double O1 = Cross2D(A, B, C);
    const double O2 = Cross2D(A, B, D);
    const double O3 = Cross2D(C, D, A);
    const double O4 = Cross2D(C, D, B);

    if (((O1 > Eps && O2 < -Eps) || (O1 < -Eps && O2 > Eps)) &&
        ((O3 > Eps && O4 < -Eps) || (O3 < -Eps && O4 > Eps)))
    {
        return true;
    }

    auto OnSeg = [Eps](const FVector2D& P, const FVector2D& Q, const FVector2D& R) -> bool
    {
        return R.X <= FMath::Max(P.X, Q.X) + Eps && R.X >= FMath::Min(P.X, Q.X) - Eps &&
               R.Y <= FMath::Max(P.Y, Q.Y) + Eps && R.Y >= FMath::Min(P.Y, Q.Y) - Eps;
    };

    if (FMath::Abs(O1) <= Eps && OnSeg(A, B, C)) return true;
    if (FMath::Abs(O2) <= Eps && OnSeg(A, B, D)) return true;
    if (FMath::Abs(O3) <= Eps && OnSeg(C, D, A)) return true;
    if (FMath::Abs(O4) <= Eps && OnSeg(C, D, B)) return true;

    return false;
}

bool PolygonIsSimple2D(const TArray<FVector2D>& Poly, double Eps)
{
    const int32 N = Poly.Num();
    if (N < 3) return false;
    for (int32 i = 0; i < N; ++i)
    {
        const FVector2D& A = Poly[i];
        const FVector2D& B = Poly[(i + 1) % N];
        for (int32 j = i + 1; j < N; ++j)
        {
            if (j == i || j == (i + 1) % N || (j + 1) % N == i)
            {
                continue;
            }
            const FVector2D& C = Poly[j];
            const FVector2D& D = Poly[(j + 1) % N];
            if (SegmentsProperlyIntersectOrOverlap2D(A, B, C, D, Eps))
            {
                return false;
            }
        }
    }
    return true;
}

double ProfileSuperellipseExponent(double Profile)
{
    const double Clamped = FMath::Clamp(Profile, 0.05, 0.9999);
    if (FMath::Abs(Clamped - 0.25) <= 1e-6)
    {
        return 1.0;
    }
    if (FMath::Abs(Clamped - 0.5) <= 1e-6)
    {
        return 2.0;
    }
    const double Denom = FMath::Loge(FMath::Sqrt(Clamped));
    if (FMath::Abs(Denom) <= 1e-9)
    {
        return 1e3;
    }
    return FMath::Clamp(-FMath::Loge(2.0) / Denom, 1e-3, 1e3);
}

TArray<FVector2D> BuildEvenProfileSamples(int32 Segments, double Profile)
{
    TArray<FVector2D> Samples;
    Samples.Reserve(Segments + 1);
    Samples.Add(FVector2D(0.0, 1.0));
    if (Segments <= 1)
    {
        Samples.Add(FVector2D(1.0, 0.0));
        return Samples;
    }

    const double R = ProfileSuperellipseExponent(Profile);
    if (FMath::Abs(R - 1.0) <= 1e-5)
    {
        for (int32 s = 1; s < Segments; ++s)
        {
            const double T = static_cast<double>(s) / static_cast<double>(Segments);
            Samples.Add(FVector2D(T, 1.0 - T));
        }
        Samples.Add(FVector2D(1.0, 0.0));
        return Samples;
    }

    constexpr int32 DenseSteps = 256;
    TArray<FVector2D> DensePts;
    TArray<double> CumLen;
    DensePts.Reserve(DenseSteps + 1);
    CumLen.Reserve(DenseSteps + 1);

    const double InvR = 1.0 / R;
    for (int32 i = 0; i <= DenseSteps; ++i)
    {
        const double Theta = (0.5 * PI) * (static_cast<double>(i) / static_cast<double>(DenseSteps));
        const double SinT = FMath::Max(0.0, FMath::Sin(Theta));
        const double CosT = FMath::Max(0.0, FMath::Cos(Theta));
        const double X = FMath::Pow(SinT, 2.0 * InvR);
        const double Y = FMath::Pow(CosT, 2.0 * InvR);
        const FVector2D Pt(X, Y);
        if (i == 0)
        {
            CumLen.Add(0.0);
        }
        else
        {
            CumLen.Add(CumLen.Last() + FVector2D::Distance(DensePts.Last(), Pt));
        }
        DensePts.Add(Pt);
    }

    const double TotalLen = CumLen.Last();
    if (TotalLen <= BEVEL_EPSILON)
    {
        for (int32 s = 1; s < Segments; ++s)
        {
            const double T = static_cast<double>(s) / static_cast<double>(Segments);
            Samples.Add(FVector2D(T, 1.0 - T));
        }
        Samples.Add(FVector2D(1.0, 0.0));
        return Samples;
    }

    int32 Cursor = 0;
    for (int32 s = 1; s < Segments; ++s)
    {
        const double Target = TotalLen * (static_cast<double>(s) / static_cast<double>(Segments));
        while (Cursor + 1 < DenseSteps && CumLen[Cursor + 1] < Target)
        {
            ++Cursor;
        }
        const double SegLen = CumLen[Cursor + 1] - CumLen[Cursor];
        const double Alpha = SegLen > 1e-12 ? FMath::Clamp((Target - CumLen[Cursor]) / SegLen, 0.0, 1.0) : 0.0;
        Samples.Add(FMath::Lerp(DensePts[Cursor], DensePts[Cursor + 1], Alpha));
    }
    Samples.Add(FVector2D(1.0, 0.0));
    return Samples;
}

FVector MapProfileSample(const FVector& Start, const FVector& Middle, const FVector& End, const FVector2D& Sample)
{
    const FVector Origin = Start + End - Middle;
    return Origin + (End - Origin) * Sample.X + (Start - Origin) * Sample.Y;
}

bool LineIntersectionInPlane(
    const FVector& P1, const FVector& D1,
    const FVector& P2, const FVector& D2,
    const FVector& PlaneNormal,
    FVector& OutPoint)
{
    const FVector Cross = FVector::CrossProduct(D1, D2);
    const double Denom = FVector::DotProduct(Cross, PlaneNormal);
    if (!FMath::IsFinite(Denom) || FMath::Abs(Denom) <= 1e-9)
    {
        return false;
    }
    const FVector Diff = P2 - P1;
    const double T = FVector::DotProduct(FVector::CrossProduct(Diff, D2), PlaneNormal) / Denom;
    if (!FMath::IsFinite(T))
    {
        return false;
    }
    OutPoint = P1 + D1 * T;
    return FMath::IsFinite(OutPoint.X) && FMath::IsFinite(OutPoint.Y) && FMath::IsFinite(OutPoint.Z);
}

bool ClosestPointBetweenLines(
    const FVector& P1, const FVector& D1,
    const FVector& P2, const FVector& D2,
    FVector& OutPointOnLine1)
{
    const FVector W0 = P1 - P2;
    const double A = FVector::DotProduct(D1, D1);
    const double B = FVector::DotProduct(D1, D2);
    const double C = FVector::DotProduct(D2, D2);
    const double D = FVector::DotProduct(D1, W0);
    const double E = FVector::DotProduct(D2, W0);
    const double Denom = A * C - B * B;
    if (!FMath::IsFinite(Denom) || FMath::Abs(Denom) <= 1e-10)
    {
        return false;
    }
    const double Sc = (B * E - C * D) / Denom;
    if (!FMath::IsFinite(Sc))
    {
        return false;
    }
    OutPointOnLine1 = P1 + D1 * Sc;
    return true;
}

FVector ProjectAlongDirectionToPlane(
    const FVector& Point,
    const FVector& Direction,
    const FVector& PlanePoint,
    const FVector& PlaneNormal)
{
    const double Denom = FVector::DotProduct(Direction, PlaneNormal);
    if (FMath::Abs(Denom) <= 1e-6)
    {
        return Point;
    }
    const double T = FVector::DotProduct(PlanePoint - Point, PlaneNormal) / Denom;
    if (!FMath::IsFinite(T))
    {
        return Point;
    }
    return Point + Direction * T;
}

struct FLocalCornerFrame
{
    FVector Normal = FVector::UpVector;
    bool bReflex = false;
};

FLocalCornerFrame BuildLocalCornerFrame(
    const FVector& Vertex,
    const FVector& Prev,
    const FVector& Next,
    const FVector& FaceNormal)
{
    FLocalCornerFrame Out;
    Out.Normal = FaceNormal;
    Out.bReflex = false;

    const FVector InVec = Vertex - Prev;
    const FVector OutVec = Next - Vertex;
    const double InLen = InVec.Size();
    const double OutLen = OutVec.Size();
    if (InLen <= BEVEL_EPSILON || OutLen <= BEVEL_EPSILON)
    {
        return Out;
    }

    const FVector InDir = InVec / InLen;
    const FVector OutDir = OutVec / OutLen;
    const double Dot = FMath::Clamp(FVector::DotProduct(InDir, OutDir), -1.0, 1.0);
    const double Angle = FMath::Acos(Dot);
    const FVector Cross = FVector::CrossProduct(InDir, OutDir);
    const double CrossLen = Cross.Size();

    Out.bReflex = FVector::DotProduct(Cross, FaceNormal) < 0.0;
    if (CrossLen <= 1e-7 || Angle < BEVEL_SMALL_ANGLE_RAD || (PI - Angle) < BEVEL_SMALL_ANGLE_RAD)
    {
        return Out;
    }
    const FVector LocalN = Cross / CrossLen;
    Out.Normal = Out.bReflex ? -LocalN : LocalN;
    return Out;
}

struct FFaceCorner
{
    int32 FaceIdx = INDEX_NONE;
    int32 VertexIdx = INDEX_NONE;
    int32 PrevIdx = INDEX_NONE;
    int32 NextIdx = INDEX_NONE;
    FVector FaceNormal = FVector::UpVector;
    FVector Normal = FVector::UpVector;
    bool bReflex = false;
};

struct FEdgeIncident
{
    int32 FaceIdx = INDEX_NONE;
    int32 FromIdx = INDEX_NONE;
    int32 ToIdx = INDEX_NONE;
};

struct FFaceSector
{
    int32 FaceIdx = INDEX_NONE;
    int32 NeighborA = INDEX_NONE;
    int32 NeighborB = INDEX_NONE;
};

struct FVertexStar
{
    int32 VertexIdx = INDEX_NONE;
    TArray<int32> Neighbors;
    TArray<FFaceSector> Sectors;
    bool bClosed = false;
};

struct FTopologyIndex
{
    TMap<int32, TArray<FFaceCorner>> CornersByVertex;
    TMap<uint64, FFaceCorner> CornersByFaceVertex;
    TMap<uint64, TArray<FEdgeIncident>> EdgeFaces;
    TArray<FVector> FaceNormals;
};

bool BuildTopologyIndex(
    const TArray<FVector>& Positions,
    const TArray<TArray<int32>>& Faces,
    FTopologyIndex& OutIndex,
    FString& OutError)
{
    OutIndex.FaceNormals.SetNum(Faces.Num());
    for (int32 FaceIdx = 0; FaceIdx < Faces.Num(); ++FaceIdx)
    {
        const TArray<int32>& Perim = Faces[FaceIdx];
        if (Perim.Num() < 3)
        {
            OutError = FString::Printf(TEXT("Bevel found degenerate face %d with fewer than 3 vertices."), FaceIdx);
            return false;
        }
        for (const int32 V : Perim)
        {
            if (!Positions.IsValidIndex(V))
            {
                OutError = FString::Printf(TEXT("Bevel found invalid vertex index %d in face %d."), V, FaceIdx);
                return false;
            }
        }
        const FVector FaceNormal = ComputeNewellNormal(Positions, Perim);
        OutIndex.FaceNormals[FaceIdx] = FaceNormal;

        const int32 N = Perim.Num();
        for (int32 i = 0; i < N; ++i)
        {
            const int32 VertIdx = Perim[i];
            const int32 PrevIdx = Perim[(i - 1 + N) % N];
            const int32 NextIdx = Perim[(i + 1) % N];
            const FLocalCornerFrame Local = BuildLocalCornerFrame(
                Positions[VertIdx], Positions[PrevIdx], Positions[NextIdx], FaceNormal);

            FFaceCorner Corner;
            Corner.FaceIdx = FaceIdx;
            Corner.VertexIdx = VertIdx;
            Corner.PrevIdx = PrevIdx;
            Corner.NextIdx = NextIdx;
            Corner.FaceNormal = FaceNormal;
            Corner.Normal = Local.Normal;
            Corner.bReflex = Local.bReflex;

            OutIndex.CornersByVertex.FindOrAdd(VertIdx).Add(Corner);
            OutIndex.CornersByFaceVertex.Add(MakeCornerKey(FaceIdx, VertIdx), Corner);

            const uint64 EKey = MakeEdgeKey(VertIdx, NextIdx);
            OutIndex.EdgeFaces.FindOrAdd(EKey).Add({ FaceIdx, VertIdx, NextIdx });
        }
    }
    return true;
}

bool BuildVertexStar(
    int32 VertexIdx,
    const TArray<FFaceCorner>& Corners,
    FVertexStar& OutStar,
    FString& OutError)
{
    struct FLink
    {
        int32 To = INDEX_NONE;
        int32 FaceIdx = INDEX_NONE;
    };

    TMap<int32, TArray<FLink>> Adjacency;
    for (const FFaceCorner& C : Corners)
    {
        if (C.PrevIdx == C.NextIdx || C.PrevIdx == VertexIdx || C.NextIdx == VertexIdx)
        {
            OutError = FString::Printf(TEXT("Vertex %d has a degenerate incident face corner."), VertexIdx);
            return false;
        }
        Adjacency.FindOrAdd(C.PrevIdx).Add({ C.NextIdx, C.FaceIdx });
        Adjacency.FindOrAdd(C.NextIdx).Add({ C.PrevIdx, C.FaceIdx });
    }

    TArray<int32> Neighbors;
    Adjacency.GetKeys(Neighbors);
    Neighbors.Sort();
    if (Neighbors.Num() < 2)
    {
        OutError = FString::Printf(TEXT("Vertex %d has fewer than 2 incident edges."), VertexIdx);
        return false;
    }

    TArray<int32> DegreeOne;
    for (const int32 Nbr : Neighbors)
    {
        const int32 Deg = Adjacency[Nbr].Num();
        if (Deg < 1 || Deg > 2)
        {
            OutError = FString::Printf(TEXT("Vertex %d has a non-manifold branching fan."), VertexIdx);
            return false;
        }
        if (Deg == 1)
        {
            DegreeOne.Add(Nbr);
        }
    }
    if (DegreeOne.Num() != 0 && DegreeOne.Num() != 2)
    {
        OutError = FString::Printf(TEXT("Vertex %d has multiple disconnected boundary gaps."), VertexIdx);
        return false;
    }

    const bool bClosed = (DegreeOne.Num() == 0);
    const int32 StartNeighbor = bClosed ? Neighbors[0] : DegreeOne[0];

    OutStar.VertexIdx = VertexIdx;
    OutStar.bClosed = bClosed;
    OutStar.Neighbors.Reset();
    OutStar.Sectors.Reset();
    OutStar.Neighbors.Add(StartNeighbor);

    int32 Previous = INDEX_NONE;
    int32 Current = StartNeighbor;
    TSet<int32> UsedFaces;

    while (true)
    {
        const TArray<FLink>& Links = Adjacency[Current];
        const FLink* NextLink = nullptr;
        for (const FLink& L : Links)
        {
            if (L.To != Previous && !UsedFaces.Contains(L.FaceIdx))
            {
                NextLink = &L;
                break;
            }
        }
        if (!NextLink)
        {
            for (const FLink& L : Links)
            {
                if (!UsedFaces.Contains(L.FaceIdx))
                {
                    NextLink = &L;
                    break;
                }
            }
        }
        if (!NextLink)
        {
            break;
        }
        UsedFaces.Add(NextLink->FaceIdx);
        OutStar.Sectors.Add({ NextLink->FaceIdx, Current, NextLink->To });
        Previous = Current;
        Current = NextLink->To;
        if (bClosed && Current == OutStar.Neighbors[0])
        {
            break;
        }
        OutStar.Neighbors.Add(Current);
        if (OutStar.Neighbors.Num() > Neighbors.Num())
        {
            OutError = FString::Printf(TEXT("Vertex %d fan traversal did not terminate."), VertexIdx);
            return false;
        }
    }

    const bool bComplete = bClosed
        ? (Current == OutStar.Neighbors[0])
        : DegreeOne.Contains(Current);
    if (!bComplete || UsedFaces.Num() != Corners.Num() || OutStar.Neighbors.Num() != Neighbors.Num())
    {
        OutError = FString::Printf(TEXT("Vertex %d fan is disconnected or non-manifold."), VertexIdx);
        return false;
    }
    return true;
}

struct FOffsetLine
{
    FVector Point = FVector::ZeroVector;
    FVector Direction = FVector::ForwardVector;
};

bool FaceOffsetLine(
    const TArray<FVector>& Positions,
    const FFaceCorner& Corner,
    int32 NeighborIdx,
    double Width,
    FOffsetLine& OutLine)
{
    const FVector& V = Positions[Corner.VertexIdx];
    FVector RawDir;
    if (NeighborIdx == Corner.PrevIdx)
    {
        RawDir = V - Positions[Corner.PrevIdx];
    }
    else if (NeighborIdx == Corner.NextIdx)
    {
        RawDir = Positions[Corner.NextIdx] - V;
    }
    else
    {
        return false;
    }
    const double Len = RawDir.Size();
    if (Len <= BEVEL_EPSILON)
    {
        return false;
    }
    const FVector Dir = RawDir / Len;
    const FVector InwardRaw = FVector::CrossProduct(Corner.Normal, Dir);
    const double InLen = InwardRaw.Size();
    if (InLen <= BEVEL_EPSILON)
    {
        return false;
    }
    OutLine.Point = V + (InwardRaw / InLen) * Width;
    OutLine.Direction = Dir;
    return true;
}

bool TerminalOffsetInFacePlane(
    const TArray<FVector>& Positions,
    const FFaceCorner& Corner,
    int32 SelectedNeighborIdx,
    double Width,
    FVector& OutPoint)
{
    const FVector& V = Positions[Corner.VertexIdx];
    const FVector EdgeVec = Positions[SelectedNeighborIdx] - V;
    const double EdgeLen = EdgeVec.Size();
    if (EdgeLen <= BEVEL_EPSILON)
    {
        return false;
    }
    const FVector EdgeRay = EdgeVec / EdgeLen;
    const FVector FaceN = Corner.FaceNormal.GetSafeNormal();
    FVector InwardRaw;
    if (Corner.NextIdx == SelectedNeighborIdx)
    {
        InwardRaw = FVector::CrossProduct(FaceN, EdgeRay);
    }
    else if (Corner.PrevIdx == SelectedNeighborIdx)
    {
        InwardRaw = FVector::CrossProduct(EdgeRay, FaceN);
    }
    else
    {
        return false;
    }
    const double InLen = InwardRaw.Size();
    if (InLen <= BEVEL_EPSILON)
    {
        return false;
    }
    OutPoint = V + (InwardRaw / InLen) * Width;
    return true;
}

bool IntersectSelectedPair(
    const TArray<FVector>& Positions,
    const FFaceCorner& Corner,
    double Width,
    FVector& OutPoint)
{
    FOffsetLine PrevLine, NextLine;
    if (!FaceOffsetLine(Positions, Corner, Corner.PrevIdx, Width, PrevLine) ||
        !FaceOffsetLine(Positions, Corner, Corner.NextIdx, Width, NextLine))
    {
        return false;
    }
    if (LineIntersectionInPlane(PrevLine.Point, PrevLine.Direction, NextLine.Point, NextLine.Direction, Corner.Normal, OutPoint))
    {
        return true;
    }
    const double Align = FVector::DotProduct(PrevLine.Direction, NextLine.Direction);
    if (Align > 1.0 - 1e-6)
    {
        OutPoint = 0.5 * (PrevLine.Point + NextLine.Point);
        return true;
    }
    return false;
}

struct FEdgeMeetCandidate
{
    FVector Position = FVector::ZeroVector;
    double Distance = 0.0;
    double RawDistance = 0.0;
    bool bValidMeet = false;
    bool bOnUnselectedEdge = true;
};

bool IntersectSelectedWithUnselected(
    const TArray<FVector>& Positions,
    const FFaceCorner& Corner,
    int32 SelectedNeighborIdx,
    int32 UnselectedNeighborIdx,
    double Width,
    FEdgeMeetCandidate& OutCand)
{
    const FVector& V = Positions[Corner.VertexIdx];
    const FVector& NbrPos = Positions[UnselectedNeighborIdx];
    FOffsetLine SelLine, UnselLine;
    if (!FaceOffsetLine(Positions, Corner, SelectedNeighborIdx, Width, SelLine) ||
        !FaceOffsetLine(Positions, Corner, UnselectedNeighborIdx, 0.0, UnselLine))
    {
        return false;
    }

    const FVector EdgeVec = NbrPos - V;
    const double EdgeLen = EdgeVec.Size();
    if (EdgeLen <= BEVEL_EPSILON)
    {
        return false;
    }
    const FVector Ray = EdgeVec / EdgeLen;
    const double Align = FMath::Clamp(FVector::DotProduct(SelLine.Direction, UnselLine.Direction), -1.0, 1.0);
    const double Angle = FMath::Acos(Align);

    if (Angle < BEVEL_PARALLEL_ANGLE_RAD)
    {
        const FVector OffsetVec = SelLine.Point - V;
        const double OffsetLen = OffsetVec.Size();
        const FVector Inward = OffsetLen > BEVEL_EPSILON
            ? (OffsetVec / OffsetLen)
            : FVector::CrossProduct(Corner.Normal, SelLine.Direction).GetSafeNormal();
        const double Corrected = Width / FMath::Max(FMath::Cos(Angle * 0.5), 1e-6);
        OutCand.Position = V + Inward * Corrected;
        OutCand.Distance = 0.0;
        OutCand.RawDistance = 0.0;
        OutCand.bValidMeet = true;
        OutCand.bOnUnselectedEdge = false;
        return true;
    }

    FVector Intersect;
    if (!LineIntersectionInPlane(SelLine.Point, SelLine.Direction, V, Ray, Corner.Normal, Intersect))
    {
        if (Align > 1.0 - 1e-6)
        {
            OutCand.Position = SelLine.Point;
            OutCand.Distance = 0.0;
            OutCand.RawDistance = 0.0;
            OutCand.bValidMeet = true;
            OutCand.bOnUnselectedEdge = false;
            return true;
        }
        if (Align < -1.0 + 1e-6)
        {
            const double Dist = FMath::Min(Width, EdgeLen);
            OutCand.Position = V + Ray * Dist;
            OutCand.Distance = Dist;
            OutCand.RawDistance = Width;
            OutCand.bValidMeet = Dist < EdgeLen - BEVEL_EPSILON;
            OutCand.bOnUnselectedEdge = true;
            return true;
        }
        OutCand.Position = V;
        OutCand.Distance = 0.0;
        OutCand.RawDistance = 0.0;
        OutCand.bValidMeet = false;
        OutCand.bOnUnselectedEdge = true;
        return true;
    }

    const double RawDist = FVector::DotProduct(Intersect - V, Ray);
    if (!FMath::IsFinite(RawDist))
    {
        return false;
    }
    if (RawDist <= BEVEL_EPSILON)
    {
        OutCand.Position = V;
        OutCand.Distance = 0.0;
        OutCand.RawDistance = RawDist;
        OutCand.bValidMeet = false;
        OutCand.bOnUnselectedEdge = true;
        return true;
    }
    if (RawDist >= EdgeLen - BEVEL_EPSILON)
    {
        OutCand.Position = NbrPos;
        OutCand.Distance = EdgeLen;
        OutCand.RawDistance = RawDist;
        OutCand.bValidMeet = false;
        OutCand.bOnUnselectedEdge = true;
        return true;
    }
    OutCand.Position = Intersect;
    OutCand.Distance = RawDist;
    OutCand.RawDistance = RawDist;
    OutCand.bValidMeet = true;
    OutCand.bOnUnselectedEdge = true;
    return true;
}

enum class ECornerMeshKind : uint8
{
    None,
    Poly,
    TriFan,
    Adj
};

struct FGeneratedPoint
{
    FString Key;
    int32 SourceVertexIdx = INDEX_NONE;
    FVector Position = FVector::ZeroVector;
    bool bHasEdgeInset = false;
    uint64 InsetEdgeKey = 0;
    int32 InsetEndpointIdx = INDEX_NONE;
    double InsetDistance = 0.0;
};

struct FCornerPlan
{
    int32 SourceVertexIdx = INDEX_NONE;
    TArray<uint64> SelectedEdgeKeys;
    TArray<FString> BoundaryPointKeys;
    TArray<FString> BaseBoundaryPointKeys;
    bool bClosed = false;
    ECornerMeshKind MeshKind = ECornerMeshKind::None;
    FString FanPointKey;
    TArray<TArray<FString>> AdjFacePointKeys; // Ordered outer-ring to inner-ring, then center poly
};

struct FEdgePlan
{
    uint64 EdgeKey = 0;
    int32 PointA = INDEX_NONE;
    int32 PointB = INDEX_NONE;
    int32 ForwardFaceIdx = INDEX_NONE;
    int32 ReverseFaceIdx = INDEX_NONE;
    FString BevelPointKeys[4]; // [aForward, aReverse, bReverse, bForward]
    TArray<FString> ProfileA;  // Length = Segments + 1
    TArray<FString> ProfileB;  // Length = Segments + 1
};

struct FBevelSolveResult
{
    bool bOk = false;
    bool bCollisionError = false;
    FString Error;
    double OverlapScale = 1.0;

    TMap<FString, FGeneratedPoint> GeneratedByKey;
    TMap<FString, FString> CanonicalKeyByKey;
    TMap<uint64, TArray<FString>> Replacements; // CornerKey(FaceIdx, VertIdx) -> replacement point keys
    TArray<FEdgePlan> EdgePlans;
    TArray<FCornerPlan> Corners;

    FString Canonical(const FString& Key) const
    {
        FString Cur = Key;
        TSet<FString> Visited;
        while (const FString* Next = CanonicalKeyByKey.Find(Cur))
        {
            if (*Next == Cur || Visited.Contains(Cur))
            {
                break;
            }
            Visited.Add(Cur);
            Cur = *Next;
        }
        return Cur;
    }
};

TArray<int32> CompactCyclicVertexIds(const TArray<int32>& RawIds)
{
    TArray<int32> Out;
    Out.Reserve(RawIds.Num());
    for (const int32 V : RawIds)
    {
        if (Out.IsEmpty() || Out.Last() != V)
        {
            Out.Add(V);
        }
    }
    while (Out.Num() >= 2 && Out[0] == Out.Last())
    {
        Out.Pop();
    }
    return Out;
}

TArray<FString> CompactUniqueKeys(const TArray<FString>& Keys)
{
    TArray<FString> Out;
    TSet<FString> Seen;
    for (const FString& K : Keys)
    {
        if (!Seen.Contains(K))
        {
            Seen.Add(K);
            Out.Add(K);
        }
    }
    return Out;
}

int32 DirectedEdgeRelation(const TArray<int32>& Loop, int32 A, int32 B)
{
    const int32 N = Loop.Num();
    for (int32 i = 0; i < N; ++i)
    {
        const int32 U = Loop[i];
        const int32 V = Loop[(i + 1) % N];
        if (U == A && V == B) return 1;
        if (U == B && V == A) return -1;
    }
    return 0;
}

FBevelSolveResult SolveEdgesAtWidth(
    const TArray<FVector>& Positions,
    const TArray<TArray<int32>>& Faces,
    const FTopologyIndex& Topo,
    const TArray<FIntPoint>& SelectedEdges,
    double Width,
    int32 Segments,
    double Profile)
{
    FBevelSolveResult Res;

    TSet<uint64> SelectedKeys;
    TSet<int32> TouchedVertices;
    for (const FIntPoint& E : SelectedEdges)
    {
        SelectedKeys.Add(MakeEdgeKey(E.X, E.Y));
        TouchedVertices.Add(E.X);
        TouchedVertices.Add(E.Y);
    }

    TSet<int32> TwoSelectedProfileWeldVertices;
    TSet<int32> TerminalProfileProjectionVertices;
    TMap<uint64, FString> RailPointKeyByFaceVertex;

    auto KeyForUnselected = [](int32 V, int32 Nbr) { return FString::Printf(TEXT("u:%d:%d"), V, Nbr); };
    auto KeyForSelectedPair = [](int32 V, int32 FaceIdx) { return FString::Printf(TEXT("f:%d:%d"), V, FaceIdx); };
    auto KeyForTerminalSide = [](int32 V, int32 FaceIdx) { return FString::Printf(TEXT("t:%d:%d"), V, FaceIdx); };

    TArray<int32> SortedTouched = TouchedVertices.Array();
    SortedTouched.Sort();

    for (const int32 VertexIdx : SortedTouched)
    {
        const TArray<FFaceCorner>* VertCornersPtr = Topo.CornersByVertex.Find(VertexIdx);
        if (!VertCornersPtr || VertCornersPtr->IsEmpty())
        {
            Res.Error = FString::Printf(TEXT("Bevel vertex %d has no incident faces."), VertexIdx);
            return Res;
        }
        const TArray<FFaceCorner>& VertCorners = *VertCornersPtr;

        FVertexStar Star;
        if (!BuildVertexStar(VertexIdx, VertCorners, Star, Res.Error))
        {
            return Res;
        }

        const FVector& VertPos = Positions[VertexIdx];
        TSet<int32> SelectedNbrSet;
        for (const int32 Nbr : Star.Neighbors)
        {
            if (SelectedKeys.Contains(MakeEdgeKey(VertexIdx, Nbr)))
            {
                SelectedNbrSet.Add(Nbr);
            }
        }
        if (SelectedNbrSet.IsEmpty())
        {
            Res.Error = TEXT("Bevel lost a selected edge at a touched vertex.");
            return Res;
        }

        if (!Star.bClosed && SelectedNbrSet.Num() != 1)
        {
            Res.Error = TEXT("Multi-edge bevel at an open boundary vertex is not supported; bevel individual terminal edges or close the mesh boundary.");
            return Res;
        }

        const bool bTwoEdgeTerminal = (Star.Neighbors.Num() == 2 && SelectedNbrSet.Num() == 1);
        if (SelectedNbrSet.Num() == 1 && Star.Neighbors.Num() >= 3)
        {
            TerminalProfileProjectionVertices.Add(VertexIdx);
        }

        int32 TermSelectedNbr = INDEX_NONE;
        int32 TermUnselectedNbr = INDEX_NONE;
        if (bTwoEdgeTerminal)
        {
            for (const int32 Nbr : Star.Neighbors)
            {
                if (SelectedNbrSet.Contains(Nbr)) TermSelectedNbr = Nbr;
                else TermUnselectedNbr = Nbr;
            }
            if (TermSelectedNbr == INDEX_NONE || TermUnselectedNbr == INDEX_NONE)
            {
                Res.Error = TEXT("Bevel could not resolve two-edge terminal fan.");
                return Res;
            }

            const FVector LegVec = Positions[TermUnselectedNbr] - VertPos;
            const double LegLen = LegVec.Size();
            if (LegLen <= BEVEL_EPSILON)
            {
                Res.Error = TEXT("Degenerate edge at two-edge terminal vertex.");
                return Res;
            }
            const FVector LegRay = LegVec / LegLen;
            const FString LegKey = KeyForUnselected(VertexIdx, TermUnselectedNbr);
            FGeneratedPoint LegPt;
            LegPt.Key = LegKey;
            LegPt.SourceVertexIdx = VertexIdx;
            LegPt.Position = VertPos + LegRay * Width;
            LegPt.bHasEdgeInset = true;
            LegPt.InsetEdgeKey = MakeEdgeKey(VertexIdx, TermUnselectedNbr);
            LegPt.InsetEndpointIdx = VertexIdx;
            LegPt.InsetDistance = Width;
            Res.GeneratedByKey.Add(LegKey, LegPt);

            for (const FFaceCorner& FC : VertCorners)
            {
                FVector SidePos;
                if (!TerminalOffsetInFacePlane(Positions, FC, TermSelectedNbr, Width, SidePos))
                {
                    Res.Error = TEXT("Failed to compute terminal offset in face plane.");
                    return Res;
                }
                const FString SideKey = KeyForTerminalSide(VertexIdx, FC.FaceIdx);
                FGeneratedPoint SidePt;
                SidePt.Key = SideKey;
                SidePt.SourceVertexIdx = VertexIdx;
                SidePt.Position = SidePos;
                Res.GeneratedByKey.Add(SideKey, SidePt);
            }
        }
        else
        {
            // Solve one shared on-edge point for every unselected incident edge.
            const int32 NbrCount = Star.Neighbors.Num();
            const int32 SecCount = Star.Sectors.Num();
            for (int32 i = 0; i < NbrCount; ++i)
            {
                const int32 Nbr = Star.Neighbors[i];
                if (SelectedNbrSet.Contains(Nbr))
                {
                    continue;
                }

                TArray<FEdgeMeetCandidate> Candidates;
                const FFaceSector* PrevSec = (i > 0) ? &Star.Sectors[i - 1] : (Star.bClosed ? &Star.Sectors[SecCount - 1] : nullptr);
                const FFaceSector* NextSec = (i < SecCount) ? &Star.Sectors[i] : (Star.bClosed ? &Star.Sectors[i % SecCount] : nullptr);
                const int32 PrevNbr = (i > 0) ? Star.Neighbors[i - 1] : (Star.bClosed ? Star.Neighbors[NbrCount - 1] : INDEX_NONE);
                const int32 NextNbr = (i + 1 < NbrCount) ? Star.Neighbors[i + 1] : (Star.bClosed ? Star.Neighbors[0] : INDEX_NONE);

                if (PrevNbr != INDEX_NONE && PrevSec && SelectedNbrSet.Contains(PrevNbr))
                {
                    if (const FFaceCorner* FC = Topo.CornersByFaceVertex.Find(MakeCornerKey(PrevSec->FaceIdx, VertexIdx)))
                    {
                        FEdgeMeetCandidate Cand;
                        if (IntersectSelectedWithUnselected(Positions, *FC, PrevNbr, Nbr, Width, Cand))
                        {
                            Candidates.Add(Cand);
                        }
                    }
                }
                if (NextNbr != INDEX_NONE && NextSec && SelectedNbrSet.Contains(NextNbr))
                {
                    if (const FFaceCorner* FC = Topo.CornersByFaceVertex.Find(MakeCornerKey(NextSec->FaceIdx, VertexIdx)))
                    {
                        FEdgeMeetCandidate Cand;
                        if (IntersectSelectedWithUnselected(Positions, *FC, NextNbr, Nbr, Width, Cand))
                        {
                            Candidates.Add(Cand);
                        }
                    }
                }

                const FVector EdgeVec = Positions[Nbr] - VertPos;
                const double EdgeLen = EdgeVec.Size();
                if (EdgeLen <= BEVEL_EPSILON)
                {
                    Res.Error = TEXT("Degenerate neighboring edge at beveled vertex.");
                    return Res;
                }
                const FVector Ray = EdgeVec / EdgeLen;

                double Distance = Width;
                double UsageDistance = Width;
                FVector Pos = VertPos + Ray * Distance;

                if (Candidates.Num() == 1)
                {
                    const FEdgeMeetCandidate& C = Candidates[0];
                    Distance = C.Distance;
                    Pos = C.Position;
                    UsageDistance = C.bOnUnselectedEdge ? FMath::Max(0.0, C.RawDistance) : 0.0;
                }
                else if (Candidates.Num() >= 2)
                {
                    TArray<FEdgeMeetCandidate> ValidCands;
                    for (const FEdgeMeetCandidate& C : Candidates)
                    {
                        if (C.bValidMeet) ValidCands.Add(C);
                    }
                    if (ValidCands.Num() == 1)
                    {
                        const FEdgeMeetCandidate& C = ValidCands[0];
                        Distance = C.Distance;
                        Pos = C.Position;
                        UsageDistance = C.bOnUnselectedEdge ? FMath::Max(0.0, C.RawDistance) : 0.0;
                    }
                    else if (ValidCands.Num() >= 2)
                    {
                        int32 OnEdgeCount = 0;
                        for (const FEdgeMeetCandidate& C : ValidCands)
                        {
                            if (C.bOnUnselectedEdge) ++OnEdgeCount;
                        }
                        if (OnEdgeCount == ValidCands.Num())
                        {
                            double SumD = 0.0, SumRaw = 0.0;
                            for (const FEdgeMeetCandidate& C : ValidCands)
                            {
                                SumD += C.Distance;
                                SumRaw += FMath::Max(0.0, C.RawDistance);
                            }
                            Distance = SumD / static_cast<double>(ValidCands.Num());
                            Pos = VertPos + Ray * Distance;
                            UsageDistance = SumRaw / static_cast<double>(ValidCands.Num());
                        }
                        else
                        {
                            FVector SumP = FVector::ZeroVector;
                            double SumRaw = 0.0;
                            for (const FEdgeMeetCandidate& C : ValidCands)
                            {
                                SumP += C.Position;
                                if (C.bOnUnselectedEdge) SumRaw += FMath::Max(0.0, C.RawDistance);
                            }
                            Pos = SumP / static_cast<double>(ValidCands.Num());
                            Distance = FMath::Max(0.0, FVector::DotProduct(Pos - VertPos, Ray));
                            UsageDistance = (OnEdgeCount == 0) ? 0.0 : (SumRaw / static_cast<double>(OnEdgeCount));
                        }
                    }
                }

                const FString Key = KeyForUnselected(VertexIdx, Nbr);
                FGeneratedPoint GP;
                GP.Key = Key;
                GP.SourceVertexIdx = VertexIdx;
                GP.Position = Pos;
                GP.bHasEdgeInset = true;
                GP.InsetEdgeKey = MakeEdgeKey(VertexIdx, Nbr);
                GP.InsetEndpointIdx = VertexIdx;
                GP.InsetDistance = UsageDistance;
                Res.GeneratedByKey.Add(Key, GP);
            }

            // Faces between two selected edges need their own offset-line intersection.
            for (const FFaceSector& Sec : Star.Sectors)
            {
                if (!SelectedNbrSet.Contains(Sec.NeighborA) || !SelectedNbrSet.Contains(Sec.NeighborB))
                {
                    continue;
                }
                const FFaceCorner* FC = Topo.CornersByFaceVertex.Find(MakeCornerKey(Sec.FaceIdx, VertexIdx));
                if (!FC)
                {
                    Res.Error = TEXT("Bevel could not resolve selected-pair face corner.");
                    return Res;
                }
                FVector IntersectPos;
                if (!IntersectSelectedPair(Positions, *FC, Width, IntersectPos))
                {
                    Res.Error = TEXT("Bevel selected edges form a backtracking or degenerate face corner.");
                    return Res;
                }
                const FString Key = KeyForSelectedPair(VertexIdx, Sec.FaceIdx);
                FGeneratedPoint GP;
                GP.Key = Key;
                GP.SourceVertexIdx = VertexIdx;
                GP.Position = IntersectPos;
                Res.GeneratedByKey.Add(Key, GP);
            }
        }

        // Local per-source-vertex welding (topology-safe; never welds across separate shells).
        TArray<FString> LocalKeys;
        for (const TPair<FString, FGeneratedPoint>& KV : Res.GeneratedByKey)
        {
            if (KV.Value.SourceVertexIdx == VertexIdx)
            {
                LocalKeys.Add(KV.Key);
            }
        }
        LocalKeys.Sort();

        double MinEdgeLen = TNumericLimits<double>::Max();
        for (const int32 Nbr : Star.Neighbors)
        {
            MinEdgeLen = FMath::Min(MinEdgeLen, FVector::Distance(Positions[Nbr], VertPos));
        }
        const double LocalScale = (MinEdgeLen < TNumericLimits<double>::Max()) ? FMath::Max(BEVEL_EPSILON, MinEdgeLen) : FMath::Max(Width, 1.0);
        const double WeldTol = FMath::Max(1e-7, FMath::Min(FMath::Max(Width, BEVEL_EPSILON) * 1e-5, LocalScale * 1e-7));

        for (int32 i = 0; i < LocalKeys.Num(); ++i)
        {
            const FString& K = LocalKeys[i];
            if (Res.CanonicalKeyByKey.Contains(K)) continue;
            Res.CanonicalKeyByKey.Add(K, K);
            const FVector& Pi = Res.GeneratedByKey[K].Position;
            for (int32 j = i + 1; j < LocalKeys.Num(); ++j)
            {
                const FString& OtherK = LocalKeys[j];
                if (Res.CanonicalKeyByKey.Contains(OtherK)) continue;
                if (FVector::Distance(Pi, Res.GeneratedByKey[OtherK].Position) <= WeldTol)
                {
                    Res.CanonicalKeyByKey.Add(OtherK, K);
                }
            }
        }

        // Build corner replacements & corner plan
        if (bTwoEdgeTerminal)
        {
            const FString LegKey = Res.Canonical(KeyForUnselected(VertexIdx, TermUnselectedNbr));
            TArray<FString> TerminalBoundary;
            for (const FFaceCorner& FC : VertCorners)
            {
                const FString SideKey = Res.Canonical(KeyForTerminalSide(VertexIdx, FC.FaceIdx));
                const bool bUnselPrev = (FC.PrevIdx == TermUnselectedNbr);
                const bool bSelNext = (FC.NextIdx == TermSelectedNbr);

                TArray<FString> Keys;
                if (bUnselPrev && bSelNext)
                {
                    Keys.Add(LegKey);
                    if (SideKey != LegKey) Keys.Add(SideKey);
                }
                else
                {
                    Keys.Add(SideKey);
                    if (LegKey != SideKey) Keys.Add(LegKey);
                }
                Res.Replacements.Add(MakeCornerKey(FC.FaceIdx, VertexIdx), MoveTemp(Keys));
                RailPointKeyByFaceVertex.Add(MakeCornerKey(FC.FaceIdx, VertexIdx), SideKey);
                TerminalBoundary.Add(SideKey);
            }

            TArray<FString> Boundary;
            if (TerminalBoundary.Num() >= 2)
            {
                Boundary = { TerminalBoundary[0], LegKey, TerminalBoundary[1] };
            }
            else
            {
                Boundary = TerminalBoundary;
                Boundary.Add(LegKey);
            }
            TArray<FString> UniqueBoundary = CompactUniqueKeys(Boundary);

            FCornerPlan CP;
            CP.SourceVertexIdx = VertexIdx;
            CP.SelectedEdgeKeys.Add(MakeEdgeKey(VertexIdx, TermSelectedNbr));
            CP.BoundaryPointKeys = UniqueBoundary;
            CP.BaseBoundaryPointKeys = UniqueBoundary;
            CP.bClosed = Star.bClosed;
            CP.MeshKind = (UniqueBoundary.Num() == 3) ? ECornerMeshKind::TriFan : ECornerMeshKind::None;
            if (UniqueBoundary.Num() == 3) CP.FanPointKey = LegKey;
            Res.Corners.Add(MoveTemp(CP));
        }
        else
        {
            for (const FFaceCorner& FC : VertCorners)
            {
                const bool bPrevSel = SelectedKeys.Contains(MakeEdgeKey(VertexIdx, FC.PrevIdx));
                const bool bNextSel = SelectedKeys.Contains(MakeEdgeKey(VertexIdx, FC.NextIdx));
                TArray<FString> Keys;
                FString RailKey;
                if (bPrevSel && bNextSel)
                {
                    RailKey = KeyForSelectedPair(VertexIdx, FC.FaceIdx);
                    Keys.Add(RailKey);
                }
                else if (bPrevSel)
                {
                    RailKey = KeyForUnselected(VertexIdx, FC.NextIdx);
                    Keys.Add(RailKey);
                }
                else if (bNextSel)
                {
                    RailKey = KeyForUnselected(VertexIdx, FC.PrevIdx);
                    Keys.Add(RailKey);
                }
                else
                {
                    Keys.Add(KeyForUnselected(VertexIdx, FC.PrevIdx));
                    Keys.Add(KeyForUnselected(VertexIdx, FC.NextIdx));
                }

                TArray<FString> Resolved;
                for (const FString& K : Keys)
                {
                    if (!Res.GeneratedByKey.Contains(K))
                    {
                        Res.Error = TEXT("Bevel failed to construct complete corner boundary.");
                        return Res;
                    }
                    const FString RK = Res.Canonical(K);
                    if (Resolved.IsEmpty() || Resolved.Last() != RK)
                    {
                        Resolved.Add(RK);
                    }
                }
                Res.Replacements.Add(MakeCornerKey(FC.FaceIdx, VertexIdx), MoveTemp(Resolved));
                if (!RailKey.IsEmpty())
                {
                    RailPointKeyByFaceVertex.Add(MakeCornerKey(FC.FaceIdx, VertexIdx), Res.Canonical(RailKey));
                }
            }

            TArray<FString> Boundary;
            auto PushBoundary = [&](const FString& RawKey)
            {
                const FString RK = Res.Canonical(RawKey);
                if (Boundary.IsEmpty() || Boundary.Last() != RK)
                {
                    Boundary.Add(RK);
                }
            };
            for (const FFaceSector& Sec : Star.Sectors)
            {
                const bool bASel = SelectedNbrSet.Contains(Sec.NeighborA);
                const bool bBSel = SelectedNbrSet.Contains(Sec.NeighborB);
                if (bASel && bBSel)
                {
                    PushBoundary(KeyForSelectedPair(VertexIdx, Sec.FaceIdx));
                }
                else if (bASel)
                {
                    PushBoundary(KeyForUnselected(VertexIdx, Sec.NeighborB));
                }
                else if (bBSel)
                {
                    PushBoundary(KeyForUnselected(VertexIdx, Sec.NeighborA));
                }
                else
                {
                    PushBoundary(KeyForUnselected(VertexIdx, Sec.NeighborA));
                    PushBoundary(KeyForUnselected(VertexIdx, Sec.NeighborB));
                }
            }
            if (Boundary.Num() > 1 && Boundary[0] == Boundary.Last())
            {
                Boundary.Pop();
            }
            TArray<FString> UniqueBoundary = CompactUniqueKeys(Boundary);

            FString TermFanKey;
            if (SelectedNbrSet.Num() == 1 && UniqueBoundary.Num() == 3)
            {
                const int32 SelNbr = *SelectedNbrSet.CreateConstIterator();
                TSet<FString> ProfileEndKeys;
                if (const TArray<FEdgeIncident>* Inc = Topo.EdgeFaces.Find(MakeEdgeKey(VertexIdx, SelNbr)))
                {
                    for (const FEdgeIncident& Item : *Inc)
                    {
                        if (const FString* RK = RailPointKeyByFaceVertex.Find(MakeCornerKey(Item.FaceIdx, VertexIdx)))
                        {
                            ProfileEndKeys.Add(Res.Canonical(*RK));
                        }
                    }
                }
                for (const FString& K : UniqueBoundary)
                {
                    if (!ProfileEndKeys.Contains(K))
                    {
                        TermFanKey = K;
                        break;
                    }
                }
            }

            const bool bTwoSelWeld = (SelectedNbrSet.Num() == 2 && UniqueBoundary.Num() == 2);
            if (bTwoSelWeld)
            {
                TwoSelectedProfileWeldVertices.Add(VertexIdx);
            }
            const bool bNoCap = (SelectedNbrSet.Num() == 1 && UniqueBoundary.Num() <= 2) || bTwoSelWeld;

            FCornerPlan CP;
            CP.SourceVertexIdx = VertexIdx;
            for (const int32 Nbr : Star.Neighbors)
            {
                if (SelectedNbrSet.Contains(Nbr))
                {
                    CP.SelectedEdgeKeys.Add(MakeEdgeKey(VertexIdx, Nbr));
                }
            }
            CP.BoundaryPointKeys = UniqueBoundary;
            CP.BaseBoundaryPointKeys = UniqueBoundary;
            CP.bClosed = Star.bClosed;
            if (bNoCap)
            {
                CP.MeshKind = ECornerMeshKind::None;
            }
            else if (!TermFanKey.IsEmpty())
            {
                CP.MeshKind = ECornerMeshKind::TriFan;
                CP.FanPointKey = TermFanKey;
            }
            else
            {
                CP.MeshKind = ECornerMeshKind::Poly;
            }
            Res.Corners.Add(MoveTemp(CP));
        }
    }

    // Build edge plans
    Res.EdgePlans.Reserve(SelectedEdges.Num());
    for (const FIntPoint& SelEdge : SelectedEdges)
    {
        const uint64 EKey = MakeEdgeKey(SelEdge.X, SelEdge.Y);
        const TArray<FEdgeIncident>* Inc = Topo.EdgeFaces.Find(EKey);
        if (!Inc || Inc->Num() != 2)
        {
            Res.Error = TEXT("Bevel requires manifold edges with 2 incident faces.");
            return Res;
        }
        const FEdgeIncident* Fwd = nullptr;
        const FEdgeIncident* Rev = nullptr;
        for (const FEdgeIncident& Item : *Inc)
        {
            if (Item.FromIdx == SelEdge.X && Item.ToIdx == SelEdge.Y) Fwd = &Item;
            else if (Item.FromIdx == SelEdge.Y && Item.ToIdx == SelEdge.X) Rev = &Item;
        }
        if (!Fwd || !Rev)
        {
            Res.Error = TEXT("Bevel requires consistently wound incident faces.");
            return Res;
        }

        const FString* AFwd = RailPointKeyByFaceVertex.Find(MakeCornerKey(Fwd->FaceIdx, SelEdge.X));
        const FString* ARev = RailPointKeyByFaceVertex.Find(MakeCornerKey(Rev->FaceIdx, SelEdge.X));
        const FString* BFwd = RailPointKeyByFaceVertex.Find(MakeCornerKey(Fwd->FaceIdx, SelEdge.Y));
        const FString* BRev = RailPointKeyByFaceVertex.Find(MakeCornerKey(Rev->FaceIdx, SelEdge.Y));
        if (!AFwd || !ARev || !BFwd || !BRev)
        {
            Res.Error = TEXT("Bevel selected edge failed to resolve rail endpoints.");
            return Res;
        }

        FEdgePlan EP;
        EP.EdgeKey = EKey;
        EP.PointA = SelEdge.X;
        EP.PointB = SelEdge.Y;
        EP.ForwardFaceIdx = Fwd->FaceIdx;
        EP.ReverseFaceIdx = Rev->FaceIdx;
        EP.BevelPointKeys[0] = *AFwd;
        EP.BevelPointKeys[1] = *ARev;
        EP.BevelPointKeys[2] = *BRev;
        EP.BevelPointKeys[3] = *BFwd;
        EP.ProfileA = { *AFwd, *ARev };
        EP.ProfileB = { *BFwd, *BRev };
        Res.EdgePlans.Add(MoveTemp(EP));
    }

    // Multi-segment superellipse profile sampling & M_ADJ corner grids
    if (Segments > 1)
    {
        const TArray<FVector2D> ProfileSamples = BuildEvenProfileSamples(Segments, Profile);

        auto InsertProfileArcIntoBoundary = [](const TArray<FString>& Boundary, const TArray<FString>& ProfileKeys, TArray<FString>& OutExpanded) -> bool
        {
            if (ProfileKeys.Num() <= 2)
            {
                OutExpanded = Boundary;
                return true;
            }
            const FString& Start = ProfileKeys[0];
            const FString& End = ProfileKeys.Last();
            if (Start == End) return false;

            OutExpanded.Reset();
            bool bInserted = false;
            const int32 N = Boundary.Num();
            for (int32 i = 0; i < N; ++i)
            {
                const FString& Cur = Boundary[i];
                const FString& Nxt = Boundary[(i + 1) % N];
                OutExpanded.Add(Cur);
                if (!bInserted && Cur == Start && Nxt == End)
                {
                    for (int32 s = 1; s < ProfileKeys.Num() - 1; ++s) OutExpanded.Add(ProfileKeys[s]);
                    bInserted = true;
                }
                else if (!bInserted && Cur == End && Nxt == Start)
                {
                    for (int32 s = ProfileKeys.Num() - 2; s >= 1; --s) OutExpanded.Add(ProfileKeys[s]);
                    bInserted = true;
                }
            }
            return bInserted;
        };

        auto InsertProfileArcIntoReplacement = [](const TArray<FString>& Repl, const TArray<FString>& ProfileKeys, TArray<FString>& OutExpanded) -> bool
        {
            if (ProfileKeys.Num() <= 2) return false;
            const FString& Start = ProfileKeys[0];
            const FString& End = ProfileKeys.Last();
            for (int32 i = 0; i + 1 < Repl.Num(); ++i)
            {
                if (Repl[i] == Start && Repl[i + 1] == End)
                {
                    OutExpanded.Reset();
                    for (int32 k = 0; k <= i; ++k) OutExpanded.Add(Repl[k]);
                    for (int32 s = 1; s < ProfileKeys.Num() - 1; ++s) OutExpanded.Add(ProfileKeys[s]);
                    for (int32 k = i + 1; k < Repl.Num(); ++k) OutExpanded.Add(Repl[k]);
                    return true;
                }
                if (Repl[i] == End && Repl[i + 1] == Start)
                {
                    OutExpanded.Reset();
                    for (int32 k = 0; k <= i; ++k) OutExpanded.Add(Repl[k]);
                    for (int32 s = ProfileKeys.Num() - 2; s >= 1; --s) OutExpanded.Add(ProfileKeys[s]);
                    for (int32 k = i + 1; k < Repl.Num(); ++k) OutExpanded.Add(Repl[k]);
                    return true;
                }
            }
            return false;
        };

        auto AttachProfileArcAtEndpoint = [&](int32 EndpointIdx, const TArray<FString>& ProfileKeys) -> bool
        {
            if (TwoSelectedProfileWeldVertices.Contains(EndpointIdx))
            {
                return true;
            }
            FCornerPlan* Corner = Res.Corners.FindByPredicate([EndpointIdx](const FCornerPlan& C) { return C.SourceVertexIdx == EndpointIdx; });
            if (!Corner) return false;

            if (Corner->MeshKind != ECornerMeshKind::None)
            {
                TArray<FString> Expanded;
                if (!InsertProfileArcIntoBoundary(Corner->BoundaryPointKeys, ProfileKeys, Expanded))
                {
                    return false;
                }
                Corner->BoundaryPointKeys = MoveTemp(Expanded);
                return true;
            }

            if (const TArray<FFaceCorner>* EndCorners = Topo.CornersByVertex.Find(EndpointIdx))
            {
                for (const FFaceCorner& EC : *EndCorners)
                {
                    const uint64 RKey = MakeCornerKey(EC.FaceIdx, EndpointIdx);
                    if (TArray<FString>* Repl = Res.Replacements.Find(RKey))
                    {
                        TArray<FString> Expanded;
                        if (InsertProfileArcIntoReplacement(*Repl, ProfileKeys, Expanded))
                        {
                            *Repl = MoveTemp(Expanded);
                            return true;
                        }
                    }
                }
            }
            return true;
        };

        for (int32 EdgeIdx = 0; EdgeIdx < Res.EdgePlans.Num(); ++EdgeIdx)
        {
            FEdgePlan& EP = Res.EdgePlans[EdgeIdx];
            const FVector& EdgeStart = Positions[EP.PointA];
            const FVector& EdgeEnd = Positions[EP.PointB];
            const FVector EdgeVec = EdgeEnd - EdgeStart;
            const double EdgeLen = EdgeVec.Size();
            if (EdgeLen <= BEVEL_EPSILON)
            {
                Res.Error = TEXT("Bevel encountered zero-length selected edge.");
                return Res;
            }
            const FVector EdgeDir = EdgeVec / EdgeLen;

            auto BuildEndpointProfile = [&](int32 EndpointIdx, const FString& StartKey, const FString& EndKey, int32 EndSlot, TArray<FString>& OutKeys) -> bool
            {
                const FString RStart = Res.Canonical(StartKey);
                const FString REnd = Res.Canonical(EndKey);
                if (RStart == REnd) return false;
                const FGeneratedPoint* PStart = Res.GeneratedByKey.Find(RStart);
                const FGeneratedPoint* PEnd = Res.GeneratedByKey.Find(REnd);
                if (!PStart || !PEnd) return false;
                if (FVector::Distance(PStart->Position, PEnd->Position) <= BEVEL_EPSILON) return false;

                const FVector ChordDir = PEnd->Position - PStart->Position;
                FVector Middle;
                if (!ClosestPointBetweenLines(EdgeStart, EdgeDir, PStart->Position, ChordDir, Middle))
                {
                    Middle = 0.5 * (PStart->Position + PEnd->Position);
                }

                OutKeys.Reset();
                OutKeys.Reserve(Segments + 1);
                OutKeys.Add(RStart);
                for (int32 Seg = 1; Seg < Segments; ++Seg)
                {
                    FVector ProfPos = MapProfileSample(PStart->Position, Middle, PEnd->Position, ProfileSamples[Seg]);
                    if (TerminalProfileProjectionVertices.Contains(EndpointIdx))
                    {
                        const FVector& Src = Positions[EndpointIdx];
                        const FVector PlaneRaw = FVector::CrossProduct(Src - PStart->Position, Src - PEnd->Position);
                        const double PlaneLen = PlaneRaw.Size();
                        if (FMath::IsFinite(PlaneLen) && PlaneLen > 1e-8)
                        {
                            ProfPos = ProjectAlongDirectionToPlane(ProfPos, EdgeDir, PStart->Position, PlaneRaw / PlaneLen);
                        }
                    }
                    const FString Key = FString::Printf(TEXT("p:%d:%d:%d"), EdgeIdx, EndSlot, Seg);
                    FGeneratedPoint GP;
                    GP.Key = Key;
                    GP.SourceVertexIdx = EndpointIdx;
                    GP.Position = ProfPos;
                    Res.GeneratedByKey.Add(Key, GP);
                    OutKeys.Add(Key);
                }
                OutKeys.Add(REnd);
                return true;
            };

            TArray<FString> ProfA, ProfB;
            if (!BuildEndpointProfile(EP.PointA, EP.BevelPointKeys[0], EP.BevelPointKeys[1], 0, ProfA) ||
                !BuildEndpointProfile(EP.PointB, EP.BevelPointKeys[3], EP.BevelPointKeys[2], 1, ProfB))
            {
                Res.bCollisionError = true;
                Res.Error = TEXT("Multi-segment bevel encountered collapsed endpoint profile.");
                return Res;
            }
            EP.ProfileA = MoveTemp(ProfA);
            EP.ProfileB = MoveTemp(ProfB);

            if (!AttachProfileArcAtEndpoint(EP.PointA, EP.ProfileA) ||
                !AttachProfileArcAtEndpoint(EP.PointB, EP.ProfileB))
            {
                Res.bCollisionError = true;
                Res.Error = TEXT("Multi-segment bevel could not attach profile arc to endpoint boundary.");
                return Res;
            }
        }

        // Weld segmented profile rows at two-selected-edge weld vertices
        for (const int32 VertexIdx : TwoSelectedProfileWeldVertices)
        {
            TArray<FEdgePlan*> Incident;
            for (FEdgePlan& EP : Res.EdgePlans)
            {
                if (EP.PointA == VertexIdx || EP.PointB == VertexIdx)
                {
                    Incident.Add(&EP);
                }
            }
            if (Incident.Num() != 2) continue;
            const TArray<FString>& RowA = (Incident[0]->PointA == VertexIdx) ? Incident[0]->ProfileA : Incident[0]->ProfileB;
            const TArray<FString>& RowB = (Incident[1]->PointA == VertexIdx) ? Incident[1]->ProfileA : Incident[1]->ProfileB;
            if (RowA.Num() != Segments + 1 || RowB.Num() != Segments + 1) continue;

            auto RowDist = [&](bool bRevB) -> double
            {
                double Sum = 0.0;
                for (int32 i = 0; i < RowA.Num(); ++i)
                {
                    const int32 BIdx = bRevB ? (RowB.Num() - 1 - i) : i;
                    const FGeneratedPoint* PA = Res.GeneratedByKey.Find(Res.Canonical(RowA[i]));
                    const FGeneratedPoint* PB = Res.GeneratedByKey.Find(Res.Canonical(RowB[BIdx]));
                    if (PA && PB) Sum += FVector::Distance(PA->Position, PB->Position);
                }
                return Sum;
            };

            const bool bRevB = RowDist(true) + BEVEL_EPSILON < RowDist(false);
            for (int32 i = 0; i < RowA.Num(); ++i)
            {
                const int32 BIdx = bRevB ? (RowB.Num() - 1 - i) : i;
                const FString RootA = Res.Canonical(RowA[i]);
                const FString RootB = Res.Canonical(RowB[BIdx]);
                if (RootA == RootB) continue;
                FGeneratedPoint* PA = Res.GeneratedByKey.Find(RootA);
                FGeneratedPoint* PB = Res.GeneratedByKey.Find(RootB);
                if (PA && PB)
                {
                    PA->Position = 0.5 * (PA->Position + PB->Position);
                    Res.CanonicalKeyByKey.Add(RootB, RootA);
                }
            }
        }

        // Build M_ADJ structured corner quad grids for closed segmented corners
        const double Exp = ProfileSuperellipseExponent(Profile);
        const double HalfSample = FMath::Pow(UE_SQRT_2 * 0.5, 2.0 / Exp);
        const double ProfileFullness = FMath::Clamp(2.0 * HalfSample - 1.0, -0.75, 0.9);
        const int32 Half = Segments / 2;
        const int32 Odd = Segments % 2;

        auto AdjCanon = [Segments, Half, Odd](int32 Sector, int32 Ring, int32 SegIdx, int32 SectorCount) -> FIntVector
        {
            const int32 i = ((Sector % SectorCount) + SectorCount) % SectorCount;
            if (!Odd && Ring == Half && SegIdx == Half) return FIntVector(0, Half, Half);
            if (Ring <= Half - 1 + Odd && SegIdx <= Half) return FIntVector(i, Ring, SegIdx);
            if (SegIdx <= Half)
            {
                return FIntVector((i + SectorCount - 1) % SectorCount, SegIdx, Segments - Ring);
            }
            return FIntVector((i + 1) % SectorCount, Segments - SegIdx, Ring);
        };

        auto FindReplacementOwningPair = [&](int32 VertexIdx, const FString& A, const FString& B, uint64& OutReplKey) -> bool
        {
            if (const TArray<FFaceCorner>* EndCorners = Topo.CornersByVertex.Find(VertexIdx))
            {
                for (const FFaceCorner& EC : *EndCorners)
                {
                    const uint64 RKey = MakeCornerKey(EC.FaceIdx, VertexIdx);
                    if (const TArray<FString>* Repl = Res.Replacements.Find(RKey))
                    {
                        for (int32 i = 0; i + 1 < Repl->Num(); ++i)
                        {
                            if (((*Repl)[i] == A && (*Repl)[i + 1] == B) ||
                                ((*Repl)[i] == B && (*Repl)[i + 1] == A))
                            {
                                OutReplKey = RKey;
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        };

        for (FCornerPlan& Corner : Res.Corners)
        {
            if (Corner.MeshKind != ECornerMeshKind::Poly ||
                Corner.SelectedEdgeKeys.Num() < 2 ||
                !Corner.bClosed ||
                Corner.BaseBoundaryPointKeys.Num() < 3)
            {
                continue;
            }

            TArray<FString> Base;
            Base.Reserve(Corner.BaseBoundaryPointKeys.Num());
            TSet<FString> BaseUnique;
            for (const FString& K : Corner.BaseBoundaryPointKeys)
            {
                const FString CK = Res.Canonical(K);
                Base.Add(CK);
                BaseUnique.Add(CK);
            }
            const int32 SectorCount = Base.Num();
            if (BaseUnique.Num() != SectorCount) continue;

            auto PairId = [](const FString& A, const FString& B) -> FString
            {
                return (A < B) ? (A + TEXT("|") + B) : (B + TEXT("|") + A);
            };

            TMap<FString, TArray<FString>> RowsByPair;
            for (const FEdgePlan& EP : Res.EdgePlans)
            {
                if (EP.PointA != Corner.SourceVertexIdx && EP.PointB != Corner.SourceVertexIdx) continue;
                const TArray<FString>& RawRow = (EP.PointA == Corner.SourceVertexIdx) ? EP.ProfileA : EP.ProfileB;
                if (RawRow.Num() != Segments + 1) continue;
                TArray<FString> Row;
                Row.Reserve(RawRow.Num());
                for (const FString& K : RawRow) Row.Add(Res.Canonical(K));

                const FString& Start = Row[0];
                const FString& End = Row.Last();
                const int32 StartIdx = Base.IndexOfByKey(Start);
                if (StartIdx != INDEX_NONE && Base[(StartIdx + 1) % SectorCount] == End)
                {
                    RowsByPair.Add(PairId(Start, End), Row);
                    continue;
                }
                const int32 EndIdx = Base.IndexOfByKey(End);
                if (EndIdx != INDEX_NONE && Base[(EndIdx + 1) % SectorCount] == Start)
                {
                    Algo::Reverse(Row);
                    RowsByPair.Add(PairId(Start, End), Row);
                }
            }

            TArray<TArray<FString>> SectorRows;
            struct FSyntheticRow { uint64 ReplKey; TArray<FString> Row; };
            TArray<FSyntheticRow> SyntheticRows;
            bool bCanBuildAdj = true;

            for (int32 Sector = 0; Sector < SectorCount; ++Sector)
            {
                const FString& Start = Base[Sector];
                const FString& End = Base[(Sector + 1) % SectorCount];
                if (const TArray<FString>* Existing = RowsByPair.Find(PairId(Start, End)))
                {
                    TArray<FString> Oriented = *Existing;
                    if (Oriented[0] != Start) Algo::Reverse(Oriented);
                    SectorRows.Add(MoveTemp(Oriented));
                    continue;
                }

                uint64 ReplKey = 0;
                if (!FindReplacementOwningPair(Corner.SourceVertexIdx, Start, End, ReplKey))
                {
                    bCanBuildAdj = false;
                    break;
                }
                const FGeneratedPoint* PStart = Res.GeneratedByKey.Find(Start);
                const FGeneratedPoint* PEnd = Res.GeneratedByKey.Find(End);
                if (!PStart || !PEnd)
                {
                    bCanBuildAdj = false;
                    break;
                }
                TArray<FString> Row;
                Row.Reserve(Segments + 1);
                Row.Add(Start);
                for (int32 s = 1; s < Segments; ++s)
                {
                    const FString Key = FString::Printf(TEXT("adj-b:%d:%d:%d"), Corner.SourceVertexIdx, Sector, s);
                    FGeneratedPoint GP;
                    GP.Key = Key;
                    GP.SourceVertexIdx = Corner.SourceVertexIdx;
                    GP.Position = FMath::Lerp(PStart->Position, PEnd->Position, static_cast<double>(s) / static_cast<double>(Segments));
                    Res.GeneratedByKey.Add(Key, GP);
                    Row.Add(Key);
                }
                Row.Add(End);
                SectorRows.Add(Row);
                SyntheticRows.Add({ ReplKey, MoveTemp(Row) });
            }
            if (!bCanBuildAdj || SectorRows.Num() != SectorCount) continue;

            TMap<uint64, TArray<FString>> PendingReplUpdates;
            for (const FSyntheticRow& SR : SyntheticRows)
            {
                const TArray<FString>* BaseRepl = PendingReplUpdates.Find(SR.ReplKey);
                if (!BaseRepl) BaseRepl = Res.Replacements.Find(SR.ReplKey);
                if (!BaseRepl) { bCanBuildAdj = false; break; }
                TArray<FString> Expanded;
                if (!InsertProfileArcIntoReplacement(*BaseRepl, SR.Row, Expanded))
                {
                    bCanBuildAdj = false;
                    break;
                }
                PendingReplUpdates.Add(SR.ReplKey, MoveTemp(Expanded));
            }
            if (!bCanBuildAdj) continue;

            TMap<FIntVector, FString> KeyByTuple;
            auto SetTupleKey = [&](int32 Sector, int32 Ring, int32 SegIdx, const FString& Key)
            {
                const FIntVector T = AdjCanon(Sector, Ring, SegIdx, SectorCount);
                if (const FString* Prev = KeyByTuple.Find(T))
                {
                    if (*Prev != Key)
                    {
                        const FGeneratedPoint* PA = Res.GeneratedByKey.Find(*Prev);
                        const FGeneratedPoint* PB = Res.GeneratedByKey.Find(Key);
                        if (!PA || !PB || FVector::Distance(PA->Position, PB->Position) > FMath::Max(1e-6, Width * 1e-4))
                        {
                            bCanBuildAdj = false;
                            return;
                        }
                        Res.CanonicalKeyByKey.Add(Key, *Prev);
                    }
                }
                else
                {
                    KeyByTuple.Add(T, Key);
                }
            };

            for (int32 Sector = 0; Sector < SectorCount && bCanBuildAdj; ++Sector)
            {
                for (int32 k = 0; k <= Segments; ++k)
                {
                    SetTupleKey(Sector, 0, k, SectorRows[Sector][k]);
                }
            }
            if (!bCanBuildAdj) continue;

            auto GetTupleKey = [&](int32 Sector, int32 Ring, int32 SegIdx) -> FString
            {
                const FIntVector T = AdjCanon(Sector, Ring, SegIdx, SectorCount);
                if (const FString* K = KeyByTuple.Find(T))
                {
                    return Res.Canonical(*K);
                }
                return FString();
            };

            auto GetTuplePos = [&](int32 Sector, int32 Ring, int32 SegIdx, FVector& OutPos) -> bool
            {
                const FString K = GetTupleKey(Sector, Ring, SegIdx);
                if (const FGeneratedPoint* GP = Res.GeneratedByKey.Find(K))
                {
                    OutPos = GP->Position;
                    return true;
                }
                return false;
            };

            FVector Centroid = FVector::ZeroVector;
            for (const FString& BK : Base)
            {
                if (const FGeneratedPoint* GP = Res.GeneratedByKey.Find(BK))
                {
                    Centroid += GP->Position;
                }
            }
            Centroid /= static_cast<double>(SectorCount);
            const FVector CenterTarget = FMath::Lerp(Centroid, Positions[Corner.SourceVertexIdx], ProfileFullness);

            if (Odd)
            {
                const double Pull = static_cast<double>(Half) / static_cast<double>(Half + 1);
                for (int32 Sector = 0; Sector < SectorCount; ++Sector)
                {
                    FVector Horiz, Vert;
                    if (!GetTuplePos(Sector, 0, Half, Horiz) || !GetTuplePos(Sector, Half, 0, Vert))
                    {
                        bCanBuildAdj = false;
                        break;
                    }
                    const FVector Anchor = 0.5 * (Horiz + Vert);
                    const FString Key = FString::Printf(TEXT("adj-c:%d:%d"), Corner.SourceVertexIdx, Sector);
                    FGeneratedPoint GP;
                    GP.Key = Key;
                    GP.SourceVertexIdx = Corner.SourceVertexIdx;
                    GP.Position = FMath::Lerp(Anchor, CenterTarget, FMath::Max(0.5, Pull));
                    Res.GeneratedByKey.Add(Key, GP);
                    SetTupleKey(Sector, Half, Half, Key);
                }
            }
            else
            {
                const FString Key = FString::Printf(TEXT("adj-c:%d:s"), Corner.SourceVertexIdx);
                FGeneratedPoint GP;
                GP.Key = Key;
                GP.SourceVertexIdx = Corner.SourceVertexIdx;
                GP.Position = CenterTarget;
                Res.GeneratedByKey.Add(Key, GP);
                SetTupleKey(0, Half, Half, Key);
            }
            if (!bCanBuildAdj) continue;

            // Coons patch interpolation for interior canonical stations
            for (int32 Sector = 0; Sector < SectorCount && bCanBuildAdj; ++Sector)
            {
                for (int32 Ring = 0; Ring <= Half && bCanBuildAdj; ++Ring)
                {
                    for (int32 k = 0; k <= Half && bCanBuildAdj; ++k)
                    {
                        const bool bIsCanon = Odd
                            ? (Ring <= Half && k <= Half)
                            : ((Ring < Half && k <= Half) || (Ring == Half && k == Half && Sector == 0));
                        if (!bIsCanon) continue;
                        const FIntVector Tuple(Sector, Ring, k);
                        if (KeyByTuple.Contains(Tuple)) continue;

                        const double U = (Half == 0) ? 0.0 : (static_cast<double>(k) / static_cast<double>(Half));
                        const double V = (Half == 0) ? 0.0 : (static_cast<double>(Ring) / static_cast<double>(Half));
                        FVector Q00, Q10, Q01, Q11, Bottom, Left;
                        if (!GetTuplePos(Sector, 0, 0, Q00) ||
                            !GetTuplePos(Sector, 0, Half, Q10) ||
                            !GetTuplePos(Sector, Half, 0, Q01) ||
                            !GetTuplePos(Odd ? Sector : 0, Half, Half, Q11) ||
                            !GetTuplePos(Sector, 0, k, Bottom) ||
                            !GetTuplePos(Sector, Ring, 0, Left))
                        {
                            bCanBuildAdj = false;
                            break;
                        }
                        const FVector Top = FMath::Lerp(Q01, Q11, U);
                        const FVector Right = FMath::Lerp(Q10, Q11, V);
                        const FVector Bilinear = Q00 * ((1.0 - U) * (1.0 - V)) + Q10 * (U * (1.0 - V))
                                               + Q01 * ((1.0 - U) * V) + Q11 * (U * V);
                        const FVector Coons = (Bottom * (1.0 - V) + Top * V) + (Left * (1.0 - U) + Right * U) - Bilinear;

                        const FString Key = FString::Printf(TEXT("adj-i:%d:%d:%d:%d"), Corner.SourceVertexIdx, Sector, Ring, k);
                        FGeneratedPoint GP;
                        GP.Key = Key;
                        GP.SourceVertexIdx = Corner.SourceVertexIdx;
                        GP.Position = Coons;
                        Res.GeneratedByKey.Add(Key, GP);
                        KeyByTuple.Add(Tuple, Key);
                    }
                }
            }
            if (!bCanBuildAdj) continue;

            // Emit ADJ cells strictly ring-by-ring from outer ring (Ring=0) inward so winding propagation always hits a shared edge!
            TArray<TArray<FString>> AdjFaces;
            for (int32 Ring = 0; Ring < Half && bCanBuildAdj; ++Ring)
            {
                for (int32 Sector = 0; Sector < SectorCount && bCanBuildAdj; ++Sector)
                {
                    for (int32 k = 0; k < Half && bCanBuildAdj; ++k)
                    {
                        const FString K00 = GetTupleKey(Sector, Ring, k);
                        const FString K10 = GetTupleKey(Sector, Ring, k + 1);
                        const FString K11 = GetTupleKey(Sector, Ring + 1, k + 1);
                        const FString K01 = GetTupleKey(Sector, Ring + 1, k);
                        if (K00.IsEmpty() || K10.IsEmpty() || K11.IsEmpty() || K01.IsEmpty())
                        {
                            bCanBuildAdj = false;
                            break;
                        }
                        TArray<FString> Cell = CompactUniqueKeys({ K00, K10, K11, K01 });
                        if (Cell.Num() >= 3)
                        {
                            AdjFaces.Add(MoveTemp(Cell));
                        }
                    }
                }
            }
            if (bCanBuildAdj && Odd)
            {
                TArray<FString> CenterPoly;
                for (int32 Sector = 0; Sector < SectorCount; ++Sector)
                {
                    const FString CK = GetTupleKey(Sector, Half, Half);
                    if (CK.IsEmpty()) { bCanBuildAdj = false; break; }
                    CenterPoly.Add(CK);
                }
                CenterPoly = CompactUniqueKeys(CenterPoly);
                if (CenterPoly.Num() >= 3)
                {
                    AdjFaces.Add(MoveTemp(CenterPoly));
                }
            }

            if (bCanBuildAdj && !AdjFaces.IsEmpty())
            {
                for (const TPair<uint64, TArray<FString>>& KV : PendingReplUpdates)
                {
                    Res.Replacements.Add(KV.Key, KV.Value);
                }
                Corner.MeshKind = ECornerMeshKind::Adj;
                Corner.AdjFacePointKeys = MoveTemp(AdjFaces);
            }
        }
    }

    // Compute overlap scale (longitudinal unselected edge usage + lateral selected edge usage)
    TMap<uint64, TMap<int32, double>> EdgeUsage;
    for (const TPair<FString, FGeneratedPoint>& KV : Res.GeneratedByKey)
    {
        const FGeneratedPoint& GP = KV.Value;
        if (!GP.bHasEdgeInset) continue;
        TMap<int32, double>& ByEnd = EdgeUsage.FindOrAdd(GP.InsetEdgeKey);
        double& Cur = ByEnd.FindOrAdd(GP.InsetEndpointIdx);
        Cur = FMath::Max(Cur, GP.InsetDistance);
    }

    double OverlapScale = 1.0;
    for (const TPair<uint64, TMap<int32, double>>& KV : EdgeUsage)
    {
        const int32 V0 = static_cast<int32>(KV.Key >> 32);
        const int32 V1 = static_cast<int32>(KV.Key & 0xFFFFFFFFu);
        if (!Positions.IsValidIndex(V0) || !Positions.IsValidIndex(V1)) continue;
        const double EdgeLen = FVector::Distance(Positions[V0], Positions[V1]);
        double TotalUsed = 0.0;
        for (const TPair<int32, double>& EndKV : KV.Value)
        {
            TotalUsed += EndKV.Value;
        }
        if (TotalUsed > BEVEL_EPSILON && TotalUsed > EdgeLen * 0.98)
        {
            OverlapScale = FMath::Min(OverlapScale, (EdgeLen * 0.98) / TotalUsed);
        }
    }
    Res.OverlapScale = OverlapScale;

    // Validate all reconstructed original faces (no collapse, no inversion, no 2D self-intersection)
    for (int32 FaceIdx = 0; FaceIdx < Faces.Num(); ++FaceIdx)
    {
        const TArray<int32>& OrigPerim = Faces[FaceIdx];
        const FVector& FaceN = Topo.FaceNormals[FaceIdx];

        TArray<FVector2D> Orig2D;
        Orig2D.Reserve(OrigPerim.Num());
        for (const int32 V : OrigPerim) Orig2D.Add(ProjectDominant(Positions[V], FaceN));
        const double OrigArea = PolygonSignedArea2D(Orig2D);
        if (FMath::Abs(OrigArea) <= 1e-10) continue;

        TArray<FVector> Planned3D;
        bool bChanged = false;
        for (const int32 V : OrigPerim)
        {
            const TArray<FString>* Repl = Res.Replacements.Find(MakeCornerKey(FaceIdx, V));
            if (!Repl)
            {
                Planned3D.Add(Positions[V]);
                continue;
            }
            bChanged = true;
            for (const FString& RK : *Repl)
            {
                const FGeneratedPoint* GP = Res.GeneratedByKey.Find(Res.Canonical(RK));
                if (!GP)
                {
                    Res.Error = TEXT("Bevel face validation lost planned point.");
                    return Res;
                }
                Planned3D.Add(GP->Position);
            }
        }
        if (!bChanged) continue;

        // Compact near-coincident consecutive planned vertices
        TArray<FVector> Clean3D;
        Clean3D.Reserve(Planned3D.Num());
        for (const FVector& P : Planned3D)
        {
            if (Clean3D.IsEmpty() || FVector::Distance(Clean3D.Last(), P) > 1e-7)
            {
                Clean3D.Add(P);
            }
        }
        while (Clean3D.Num() >= 2 && FVector::Distance(Clean3D[0], Clean3D.Last()) <= 1e-7)
        {
            Clean3D.Pop();
        }
        if (Clean3D.Num() < 3)
        {
            Res.bCollisionError = true;
            Res.Error = FString::Printf(TEXT("Bevel would collapse face %d."), FaceIdx);
            return Res;
        }

        TArray<FVector2D> Planned2D;
        Planned2D.Reserve(Clean3D.Num());
        for (const FVector& P : Clean3D) Planned2D.Add(ProjectDominant(P, FaceN));
        const double PlannedArea = PolygonSignedArea2D(Planned2D);
        if (FMath::Abs(PlannedArea) <= 1e-9 || PlannedArea * OrigArea <= 0.0)
        {
            Res.bCollisionError = true;
            Res.Error = FString::Printf(TEXT("Bevel would invert face %d."), FaceIdx);
            return Res;
        }
        if (!PolygonIsSimple2D(Planned2D, 1e-7))
        {
            Res.bCollisionError = true;
            Res.Error = FString::Printf(TEXT("Bevel would self-intersect face %d."), FaceIdx);
            return Res;
        }
    }

    Res.bOk = true;
    return Res;
}

void CompactUnusedVertices(
    TArray<FVector>& InOutPositions,
    TArray<TArray<int32>>& InOutFaces,
    TArray<int32>& InOutCreatedVertices)
{
    TArray<int32> Remap;
    Remap.Init(INDEX_NONE, InOutPositions.Num());
    TArray<FVector> NewPositions;
    NewPositions.Reserve(InOutPositions.Num());

    for (TArray<int32>& Face : InOutFaces)
    {
        for (int32& V : Face)
        {
            if (Remap[V] == INDEX_NONE)
            {
                Remap[V] = NewPositions.Add(InOutPositions[V]);
            }
            V = Remap[V];
        }
    }

    TArray<int32> NewCreated;
    NewCreated.Reserve(InOutCreatedVertices.Num());
    for (const int32 OldV : InOutCreatedVertices)
    {
        if (Remap.IsValidIndex(OldV) && Remap[OldV] != INDEX_NONE)
        {
            NewCreated.AddUnique(Remap[OldV]);
        }
    }

    InOutPositions = MoveTemp(NewPositions);
    InOutCreatedVertices = MoveTemp(NewCreated);
}
} // namespace

namespace NGon::Bevel
{
FNGonBevelOutput BevelEdges(
    const TArray<FVector>& InPositions,
    const TArray<TArray<int32>>& InFaces,
    const TArray<FIntPoint>& SelectedEdges,
    const FNGonBevelOptions& Options)
{
    FNGonBevelOutput Out;
    const double RequestedWidth = FMath::Max(0.0, Options.Width);
    const int32 Segments = FMath::Clamp(Options.Segments, 1, 32);
    const double Profile = FMath::Clamp(Options.Profile, 0.05, 0.9999);
    Out.RequestedWidth = RequestedWidth;

    if (InPositions.IsEmpty() || InFaces.IsEmpty())
    {
        Out.Message = TEXT("Mesh is empty.");
        return Out;
    }
    if (SelectedEdges.IsEmpty())
    {
        Out.Message = TEXT("No edges selected to bevel.");
        return Out;
    }
    if (!FMath::IsFinite(RequestedWidth) || RequestedWidth <= 1e-6)
    {
        Out.Message = TEXT("Bevel width must be greater than 0.");
        return Out;
    }

    FTopologyIndex Topo;
    if (!BuildTopologyIndex(InPositions, InFaces, Topo, Out.Message))
    {
        return Out;
    }

    // Filter selected edges: deduplicate and check manifoldness (2 opposite-wound faces + valid vertex stars)
    TArray<FIntPoint> ValidEdges;
    TSet<uint64> SeenKeys;
    for (const FIntPoint& RawE : SelectedEdges)
    {
        if (!InPositions.IsValidIndex(RawE.X) || !InPositions.IsValidIndex(RawE.Y) || RawE.X == RawE.Y)
        {
            ++Out.SkippedEdges;
            continue;
        }
        const uint64 EKey = MakeEdgeKey(RawE.X, RawE.Y);
        if (SeenKeys.Contains(EKey)) continue;
        SeenKeys.Add(EKey);

        const TArray<FEdgeIncident>* Inc = Topo.EdgeFaces.Find(EKey);
        bool bEdgeManifold = false;
        if (Inc && Inc->Num() == 2)
        {
            const bool bOpp = ((*Inc)[0].FromIdx == (*Inc)[1].ToIdx && (*Inc)[0].ToIdx == (*Inc)[1].FromIdx);
            bEdgeManifold = bOpp;
        }
        if (!bEdgeManifold)
        {
            if (Options.bSkipNonManifoldEdges)
            {
                ++Out.SkippedEdges;
                continue;
            }
            Out.Message = TEXT("Bevel requires manifold edges shared by exactly two consistently wound faces.");
            return Out;
        }

        // Verify both endpoints have valid vertex stars
        FVertexStar StarA, StarB;
        FString StarErr;
        const TArray<FFaceCorner>* CornersA = Topo.CornersByVertex.Find(RawE.X);
        const TArray<FFaceCorner>* CornersB = Topo.CornersByVertex.Find(RawE.Y);
        if (!CornersA || !CornersB ||
            !BuildVertexStar(RawE.X, *CornersA, StarA, StarErr) ||
            !BuildVertexStar(RawE.Y, *CornersB, StarB, StarErr))
        {
            if (Options.bSkipNonManifoldEdges)
            {
                ++Out.SkippedEdges;
                continue;
            }
            Out.Message = StarErr;
            return Out;
        }

        ValidEdges.Add(FIntPoint(FMath::Min(RawE.X, RawE.Y), FMath::Max(RawE.X, RawE.Y)));
    }

    if (ValidEdges.IsEmpty())
    {
        Out.Message = TEXT("No valid manifold edges available to bevel (boundary or non-manifold edges were skipped).");
        return Out;
    }

    // Solve with optional overlap clamping + 32-step monotonic binary search on collision
    double AppliedWidth = RequestedWidth;
    FBevelSolveResult BestSolve;
    bool bHaveBest = false;

    if (!Options.bClampOverlap)
    {
        BestSolve = SolveEdgesAtWidth(InPositions, InFaces, Topo, ValidEdges, AppliedWidth, Segments, Profile);
        if (!BestSolve.bOk)
        {
            Out.Message = BestSolve.Error;
            return Out;
        }
        bHaveBest = true;
    }
    else
    {
        // Pass 1: Iterative edge-inset overlap scaling
        for (int32 Iter = 0; Iter < 16; ++Iter)
        {
            FBevelSolveResult Attempt = SolveEdgesAtWidth(InPositions, InFaces, Topo, ValidEdges, AppliedWidth, Segments, Profile);
            if (Attempt.bOk)
            {
                if (Attempt.OverlapScale < 0.999)
                {
                    AppliedWidth *= Attempt.OverlapScale;
                    Out.bWasClamped = true;
                    continue;
                }
                BestSolve = MoveTemp(Attempt);
                bHaveBest = true;
                break;
            }
            if (!Attempt.bCollisionError)
            {
                // Topology/unsupported error that shrinking won't fix
                Out.Message = Attempt.Error;
                return Out;
            }
            // Collision encountered: run 32-step binary search in [0, AppliedWidth]
            double Lo = 0.0;
            double Hi = AppliedWidth;
            for (int32 Step = 0; Step < 32; ++Step)
            {
                const double Mid = 0.5 * (Lo + Hi);
                if (Mid <= 1e-6)
                {
                    Hi = Mid;
                    continue;
                }
                FBevelSolveResult Probe = SolveEdgesAtWidth(InPositions, InFaces, Topo, ValidEdges, Mid, Segments, Profile);
                if (Probe.bOk && Probe.OverlapScale >= 0.999)
                {
                    Lo = Mid;
                    BestSolve = MoveTemp(Probe);
                    bHaveBest = true;
                }
                else
                {
                    Hi = Mid;
                }
            }
            if (bHaveBest)
            {
                AppliedWidth = Lo;
                Out.bWasClamped = true;
            }
            break;
        }
    }

    if (!bHaveBest || !BestSolve.bOk)
    {
        Out.Message = BestSolve.Error.IsEmpty()
            ? TEXT("Bevel could not find a non-overlapping width for the selected edges.")
            : BestSolve.Error;
        return Out;
    }

    // Materialize new positions and faces
    Out.Positions = InPositions;
    TMap<FString, int32> VertexIdByCanonicalKey;

    TArray<FString> SortedGenKeys;
    BestSolve.GeneratedByKey.GetKeys(SortedGenKeys);
    SortedGenKeys.Sort();

    for (const FString& RawKey : SortedGenKeys)
    {
        const FString CanonKey = BestSolve.Canonical(RawKey);
        if (!VertexIdByCanonicalKey.Contains(CanonKey))
        {
            const FGeneratedPoint& GP = BestSolve.GeneratedByKey[CanonKey];
            const int32 NewV = Out.Positions.Add(GP.Position);
            VertexIdByCanonicalKey.Add(CanonKey, NewV);
            Out.CreatedVertices.Add(NewV);
        }
    }

    auto ResolveKey = [&](const FString& Key) -> int32
    {
        const FString CK = BestSolve.Canonical(Key);
        if (const int32* Found = VertexIdByCanonicalKey.Find(CK))
        {
            return *Found;
        }
        return INDEX_NONE;
    };

    // 1. Reconstruct original faces
    Out.Faces.Reserve(InFaces.Num() + ValidEdges.Num() * Segments + BestSolve.Corners.Num() * 4);
    for (int32 FaceIdx = 0; FaceIdx < InFaces.Num(); ++FaceIdx)
    {
        const TArray<int32>& OrigPerim = InFaces[FaceIdx];
        TArray<int32> NextPerim;
        NextPerim.Reserve(OrigPerim.Num() + 4);
        for (const int32 V : OrigPerim)
        {
            if (const TArray<FString>* Repl = BestSolve.Replacements.Find(MakeCornerKey(FaceIdx, V)))
            {
                for (const FString& RK : *Repl)
                {
                    const int32 NewV = ResolveKey(RK);
                    if (NewV != INDEX_NONE) NextPerim.Add(NewV);
                }
            }
            else
            {
                NextPerim.Add(V);
            }
        }
        TArray<int32> Clean = CompactCyclicVertexIds(NextPerim);
        if (Clean.Num() < 3)
        {
            Out.Message = FString::Printf(TEXT("Bevel produced degenerate reconstructed face %d."), FaceIdx);
            return Out;
        }
        Out.Faces.Add(MoveTemp(Clean));
    }

    // 2. Emit bevel edge strips
    for (const FEdgePlan& EP : BestSolve.EdgePlans)
    {
        for (int32 Seg = 0; Seg < Segments; ++Seg)
        {
            const int32 V0 = ResolveKey(EP.ProfileA[Seg]);
            const int32 V1 = ResolveKey(EP.ProfileA[Seg + 1]);
            const int32 V2 = ResolveKey(EP.ProfileB[Seg + 1]);
            const int32 V3 = ResolveKey(EP.ProfileB[Seg]);
            if (V0 == INDEX_NONE || V1 == INDEX_NONE || V2 == INDEX_NONE || V3 == INDEX_NONE)
            {
                Out.Message = TEXT("Bevel failed to resolve strip vertex.");
                return Out;
            }
            TArray<int32> Strip = CompactCyclicVertexIds({ V0, V1, V2, V3 });
            TSet<int32> Uniq(Strip);
            if (Strip.Num() < 3 || Uniq.Num() != Strip.Num())
            {
                continue;
            }
            const int32 FaceIdx = Out.Faces.Add(MoveTemp(Strip));
            Out.BevelFaceIndices.Add(FaceIdx);
        }
    }

    // Helper: normalize corner/cell winding opposite to already-created adjacent faces, with vertex-normal fallback
    auto NormalizeCornerWinding = [&](TArray<int32>& Loop, const FVector& FallbackNormal)
    {
        bool bFoundShared = false;
        bool bShouldReverse = false;
        const int32 N = Loop.Num();
        for (int32 i = 0; i < N && !bFoundShared; ++i)
        {
            const int32 A = Loop[i];
            const int32 B = Loop[(i + 1) % N];
            for (const TArray<int32>& Existing : Out.Faces)
            {
                const int32 Rel = DirectedEdgeRelation(Existing, A, B);
                if (Rel != 0)
                {
                    bShouldReverse = (Rel == 1);
                    bFoundShared = true;
                    break;
                }
            }
        }
        if (!bFoundShared && !FallbackNormal.IsNearlyZero())
        {
            const FVector CellN = ComputeNewellNormal(Out.Positions, Loop);
            if (FVector::DotProduct(CellN, FallbackNormal) < 0.0)
            {
                bShouldReverse = true;
            }
        }
        if (bShouldReverse)
        {
            Algo::Reverse(Loop);
        }
    };

    // 3. Emit corner faces (POLY, TRI_FAN, or ring-ordered ADJ)
    for (const FCornerPlan& Corner : BestSolve.Corners)
    {
        if (Corner.MeshKind == ECornerMeshKind::None || Corner.BoundaryPointKeys.Num() < 3)
        {
            continue;
        }

        // Compute average incident face normal at source vertex for fallback orientation
        FVector AvgVertNormal = FVector::ZeroVector;
        if (const TArray<FFaceCorner>* IncCorners = Topo.CornersByVertex.Find(Corner.SourceVertexIdx))
        {
            for (const FFaceCorner& FC : *IncCorners) AvgVertNormal += FC.FaceNormal;
            AvgVertNormal = AvgVertNormal.GetSafeNormal();
        }

        if (Corner.MeshKind == ECornerMeshKind::Adj && !Corner.AdjFacePointKeys.IsEmpty())
        {
            for (const TArray<FString>& CellKeys : Corner.AdjFacePointKeys)
            {
                TArray<int32> CellVerts;
                CellVerts.Reserve(CellKeys.Num());
                for (const FString& K : CellKeys)
                {
                    const int32 V = ResolveKey(K);
                    if (V != INDEX_NONE) CellVerts.AddUnique(V);
                }
                if (CellVerts.Num() < 3) continue;
                NormalizeCornerWinding(CellVerts, AvgVertNormal);
                const int32 FaceIdx = Out.Faces.Add(MoveTemp(CellVerts));
                Out.CornerFaceIndices.Add(FaceIdx);
            }
        }
        else if (Corner.MeshKind == ECornerMeshKind::TriFan && !Corner.FanPointKey.IsEmpty())
        {
            TArray<int32> BoundVerts;
            for (const FString& K : Corner.BoundaryPointKeys)
            {
                const int32 V = ResolveKey(K);
                if (V != INDEX_NONE) BoundVerts.AddUnique(V);
            }
            if (BoundVerts.Num() < 3) continue;
            NormalizeCornerWinding(BoundVerts, AvgVertNormal);

            const int32 FanV = ResolveKey(Corner.FanPointKey);
            const int32 FanIdx = BoundVerts.IndexOfByKey(FanV);
            if (FanIdx == INDEX_NONE)
            {
                const int32 FaceIdx = Out.Faces.Add(MoveTemp(BoundVerts));
                Out.CornerFaceIndices.Add(FaceIdx);
            }
            else
            {
                TArray<int32> Rotated;
                Rotated.Reserve(BoundVerts.Num());
                for (int32 i = FanIdx; i < BoundVerts.Num(); ++i) Rotated.Add(BoundVerts[i]);
                for (int32 i = 0; i < FanIdx; ++i) Rotated.Add(BoundVerts[i]);
                for (int32 i = 1; i + 1 < Rotated.Num(); ++i)
                {
                    TArray<int32> Tri = { Rotated[0], Rotated[i], Rotated[i + 1] };
                    const int32 FaceIdx = Out.Faces.Add(MoveTemp(Tri));
                    Out.CornerFaceIndices.Add(FaceIdx);
                }
            }
        }
        else
        {
            TArray<int32> CapVerts;
            for (const FString& K : Corner.BoundaryPointKeys)
            {
                const int32 V = ResolveKey(K);
                if (V != INDEX_NONE) CapVerts.AddUnique(V);
            }
            if (CapVerts.Num() < 3) continue;
            NormalizeCornerWinding(CapVerts, AvgVertNormal);
            const int32 FaceIdx = Out.Faces.Add(MoveTemp(CapVerts));
            Out.CornerFaceIndices.Add(FaceIdx);
        }
    }

    CompactUnusedVertices(Out.Positions, Out.Faces, Out.CreatedVertices);

    Out.AppliedWidth = AppliedWidth;
    Out.bOk = true;
    Out.Message = FString::Printf(
        TEXT("Beveled %d edge(s) (width %.2f%s, %d seg, profile %.2f): +%d strip(s), +%d corner(s)%s."),
        ValidEdges.Num(),
        AppliedWidth,
        Out.bWasClamped ? TEXT(" clamped") : TEXT(""),
        Segments,
        Profile,
        Out.BevelFaceIndices.Num(),
        Out.CornerFaceIndices.Num(),
        Out.SkippedEdges > 0 ? *FString::Printf(TEXT(", skipped %d non-manifold"), Out.SkippedEdges) : TEXT(""));
    return Out;
}

FNGonBevelOutput BevelVertices(
    const TArray<FVector>& InPositions,
    const TArray<TArray<int32>>& InFaces,
    const TArray<int32>& SelectedVertices,
    const FNGonBevelOptions& Options)
{
    FNGonBevelOutput Out;
    const double RequestedWidth = FMath::Max(0.0, Options.Width);
    const int32 Segments = FMath::Clamp(Options.Segments, 1, 16);
    const double Profile = FMath::Clamp(Options.Profile, 0.05, 0.9999);
    Out.RequestedWidth = RequestedWidth;

    if (InPositions.IsEmpty() || InFaces.IsEmpty())
    {
        Out.Message = TEXT("Mesh is empty.");
        return Out;
    }
    if (SelectedVertices.IsEmpty())
    {
        Out.Message = TEXT("No vertices selected to bevel.");
        return Out;
    }
    if (!FMath::IsFinite(RequestedWidth) || RequestedWidth <= 1e-6)
    {
        Out.Message = TEXT("Bevel width must be greater than 0.");
        return Out;
    }

    FTopologyIndex Topo;
    if (!BuildTopologyIndex(InPositions, InFaces, Topo, Out.Message))
    {
        return Out;
    }

    TSet<int32> SelectedSet;
    TMap<int32, FVertexStar> Stars;
    for (const int32 V : SelectedVertices)
    {
        if (!InPositions.IsValidIndex(V) || SelectedSet.Contains(V)) continue;
        const TArray<FFaceCorner>* Corners = Topo.CornersByVertex.Find(V);
        if (!Corners || Corners->Num() < 2)
        {
            ++Out.SkippedVertices;
            continue;
        }
        FVertexStar Star;
        FString Err;
        if (!BuildVertexStar(V, *Corners, Star, Err) || Star.Neighbors.Num() < 2)
        {
            ++Out.SkippedVertices;
            continue;
        }
        SelectedSet.Add(V);
        Stars.Add(V, MoveTemp(Star));
    }

    if (SelectedSet.IsEmpty())
    {
        Out.Message = TEXT("No valid manifold vertices available to bevel.");
        return Out;
    }

    // Clamp width against incident edge lengths (0.48 * len if both endpoints beveled, 0.96 * len if one endpoint beveled)
    double AppliedWidth = RequestedWidth;
    if (Options.bClampOverlap)
    {
        for (const TPair<int32, FVertexStar>& KV : Stars)
        {
            const int32 V = KV.Key;
            for (const int32 Nbr : KV.Value.Neighbors)
            {
                const double EdgeLen = FVector::Distance(InPositions[V], InPositions[Nbr]);
                const double MaxAllowed = SelectedSet.Contains(Nbr) ? (EdgeLen * 0.48) : (EdgeLen * 0.96);
                if (MaxAllowed < AppliedWidth)
                {
                    AppliedWidth = MaxAllowed;
                    Out.bWasClamped = true;
                }
            }
        }
    }
    if (AppliedWidth <= 1e-6)
    {
        Out.Message = TEXT("Incident edges are too short to bevel the selected vertices.");
        return Out;
    }

    Out.Positions = InPositions;
    // Key: (VertexIdx, NeighborIdx) -> new slid vertex index
    TMap<uint64, int32> SlidVertexMap;
    auto MakeDirKey = [](int32 V, int32 Nbr) -> uint64
    {
        return (static_cast<uint64>(static_cast<uint32>(V)) << 32) | static_cast<uint64>(static_cast<uint32>(Nbr));
    };

    TArray<int32> SortedVerts = SelectedSet.Array();
    SortedVerts.Sort();
    for (const int32 V : SortedVerts)
    {
        const FVertexStar& Star = Stars[V];
        for (const int32 Nbr : Star.Neighbors)
        {
            const FVector Dir = (InPositions[Nbr] - InPositions[V]).GetSafeNormal();
            const double EdgeLen = FVector::Distance(InPositions[V], InPositions[Nbr]);
            const double SlideDist = FMath::Min(AppliedWidth, EdgeLen * (SelectedSet.Contains(Nbr) ? 0.48 : 0.96));
            const int32 NewV = Out.Positions.Add(InPositions[V] + Dir * SlideDist);
            SlidVertexMap.Add(MakeDirKey(V, Nbr), NewV);
            Out.CreatedVertices.Add(NewV);
        }
    }

    // Reconstruct original faces by replacing each beveled vertex with [Slid(V, Prev), Slid(V, Next)]
    Out.Faces.Reserve(InFaces.Num() + SortedVerts.Num() * Segments);
    for (int32 FaceIdx = 0; FaceIdx < InFaces.Num(); ++FaceIdx)
    {
        const TArray<int32>& Perim = InFaces[FaceIdx];
        const int32 N = Perim.Num();
        TArray<int32> NextPerim;
        NextPerim.Reserve(N + 4);
        for (int32 i = 0; i < N; ++i)
        {
            const int32 V = Perim[i];
            if (!SelectedSet.Contains(V))
            {
                NextPerim.Add(V);
                continue;
            }
            const int32 Prev = Perim[(i - 1 + N) % N];
            const int32 Next = Perim[(i + 1) % N];
            const int32* VPrev = SlidVertexMap.Find(MakeDirKey(V, Prev));
            const int32* VNext = SlidVertexMap.Find(MakeDirKey(V, Next));
            if (VPrev) NextPerim.Add(*VPrev);
            if (VNext && (!VPrev || *VNext != *VPrev)) NextPerim.Add(*VNext);
        }
        TArray<int32> Clean = CompactCyclicVertexIds(NextPerim);
        if (Clean.Num() >= 3)
        {
            Out.Faces.Add(MoveTemp(Clean));
        }
    }

    // Emit vertex bevel caps (single polygon for Segments == 1, concentric superellipse dome rings for Segments > 1)
    const TArray<FVector2D> ProfileSamples = BuildEvenProfileSamples(Segments, Profile);

    for (const int32 V : SortedVerts)
    {
        const FVertexStar& Star = Stars[V];
        TArray<int32> OuterRing;
        OuterRing.Reserve(Star.Neighbors.Num());
        for (const int32 Nbr : Star.Neighbors)
        {
            if (const int32* SlidV = SlidVertexMap.Find(MakeDirKey(V, Nbr)))
            {
                OuterRing.Add(*SlidV);
            }
        }
        if (OuterRing.Num() < 3) continue;

        // Orient outer ring opposite to adjacent reconstructed faces
        bool bFoundShared = false;
        bool bShouldReverse = false;
        for (int32 i = 0; i < OuterRing.Num() && !bFoundShared; ++i)
        {
            const int32 A = OuterRing[i];
            const int32 B = OuterRing[(i + 1) % OuterRing.Num()];
            for (const TArray<int32>& Existing : Out.Faces)
            {
                const int32 Rel = DirectedEdgeRelation(Existing, A, B);
                if (Rel != 0)
                {
                    bShouldReverse = (Rel == 1);
                    bFoundShared = true;
                    break;
                }
            }
        }
        if (bShouldReverse)
        {
            Algo::Reverse(OuterRing);
        }

        if (Segments <= 1)
        {
            const int32 CapIdx = Out.Faces.Add(MoveTemp(OuterRing));
            Out.BevelFaceIndices.Add(CapIdx);
            Out.CornerFaceIndices.Add(CapIdx);
        }
        else
        {
            FVector RingCenter = FVector::ZeroVector;
            for (const int32 RV : OuterRing) RingCenter += Out.Positions[RV];
            RingCenter /= static_cast<double>(OuterRing.Num());

            const double Exp = ProfileSuperellipseExponent(Profile);
            const double Fullness = FMath::Clamp(2.0 * FMath::Pow(UE_SQRT_2 * 0.5, 2.0 / Exp) - 1.0, -0.75, 0.9);
            const FVector ApexPos = FMath::Lerp(RingCenter, InPositions[V], FMath::Max(0.0, Fullness));

            TArray<int32> PrevRing = OuterRing;
            const int32 RingN = OuterRing.Num();
            for (int32 s = 1; s < Segments; ++s)
            {
                const FVector2D& Sample = ProfileSamples[s];
                // Sample moves from (0,1) at outer ring toward (1,0) at apex
                TArray<int32> CurRing;
                CurRing.Reserve(RingN);
                for (int32 i = 0; i < RingN; ++i)
                {
                    const FVector Pos = MapProfileSample(Out.Positions[OuterRing[i]], InPositions[V], ApexPos, Sample);
                    const int32 NewV = Out.Positions.Add(Pos);
                    Out.CreatedVertices.Add(NewV);
                    CurRing.Add(NewV);
                }
                for (int32 i = 0; i < RingN; ++i)
                {
                    const int32 Next = (i + 1) % RingN;
                    const int32 QuadIdx = Out.Faces.Add({ PrevRing[i], PrevRing[Next], CurRing[Next], CurRing[i] });
                    Out.BevelFaceIndices.Add(QuadIdx);
                }
                PrevRing = MoveTemp(CurRing);
            }

            // Center apex fan
            const int32 CenterV = Out.Positions.Add(ApexPos);
            Out.CreatedVertices.Add(CenterV);
            for (int32 i = 0; i < RingN; ++i)
            {
                const int32 Next = (i + 1) % RingN;
                const int32 TriIdx = Out.Faces.Add({ PrevRing[i], PrevRing[Next], CenterV });
                Out.CornerFaceIndices.Add(TriIdx);
            }
        }
    }

    CompactUnusedVertices(Out.Positions, Out.Faces, Out.CreatedVertices);

    Out.AppliedWidth = AppliedWidth;
    Out.bOk = true;
    Out.Message = FString::Printf(
        TEXT("Beveled %d vertex(es) (width %.2f%s, %d seg, profile %.2f): +%d face(s)."),
        SortedVerts.Num(),
        AppliedWidth,
        Out.bWasClamped ? TEXT(" clamped") : TEXT(""),
        Segments,
        Profile,
        Out.BevelFaceIndices.Num() + Out.CornerFaceIndices.Num());
    return Out;
}

FNGonBevelOutput BevelFaceBoundaries(
    const TArray<FVector>& InPositions,
    const TArray<TArray<int32>>& InFaces,
    const TSet<int32>& SelectedFaceIndices,
    const FNGonBevelOptions& Options)
{
    FNGonBevelOutput Out;
    if (SelectedFaceIndices.IsEmpty())
    {
        Out.Message = TEXT("No faces selected to bevel.");
        return Out;
    }

    // Collect edges that have exactly 1 incident face in SelectedFaceIndices (perimeter of the face selection)
    TMap<uint64, int32> SelectedIncidentCount;
    TMap<uint64, FIntPoint> EdgeEndpoints;
    for (const int32 FaceIdx : SelectedFaceIndices)
    {
        if (!InFaces.IsValidIndex(FaceIdx)) continue;
        const TArray<int32>& Perim = InFaces[FaceIdx];
        const int32 N = Perim.Num();
        for (int32 i = 0; i < N; ++i)
        {
            const int32 A = Perim[i];
            const int32 B = Perim[(i + 1) % N];
            const uint64 Key = MakeEdgeKey(A, B);
            SelectedIncidentCount.FindOrAdd(Key) += 1;
            EdgeEndpoints.Add(Key, FIntPoint(FMath::Min(A, B), FMath::Max(A, B)));
        }
    }

    TArray<FIntPoint> BoundaryEdges;
    for (const TPair<uint64, int32>& KV : SelectedIncidentCount)
    {
        if (KV.Value == 1)
        {
            BoundaryEdges.Add(EdgeEndpoints[KV.Key]);
        }
    }

    // Fallback: if all selected faces form a closed shell (no boundary), bevel all their edges
    if (BoundaryEdges.IsEmpty())
    {
        for (const TPair<uint64, FIntPoint>& KV : EdgeEndpoints)
        {
            BoundaryEdges.Add(KV.Value);
        }
    }

    return BevelEdges(InPositions, InFaces, BoundaryEdges, Options);
}
} // namespace NGon::Bevel

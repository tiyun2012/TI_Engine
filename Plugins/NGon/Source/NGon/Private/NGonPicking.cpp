#include "NGonPicking.h"

#include "NGonTopology.h"
#include "SceneView.h"
#include "Math/Box2D.h"

using namespace UE::Geometry;

void FNGonPicker::SetMesh(const FDynamicMesh3* InMesh)
{
    Spatial.Reset();
    Mesh = InMesh;
    bSpatialDirty = false;
    if (Mesh && Mesh->TriangleCount() > 0)
    {
        Spatial = MakeUnique<FDynamicMeshAABBTree3>(Mesh, true);
    }
}

void FNGonPicker::PositionsChanged()
{
    // A deformation can produce several position writes in one frame. Defer
    // the relatively expensive tree rebuild until the next actual query.
    bSpatialDirty = Spatial.IsValid();
}

void FNGonPicker::EnsureSpatialBuilt() const
{
    if (Spatial && bSpatialDirty)
    {
        Spatial->Build();
        bSpatialDirty = false;
    }
}

int32 FNGonPicker::PickFace(const FVector& Origin, const FVector& Direction) const
{
    if (!Spatial)
    {
        return INDEX_NONE;
    }

    EnsureSpatialBuilt();
    const int32 Triangle = Spatial->FindNearestHitTriangle(
        FRay3d(Origin, Direction), IMeshSpatial::FQueryOptions());
    return Mesh->IsTriangle(Triangle) ? Mesh->GetTriangleGroup(Triangle) : INDEX_NONE;
}

int32 FNGonPicker::Pick(
    ENGonElement Element,
    const FSceneView& View,
    const FVector2D& Cursor,
    const FNGonTopology& Topology,
    const TArray<FVector>& Positions) const
{
    if (!Spatial)
    {
        return INDEX_NONE;
    }
    EnsureSpatialBuilt();

    if (Element == ENGonElement::Face)
    {
        FVector Origin;
        FVector Direction;
        View.DeprojectFVector2D(Cursor, Origin, Direction);
        return PickFace(Origin, Direction);
    }

    constexpr double PickRadiusPixels = 10.0;
    const double DepthTolerance = FMath::Max(0.0001, Topology.RestBounds.GetSize().Size() * 0.000001);
    double BestDistanceSquared = FMath::Square(PickRadiusPixels);
    double BestDepth = TNumericLimits<double>::Max();
    int32 BestId = INDEX_NONE;

    const auto Consider = [&](int32 Id, const FVector2D& Pixel, const FVector& Position)
    {
        const double DistanceSquared = FVector2D::DistSquared(Cursor, Pixel);
        if (DistanceSquared > BestDistanceSquared)
        {
            return;
        }

        FVector Origin;
        FVector Direction;
        View.DeprojectFVector2D(Pixel, Origin, Direction);
        const double Depth = FVector::DotProduct(Position - Origin, Direction);
        if (Depth < 0.0 || (DistanceSquared == BestDistanceSquared && Depth >= BestDepth))
        {
            return;
        }

        double HitDepth = 0.0;
        int32 HitTriangle = INDEX_NONE;
        if (Spatial->FindNearestHitTriangle(
            FRay3d(Origin, Direction), HitDepth, HitTriangle, IMeshSpatial::FQueryOptions())
            && HitDepth + DepthTolerance < Depth)
        {
            return;
        }

        BestId = Id;
        BestDistanceSquared = DistanceSquared;
        BestDepth = Depth;
    };

    if (Element == ENGonElement::Vertex)
    {
        for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
        {
            const FVector4 Clip = View.WorldToScreen(Positions[Vertex]);
            if (Clip.W > 0.0 && FMath::IsFinite(Clip.W))
            {
                FVector2D Pixel;
                if (View.ScreenToPixel(Clip, Pixel))
                {
                    if (FMath::Abs(Pixel.X - Cursor.X) > PickRadiusPixels || FMath::Abs(Pixel.Y - Cursor.Y) > PickRadiusPixels)
                    {
                        continue;
                    }
                    Consider(Vertex, Pixel, Positions[Vertex]);
                }
            }
        }
        return BestId;
    }

    for (int32 EdgeIndex = 0; EdgeIndex < Topology.Edges.Num(); ++EdgeIndex)
    {
        const FIntPoint Edge = Topology.Edges[EdgeIndex];
        const FVector& A = Positions[Edge.X];
        const FVector& B = Positions[Edge.Y];
        const FVector4 ClipA = View.WorldToScreen(A);
        const FVector4 ClipB = View.WorldToScreen(B);
        if (ClipA.W <= 0.0 || ClipB.W <= 0.0 || !FMath::IsFinite(ClipA.W) || !FMath::IsFinite(ClipB.W))
        {
            continue;
        }
        FVector2D PixelA;
        FVector2D PixelB;
        if (!View.ScreenToPixel(ClipA, PixelA) || !View.ScreenToPixel(ClipB, PixelB))
        {
            continue;
        }
        if (Cursor.X < FMath::Min(PixelA.X, PixelB.X) - PickRadiusPixels || Cursor.X > FMath::Max(PixelA.X, PixelB.X) + PickRadiusPixels ||
            Cursor.Y < FMath::Min(PixelA.Y, PixelB.Y) - PickRadiusPixels || Cursor.Y > FMath::Max(PixelA.Y, PixelB.Y) + PickRadiusPixels)
        {
            continue;
        }

        const FVector2D ScreenEdge = PixelB - PixelA;
        const double LengthSquared = ScreenEdge.SizeSquared();
        if (LengthSquared <= UE_SMALL_NUMBER)
        {
            continue;
        }

        const double ScreenT = FMath::Clamp(
            FVector2D::DotProduct(Cursor - PixelA, ScreenEdge) / LengthSquared, 0.0, 1.0);
        // The closest point in screen space must be mapped back through perspective.
        const double WeightedA = (1.0 - ScreenT) / ClipA.W;
        const double WeightedB = ScreenT / ClipB.W;
        const double WorldT = WeightedB / (WeightedA + WeightedB);
        Consider(EdgeIndex, PixelA + ScreenEdge * ScreenT, FMath::Lerp(A, B, WorldT));
    }

    return BestId;
}

namespace
{
bool IsFinitePixel(const FVector2D& P)
{
    return FMath::IsFinite(P.X) && FMath::IsFinite(P.Y);
}

bool SegmentIntersectsRect(const FVector2D& A, const FVector2D& B, const FBox2D& Rect)
{
    if (Rect.IsInsideOrOn(A) || Rect.IsInsideOrOn(B)) return true;
    const FVector2D R0(Rect.Min.X, Rect.Min.Y), R1(Rect.Max.X, Rect.Min.Y);
    const FVector2D R2(Rect.Max.X, Rect.Max.Y), R3(Rect.Min.X, Rect.Max.Y);
    const auto Cross = [](const FVector2D& U, const FVector2D& V) { return U.X * V.Y - U.Y * V.X; };
    const auto Intersects = [&](const FVector2D& P0, const FVector2D& P1, const FVector2D& Q0, const FVector2D& Q1)
    {
        const FVector2D A0 = P1 - P0, B0 = Q1 - Q0;
        const double D = Cross(A0, B0);
        const double C = Cross(Q0 - P0, A0);
        if (FMath::IsNearlyZero(D)) return FMath::IsNearlyZero(C) &&
            FMath::Min(P0.X, P1.X) <= FMath::Max(Q0.X, Q1.X) && FMath::Max(P0.X, P1.X) >= FMath::Min(Q0.X, Q1.X) &&
            FMath::Min(P0.Y, P1.Y) <= FMath::Max(Q0.Y, Q1.Y) && FMath::Max(P0.Y, P1.Y) >= FMath::Min(Q0.Y, Q1.Y);
        const double T = Cross(Q0 - P0, B0) / D;
        const double U = Cross(Q0 - P0, A0) / D;
        return T >= 0.0 && T <= 1.0 && U >= 0.0 && U <= 1.0;
    };
    return Intersects(A, B, R0, R1) || Intersects(A, B, R1, R2) ||
        Intersects(A, B, R2, R3) || Intersects(A, B, R3, R0);
}

bool Project(const FSceneView& View, const FVector& Position, FVector2D& Out)
{
    const FVector4 Clip = View.WorldToScreen(Position);
    return Clip.W > 0.0 && FMath::IsFinite(Clip.W) && View.ScreenToPixel(Clip, Out) && IsFinitePixel(Out);
}
}

void FNGonPicker::BoxPick(
    ENGonElement Element, const FSceneView& View, const FBox2D& ScreenRect,
    const FNGonTopology& Topology, const TArray<FVector>& Positions,
    TSet<int32>& OutIds, bool bFullyContained) const
{
    OutIds.Reset();
    if (!Spatial || !ScreenRect.bIsValid || Positions.IsEmpty()) return;
    EnsureSpatialBuilt();
    const FBox2D Rect(FVector2D(FMath::Min(ScreenRect.Min.X, ScreenRect.Max.X), FMath::Min(ScreenRect.Min.Y, ScreenRect.Max.Y)),
        FVector2D(FMath::Max(ScreenRect.Min.X, ScreenRect.Max.X), FMath::Max(ScreenRect.Min.Y, ScreenRect.Max.Y)));
    const auto InRect = [&](const FVector2D& P) { return Rect.IsInsideOrOn(P); };

    if (Element == ENGonElement::Vertex)
    {
        for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
        {
            FVector2D Pixel;
            if (Project(View, Positions[Vertex], Pixel) && InRect(Pixel)) OutIds.Add(Vertex);
        }
        return;
    }
    if (Element == ENGonElement::Edge)
    {
        for (int32 EdgeId = 0; EdgeId < Topology.Edges.Num(); ++EdgeId)
        {
            const FIntPoint Edge = Topology.Edges[EdgeId];
            if (!Positions.IsValidIndex(Edge.X) || !Positions.IsValidIndex(Edge.Y)) continue;
            FVector2D A, B;
            if (!Project(View, Positions[Edge.X], A) || !Project(View, Positions[Edge.Y], B)) continue;
            const bool bHit = bFullyContained ? (InRect(A) && InRect(B)) : SegmentIntersectsRect(A, B, Rect);
            if (bHit) OutIds.Add(EdgeId);
        }
        return;
    }
    for (const TPair<int32, FNGonFace>& Pair : Topology.Faces)
    {
        const FNGonFace& Face = Pair.Value;
        if (Face.Perimeter.Num() < 3) continue;
        bool bAll = true, bHit = false;
        FVector2D First(0), Previous(0);
        FVector2D Center(0);
        int32 ProjectedCount = 0;
        bool bHavePrevious = false;
        for (const int32 Vertex : Face.Perimeter)
        {
            FVector2D Pixel;
            if (!Positions.IsValidIndex(Vertex) || !Project(View, Positions[Vertex], Pixel)) { bAll = false; continue; }
            if (!bHavePrevious) First = Pixel; else bHit |= SegmentIntersectsRect(Previous, Pixel, Rect);
            bAll &= InRect(Pixel); bHit |= InRect(Pixel); Center += Pixel; ++ProjectedCount;
            Previous = Pixel; bHavePrevious = true;
        }
        if (bHavePrevious) bHit |= SegmentIntersectsRect(Previous, First, Rect);
        if (ProjectedCount > 0) bHit |= InRect(Center / static_cast<double>(ProjectedCount));
        if ((bFullyContained && bAll) || (!bFullyContained && bHit)) OutIds.Add(Pair.Key);
    }
}

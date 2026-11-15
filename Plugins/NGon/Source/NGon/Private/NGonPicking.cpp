#include "NGonPicking.h"

#include "NGonTopology.h"
#include "SceneView.h"

using namespace UE::Geometry;

void FNGonPicker::SetMesh(const FDynamicMesh3* InMesh)
{
    Spatial.Reset();
    Mesh = InMesh;
    if (Mesh && Mesh->TriangleCount() > 0)
    {
        Spatial = MakeUnique<FDynamicMeshAABBTree3>(Mesh, true);
    }
}

void FNGonPicker::PositionsChanged()
{
    if (Spatial)
    {
        Spatial->Build();
    }
}

int32 FNGonPicker::PickFace(const FVector& Origin, const FVector& Direction) const
{
    if (!Spatial)
    {
        return INDEX_NONE;
    }

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

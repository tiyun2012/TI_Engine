#include "NGonTopology.h"

#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "StaticMeshAttributes.h"

using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "NGonTopology"

namespace
{
bool BuildSourceMesh(
    const TArray<FVector>& Positions,
    const TArray<TArray<int32>>& Faces,
    bool bDemoUVSeam,
    FMeshDescription& Out,
    FText& Error)
{
    if (Positions.Num() < 3 || Faces.IsEmpty())
    {
        Error = LOCTEXT("EmptyPolygonData", "The polygon file needs at least three positions and one face.");
        return false;
    }

    FStaticMeshAttributes Attributes(Out);
    Attributes.Register();
    Out.SetNumUVChannels(1);
    auto VertexPositions = Attributes.GetVertexPositions();
    auto UVs = Attributes.GetVertexInstanceUVs();
    UVs.SetNumChannels(1);
    auto SharedUVs = Attributes.GetUVCoordinates(0);
    auto Normals = Attributes.GetVertexInstanceNormals();

    TArray<FVertexID> VertexIds;
    VertexIds.Reserve(Positions.Num());
    for (const FVector& Position : Positions)
    {
        if (Position.ContainsNaN() || Position.GetAbsMax() > TNumericLimits<float>::Max())
        {
            Error = LOCTEXT("InvalidPosition", "The mesh contains an invalid vertex position.");
            return false;
        }
        const FVertexID Vertex = Out.CreateVertex();
        VertexPositions[Vertex] = FVector3f(Position);
        VertexIds.Add(Vertex);
    }

    const FPolygonGroupID PolygonGroup = Out.CreatePolygonGroup();
    TArray<FUVID> BaseUVIds;
    BaseUVIds.Init(FUVID(INDEX_NONE), Positions.Num());
    for (int32 FaceIndex = 0; FaceIndex < Faces.Num(); ++FaceIndex)
    {
        const TArray<int32>& Face = Faces[FaceIndex];
        TSet<int32> Seen;
        TArray<FVertexInstanceID> Instances;
        Instances.Reserve(Face.Num());
        for (const int32 VertexIndex : Face)
        {
            if (!VertexIds.IsValidIndex(VertexIndex) || Seen.Contains(VertexIndex))
            {
                Error = FText::Format(
                    LOCTEXT("InvalidFaceVertex", "Face {0} contains an invalid or repeated vertex index."),
                    FText::AsNumber(FaceIndex));
                return false;
            }

            Seen.Add(VertexIndex);
        }

        if (Face.Num() < 3)
        {
            Error = FText::Format(
                LOCTEXT("ShortFace", "Face {0} needs at least three vertices."),
                FText::AsNumber(FaceIndex));
            return false;
        }

        const FVector& Origin = Positions[Face[0]];
        FVector Normal = FVector::ZeroVector;
        double ScaleSquared = 0.0;
        for (int32 Corner = 1; Corner < Face.Num(); ++Corner)
        {
            const FVector A = Positions[Face[Corner]] - Origin;
            const FVector B = Positions[Face[(Corner + 1) % Face.Num()]] - Origin;
            Normal += FVector::CrossProduct(A, B);
            ScaleSquared = FMath::Max(ScaleSquared, A.SizeSquared());
        }

        const double Area = Normal.Size();
        if (!FMath::IsFinite(Area) || Area <= ScaleSquared * 1.e-12)
        {
            Error = FText::Format(
                LOCTEXT("DegenerateFace", "Face {0} has no usable area."),
                FText::AsNumber(FaceIndex));
            return false;
        }
        Normal /= Area;

        TMap<FVertexInstanceID, FUVID> UVForInstance;
        for (const int32 VertexIndex : Face)
        {
            const FVertexInstanceID Instance = Out.CreateVertexInstance(VertexIds[VertexIndex]);
            Instances.Add(Instance);
            Normals[Instance] = FVector3f(Normal);

            const FVector& Position = Positions[VertexIndex];
            const bool bSeparateUV = bDemoUVSeam && FaceIndex == 0;
            const FVector2f UV(
                static_cast<float>(Position.X / 100.0 + (bSeparateUV ? 3.0 : 0.0)),
                static_cast<float>(Position.Y / 100.0));
            UVs.Set(Instance, 0, UV);
            FUVID UvId = BaseUVIds[VertexIndex];
            if (bSeparateUV || UvId.GetValue() == INDEX_NONE)
            {
                UvId = Out.CreateUV(0);
                SharedUVs[UvId] = UV;
                if (!bSeparateUV)
                {
                    BaseUVIds[VertexIndex] = UvId;
                }
            }
            UVForInstance.Add(Instance, UvId);
        }

        const FPolygonID Polygon = Out.CreatePolygon(PolygonGroup, Instances);
        const auto Triangles = Out.GetPolygonTriangles(Polygon);
        if (Triangles.Num() != Instances.Num() - 2)
        {
            Error = FText::Format(
                LOCTEXT("TriangulationFailed", "Face {0} could not be triangulated as a single polygon."),
                FText::AsNumber(FaceIndex));
            return false;
        }
        for (const FTriangleID Triangle : Triangles)
        {
            const auto Corners = Out.GetTriangleVertexInstances(Triangle);
            const FUVID TriangleUVs[] = {
                UVForInstance.FindChecked(Corners[0]),
                UVForInstance.FindChecked(Corners[1]),
                UVForInstance.FindChecked(Corners[2])
            };
            Out.SetTriangleUVIndices(Triangle, MakeArrayView(TriangleUVs), 0);
        }
    }

    return true;
}
}

bool NGon::ConvertMeshDescription(const FMeshDescription& Source, FNGonMeshData& Out, FText& Error)
{
    if (Source.Triangles().Num() == 0)
    {
        Error = LOCTEXT("NoTriangles", "The mesh description contains no triangles.");
        return false;
    }

    FNGonMeshData Result;
    FNGonTopology& Topology = Result.Topology;
    FMeshDescriptionToDynamicMesh Converter;
    Converter.bDisableAttributes = false;
    Converter.bEnableOutputGroups = true;
    Converter.bCalculateMaps = true;
    // UE 5.8 keeps the group values in the nested EPrimaryGroupMode enum.
    // Qualify the enum type explicitly so this also compiles with MSVC's
    // stricter lookup rules.
    Converter.GroupMode = FMeshDescriptionToDynamicMesh::EPrimaryGroupMode::SetToPolygonID;
    Converter.Convert(&Source, Result.Mesh, false);

    if (Result.Mesh.TriangleCount() != Source.Triangles().Num())
    {
        Error = LOCTEXT("IncompleteConversion", "Some source triangles could not be converted. Check the source mesh for invalid faces.");
        return false;
    }

    const auto Positions = FStaticMeshConstAttributes(Source).GetVertexPositions();
    TMap<int32, int32> SourceToLogical;
    TSet<FIntPoint> UniqueEdges;
    for (const FPolygonID PolygonId : Source.Polygons().GetElementIDs())
    {
        FNGonFace& Face = Topology.Faces.Add(PolygonId.GetValue());
        const auto Instances = Source.GetPolygonVertexInstances(PolygonId);
        for (const FVertexInstanceID Instance : Instances)
        {
            const FVertexID VertexId = Source.GetVertexInstanceVertex(Instance);
            const int32 SourceId = VertexId.GetValue();
            int32* LogicalId = SourceToLogical.Find(SourceId);
            if (!LogicalId)
            {
                const FVector Position(Positions[VertexId]);
                if (Position.ContainsNaN())
                {
                    Error = LOCTEXT("InvalidPosition", "The mesh contains an invalid vertex position.");
                    return false;
                }

                const int32 NewId = Topology.RestPositions.Add(Position);
                Topology.SourceVertexIds.Add(SourceId);
                Topology.RestBounds += Position;
                LogicalId = &SourceToLogical.Add(SourceId, NewId);
            }

            Face.Perimeter.Add(*LogicalId);
        }

        Topology.MaxFaceCorners = FMath::Max(Topology.MaxFaceCorners, Face.Perimeter.Num());
        for (int32 Corner = 0; Corner < Face.Perimeter.Num(); ++Corner)
        {
            const int32 A = Face.Perimeter[Corner];
            const int32 B = Face.Perimeter[(Corner + 1) % Face.Perimeter.Num()];
            const FIntPoint Edge(FMath::Min(A, B), FMath::Max(A, B));
            bool bAlreadyAdded = false;
            UniqueEdges.Add(Edge, &bAlreadyAdded);
            if (A != B && !bAlreadyAdded)
            {
                Topology.Edges.Add(Edge);
            }
        }
    }

    // Keep the exact source polygon ID even if the converter uses a different
    // numbering convention for its output groups.
    for (const int32 TriangleId : Result.Mesh.TriangleIndicesItr())
    {
        if (!Converter.TriIDMap.IsValidIndex(TriangleId)
            || !Source.IsTriangleValid(Converter.TriIDMap[TriangleId]))
        {
            Error = LOCTEXT("MissingTriangleMap", "A preview triangle has no valid source triangle ID.");
            return false;
        }

        const int32 FaceId = Source.GetTrianglePolygon(Converter.TriIDMap[TriangleId]).GetValue();
        FNGonFace* Face = Topology.Faces.Find(FaceId);
        if (!Face)
        {
            Error = LOCTEXT("MissingSourceFace", "A source triangle has no valid polygon ID.");
            return false;
        }

        Result.Mesh.SetTriangleGroup(TriangleId, FaceId);
        Face->Triangles.Add(TriangleId);
    }

    Topology.DynamicToLogicalVertex.Init(INDEX_NONE, Result.Mesh.MaxVertexID());
    for (const int32 VertexId : Result.Mesh.VertexIndicesItr())
    {
        const int32* LogicalId = Converter.VertIDMap.IsValidIndex(VertexId)
            ? SourceToLogical.Find(Converter.VertIDMap[VertexId].GetValue())
            : nullptr;
        if (!LogicalId)
        {
            if (Result.Mesh.GetVtxTriangleCount(VertexId) > 0)
            {
                Error = LOCTEXT("MissingVertexMap", "A preview triangle uses a vertex with no source polygon vertex.");
                return false;
            }
            continue;
        }

        Topology.DynamicToLogicalVertex[VertexId] = *LogicalId;
    }

    Topology.TriangleCount = Result.Mesh.TriangleCount();
    Out = MoveTemp(Result);
    return true;
}

bool NGon::BuildPolygonMesh(
    const TArray<FVector>& Positions, const TArray<TArray<int32>>& Faces,
    FNGonMeshData& Out, FText& Error)
{
    FMeshDescription Source;
    return BuildSourceMesh(Positions, Faces, false, Source, Error)
        && ConvertMeshDescription(Source, Out, Error);
}

bool NGon::MakeDemo(FNGonMeshData& Out, FText& Error)
{
    const TArray<FVector> Positions = {
        {-150, -55, 0}, {-50, -55, 0}, {50, -55, 0}, {150, -55, 0},
        {150, 55, 0}, {100, 55, 0}, {100, 0, 0}, {50, 55, 0},
        {-50, 55, 0}, {-150, 55, 0}
    };
    const TArray<TArray<int32>> Faces = {
        {0, 1, 8, 9}, {1, 2, 7, 8}, {2, 3, 4, 5, 6, 7}
    };
    FMeshDescription Source;
    return BuildSourceMesh(Positions, Faces, true, Source, Error)
        && ConvertMeshDescription(Source, Out, Error);
}

#undef LOCTEXT_NAMESPACE

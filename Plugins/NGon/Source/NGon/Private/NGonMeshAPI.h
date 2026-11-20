#pragma once

#include "CoreMinimal.h"
#include "NGonPicking.h"
#include "NGonTopology.h"
#include "NGonBevel.h"

class FNGonPreviewScene;

/** Result of any NGon Mesh API operation, including provenance IDs for AI chaining. */
struct FNGonMeshOpResult
{
    bool bOk = false;
    FString Operation;
    FString Message;
    TArray<int32> CreatedVertices;
    TArray<int32> CreatedFaces;
    TArray<int32> ModifiedVertices;
    TArray<int32> DeletedFaces;
    TArray<int32> SelectedIds;
    FString PayloadJson;

    FString ToJsonString() const;
};

/** Connected surface shell summary (ported and extended from TiMesh::MeshShell). */
struct FNGonMeshShell
{
    int32 AnchorVertex = INDEX_NONE;
    TArray<int32> Vertices;
    TArray<int32> Edges;
    TArray<int32> Faces;
    int32 TriangleCount = 0;
    int32 BoundaryEdges = 0;
    int32 NonManifoldEdges = 0;
    FBox Bounds{ForceInit};
};

struct FNGonShellAnalysis
{
    TArray<FNGonMeshShell> Shells;
    TArray<int32> LooseVertices;
    int32 TotalBoundaryEdges = 0;
    int32 TotalNonManifoldEdges = 0;
    int32 NGonFaceCount = 0;
};

struct FNGonValidationIssue
{
    FString Code;
    FString Message;
};

struct FNGonValidationReport
{
    TArray<FNGonValidationIssue> Issues;
    bool IsOk() const { return Issues.IsEmpty(); }
};

/**
 * Unified, AI-controllable Mesh Modeling, Analysis, Deformation, and JSON Command API
 * operating directly on FNGonPreviewScene with atomic validation and Undo/Redo history.
 */
class FNGonMeshAPI
{
public:
    // --- History (Undo / Redo) ---
    static void ClearHistory(const FNGonPreviewScene* Scene);
    static bool CanUndo(const FNGonPreviewScene* Scene);
    static bool CanRedo(const FNGonPreviewScene* Scene);
    static int32 GetUndoCount(const FNGonPreviewScene* Scene);
    static int32 GetRedoCount(const FNGonPreviewScene* Scene);
    static FNGonMeshOpResult Undo(FNGonPreviewScene& Scene);
    static FNGonMeshOpResult Redo(FNGonPreviewScene& Scene);

    // --- Procedural Primitives ---
    static FNGonMeshOpResult CreateBox(
        FNGonPreviewScene& Scene,
        double Width = 100.0,
        double Depth = 100.0,
        double Height = 100.0,
        int32 SubdivX = 1,
        int32 SubdivY = 1,
        int32 SubdivZ = 1,
        const FVector& Center = FVector::ZeroVector);

    static FNGonMeshOpResult CreatePlane(
        FNGonPreviewScene& Scene,
        double Width = 200.0,
        double Depth = 200.0,
        int32 SubdivX = 4,
        int32 SubdivY = 4,
        const FVector& Center = FVector::ZeroVector);

    static FNGonMeshOpResult CreateCylinder(
        FNGonPreviewScene& Scene,
        double Radius = 50.0,
        double Height = 120.0,
        int32 Sides = 16,
        int32 HeightSegments = 1,
        const FVector& Center = FVector::ZeroVector);

    static FNGonMeshOpResult CreateSphere(
        FNGonPreviewScene& Scene,
        double Radius = 60.0,
        int32 Rings = 10,
        int32 Segments = 16,
        const FVector& Center = FVector::ZeroVector);

    static FNGonMeshOpResult CreatePolygon(
        FNGonPreviewScene& Scene,
        const TArray<FVector>& Positions);

    // --- Face Modeling Operations (TiMesh-inspired, N-Gon preserving) ---
    static FNGonMeshOpResult ExtrudeFaces(
        FNGonPreviewScene& Scene,
        const TArray<int32>& FaceIds,
        double Distance = 25.0,
        double Scale = 1.0);

    static FNGonMeshOpResult InsetFaces(
        FNGonPreviewScene& Scene,
        const TArray<int32>& FaceIds,
        double Ratio = 0.25,
        double Depth = 0.0);

    static FNGonMeshOpResult BevelFaces(
        FNGonPreviewScene& Scene,
        const TArray<int32>& FaceIds,
        double Ratio = 0.18,
        double Height = 8.0);

    // --- Joint VMesh Bevel Operations (Edge / Vertex / Face Boundary) ---
    static FNGonMeshOpResult BevelEdges(
        FNGonPreviewScene& Scene,
        const TArray<int32>& EdgeIds,
        const FNGonBevelOptions& Options = FNGonBevelOptions());

    static FNGonMeshOpResult BevelVertices(
        FNGonPreviewScene& Scene,
        const TArray<int32>& VertexIds,
        const FNGonBevelOptions& Options = FNGonBevelOptions());

    static FNGonMeshOpResult BevelFacesBoundary(
        FNGonPreviewScene& Scene,
        const TArray<int32>& FaceIds,
        const FNGonBevelOptions& Options = FNGonBevelOptions());

    static FNGonMeshOpResult BevelSelection(
        FNGonPreviewScene& Scene,
        const FNGonBevelOptions& Options = FNGonBevelOptions());

    static FNGonMeshOpResult SubdivideFaces(
        FNGonPreviewScene& Scene,
        const TArray<int32>& FaceIds);

    static FNGonMeshOpResult TriangulateFaces(
        FNGonPreviewScene& Scene,
        const TArray<int32>& FaceIds);

    static FNGonMeshOpResult FlipFaces(
        FNGonPreviewScene& Scene,
        const TArray<int32>& FaceIds);

    static FNGonMeshOpResult DeleteFaces(
        FNGonPreviewScene& Scene,
        const TArray<int32>& FaceIds);

    // --- Edge & Vertex Topology Operations ---
    static FNGonMeshOpResult SplitEdge(
        FNGonPreviewScene& Scene,
        int32 EdgeId,
        double T = 0.5);

    static FNGonMeshOpResult CutFace(
        FNGonPreviewScene& Scene,
        int32 FaceId,
        int32 VertexA,
        int32 VertexB);

    static FNGonMeshOpResult CollapseEdge(
        FNGonPreviewScene& Scene,
        int32 EdgeId);

    static FNGonMeshOpResult WeldVertices(
        FNGonPreviewScene& Scene,
        const TArray<int32>& VertexIds,
        double Threshold = 0.0);

    static FNGonMeshOpResult BridgeEdges(
        FNGonPreviewScene& Scene,
        int32 EdgeA,
        int32 EdgeB);

    static FNGonMeshOpResult FillHole(
        FNGonPreviewScene& Scene,
        const TArray<int32>& OrderedVertices);

    static FNGonMeshOpResult MirrorMesh(
        FNGonPreviewScene& Scene,
        EAxis::Type Axis = EAxis::X,
        bool bKeepOriginal = true);

    // --- Transforms & Procedural Deformers ---
    static FNGonMeshOpResult TransformVertices(
        FNGonPreviewScene& Scene,
        const TArray<int32>& VertexIds,
        const FVector& DeltaTranslation,
        const FRotator& DeltaRotation = FRotator::ZeroRotator,
        const FVector& Scale = FVector::OneVector);

    static FNGonMeshOpResult SmoothVertices(
        FNGonPreviewScene& Scene,
        const TArray<int32>& VertexIds,
        int32 Iterations = 2,
        double Strength = 0.5);

    static FNGonMeshOpResult ApplyNoise(
        FNGonPreviewScene& Scene,
        const TArray<int32>& VertexIds,
        double Amplitude = 5.0,
        double Frequency = 0.05,
        int32 Seed = 1337);

    static FNGonMeshOpResult ApplyTaper(
        FNGonPreviewScene& Scene,
        EAxis::Type Axis = EAxis::Z,
        double Amount = 0.35);

    static FNGonMeshOpResult ApplyTwist(
        FNGonPreviewScene& Scene,
        EAxis::Type Axis = EAxis::Z,
        double AngleDeg = 45.0);

    // --- AI-Friendly Selection Helpers ---
    static FNGonMeshOpResult SelectElements(
        FNGonPreviewScene& Scene,
        ENGonElement Element,
        const TArray<int32>& Ids,
        bool bAdd = false);

    static FNGonMeshOpResult SelectAll(FNGonPreviewScene& Scene, ENGonElement Element);
    static FNGonMeshOpResult InvertSelection(FNGonPreviewScene& Scene);
    static FNGonMeshOpResult GrowSelection(FNGonPreviewScene& Scene);
    static FNGonMeshOpResult ShrinkSelection(FNGonPreviewScene& Scene);

    static FNGonMeshOpResult SelectFacesByNormal(
        FNGonPreviewScene& Scene,
        const FVector& Direction,
        double MaxAngleDeg = 30.0,
        bool bAdd = false);

    static FNGonMeshOpResult SelectByBounds(
        FNGonPreviewScene& Scene,
        ENGonElement Element,
        const FBox& Box,
        bool bAdd = false);

    static FNGonMeshOpResult SelectNGons(FNGonPreviewScene& Scene, int32 MinSides = 5);
    static FNGonMeshOpResult SelectBoundaryEdges(FNGonPreviewScene& Scene);
    static FNGonMeshOpResult SelectEdgeLoop(FNGonPreviewScene& Scene, int32 StartEdgeId = INDEX_NONE, bool bAdd = false);
    static FNGonMeshOpResult SelectEdgeRing(FNGonPreviewScene& Scene, int32 StartEdgeId = INDEX_NONE, bool bAdd = false);
    static FNGonMeshOpResult SelectFaceLoop(FNGonPreviewScene& Scene, int32 StartEdgeId = INDEX_NONE, bool bAdd = false);
    static FNGonMeshOpResult SelectLinkedShell(FNGonPreviewScene& Scene);
    static FNGonMeshOpResult SelectVertexLoop(FNGonPreviewScene& Scene, int32 AnchorVert = INDEX_NONE, int32 TargetVert = INDEX_NONE, bool bAdd = false);
    static FNGonMeshOpResult SelectShortestPath(FNGonPreviewScene& Scene, int32 AnchorId = INDEX_NONE, int32 TargetId = INDEX_NONE, bool bAdd = false);
    static FNGonMeshOpResult SelectDoubleClickGesture(FNGonPreviewScene& Scene, ENGonElement Element, int32 AnchorId, int32 TargetId, bool bAdd = false);
    static FNGonMeshOpResult ConvertSelection(FNGonPreviewScene& Scene, ENGonElement TargetElement);

    // --- Topology & Manifold Analysis ---
    static FNGonShellAnalysis AnalyzeShells(const FNGonPreviewScene& Scene);
    static FNGonValidationReport CheckManifold(const FNGonPreviewScene& Scene, bool bOriented = true);

    // --- AI JSON Inspection, Schema, and Command Execution ---
    static FString ExportMeshSummaryJson(const FNGonPreviewScene& Scene, bool bIncludeGeometry = false);
    static FString GetApiSchemaJson();
    static FNGonMeshOpResult ExecuteJsonCommand(FNGonPreviewScene& Scene, const FString& JsonText);
};

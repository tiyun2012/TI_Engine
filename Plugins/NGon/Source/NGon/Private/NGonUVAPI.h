#pragma once

#include "CoreMinimal.h"
#include "NGonUVEditor.h"

struct FNGonUVOpResult
{
    bool bOk = false;
    FString Operation;
    FString Message;
    TArray<int32> SelectedElements;
    TArray<int32> ModifiedElements;
    FString PayloadJson;

    FString ToJsonString() const;
};

struct FNGonUVIslandInfo
{
    int32 IslandId = INDEX_NONE;
    TArray<int32> Elements;
    TArray<int32> Triangles;
    FBox2D Bounds{ForceInit};
    double AreaUV = 0.0;
    int32 FlippedTriangles = 0;
};

struct FNGonUVAnalysis
{
    TArray<FNGonUVIslandInfo> Islands;
    FBox2D TotalBounds{ForceInit};
    double TotalAreaUV = 0.0;
    int32 FlippedTriangleCount = 0;
    int32 OutOfUnitBoundsCorners = 0;
    int32 SeamEdgeCount = 0;
};

enum class ENGonUVAlign : uint8
{
    Left,
    Right,
    Top,
    Bottom,
    CenterU,
    CenterV
};

/**
 * Powerful, AI-controllable UV Editing, Layout, Packing, Relaxation, Analysis,
 * and JSON Command API operating on FNGonUVDocument with Undo/Redo history.
 */
class FNGonUVAPI
{
public:
    // --- History ---
    static void ClearHistory(const FNGonUVDocument* Doc);
    static void PushUndoSnapshot(const FNGonUVDocument& Doc);
    static bool CanUndo(const FNGonUVDocument* Doc);
    static bool CanRedo(const FNGonUVDocument* Doc);
    static int32 GetUndoCount(const FNGonUVDocument* Doc);
    static int32 GetRedoCount(const FNGonUVDocument* Doc);
    static FNGonUVOpResult Undo(FNGonUVDocument& Doc);
    static FNGonUVOpResult Redo(FNGonUVDocument& Doc);

    // --- Selection & Queries ---
    static FNGonUVOpResult SelectAll(FNGonUVDocument& Doc);
    static FNGonUVOpResult ClearSelection(FNGonUVDocument& Doc);
    static FNGonUVOpResult InvertSelection(FNGonUVDocument& Doc);
    static FNGonUVOpResult GrowSelection(FNGonUVDocument& Doc);
    static FNGonUVOpResult ShrinkSelection(FNGonUVDocument& Doc);
    static FNGonUVOpResult SelectElements(FNGonUVDocument& Doc, const TArray<int32>& ElementIds, bool bAdd = false);
    static FNGonUVOpResult SelectIsland(FNGonUVDocument& Doc, int32 IslandId, bool bAdd = false);
    static FNGonUVOpResult SelectLinkedIslands(FNGonUVDocument& Doc);
    static FNGonUVOpResult SelectSeamCorners(FNGonUVDocument& Doc);
    static FNGonUVOpResult SelectFlippedTriangles(FNGonUVDocument& Doc);
    static FNGonUVOpResult SelectByBounds(FNGonUVDocument& Doc, const FBox2D& Rect, bool bAdd = false);

    // --- Transform & Alignment ---
    static FNGonUVOpResult TranslateSelected(FNGonUVDocument& Doc, const FVector2f& Delta);
    static FNGonUVOpResult RotateSelected(FNGonUVDocument& Doc, float Degrees);
    static FNGonUVOpResult ScaleSelected(FNGonUVDocument& Doc, const FVector2f& ScaleUV);
    static FNGonUVOpResult FlipSelected(FNGonUVDocument& Doc, bool bHorizontal);
    static FNGonUVOpResult AlignSelected(FNGonUVDocument& Doc, ENGonUVAlign Alignment);
    static FNGonUVOpResult StraightenSelected(FNGonUVDocument& Doc);
    static FNGonUVOpResult SnapSelectedToGrid(FNGonUVDocument& Doc, float GridStep = 0.0625f);

    // --- Layout, Packing & Relaxation ---
    static FNGonUVOpResult NormalizeSelected(FNGonUVDocument& Doc, bool bKeepAspect = true, float Padding = 0.02f);
    static FNGonUVOpResult PackIslands(FNGonUVDocument& Doc, float Padding = 0.02f, bool bRotateToFit = true);
    static FNGonUVOpResult RelaxSelected(FNGonUVDocument& Doc, int32 Iterations = 4, float Strength = 0.45f, bool bPinBorder = true);
    static FNGonUVOpResult StackSelectedIslands(FNGonUVDocument& Doc);
    static FNGonUVOpResult DistributeSelectedIslands(FNGonUVDocument& Doc, bool bHorizontal = true, float Spacing = 0.03f);
    static FNGonUVOpResult MoveToUDIMTile(FNGonUVDocument& Doc, int32 TileU, int32 TileV, bool bNormalizeInsideTile = false);

    // --- Analysis & AI JSON Interface ---
    static FNGonUVAnalysis AnalyzeUVs(const FNGonUVDocument& Doc);
    static FString ExportUVSummaryJson(const FNGonUVDocument& Doc, bool bIncludeIslands = true);
    static FString GetApiSchemaJson();
    static FNGonUVOpResult ExecuteJsonCommand(FNGonUVDocument& Doc, const FString& JsonText);
};

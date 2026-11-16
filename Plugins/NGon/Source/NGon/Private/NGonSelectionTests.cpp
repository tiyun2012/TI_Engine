#if WITH_DEV_AUTOMATION_TESTS

#include "NGonPreviewScene.h"
#include "NGonTopology.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonSafeEdgeLoopTest, "NGon.Selection.SafeEdgeLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonSafeEdgeLoopTest::RunTest(const FString& Parameters)
{
    const TArray<FVector> Positions = {
        {0, 0, 0}, {100, 0, 0}, {200, 0, 0},
        {0, 100, 0}, {100, 100, 0}, {200, 100, 0}};
    const TArray<TArray<int32>> Faces = {{0, 1, 4, 3}, {1, 2, 5, 4}};
    FNGonMeshData Data;
    FText Error;
    if (!TestTrue(TEXT("Build quad strip"), NGon::BuildPolygonMesh(Positions, Faces, Data, Error))) return false;

    FNGonPreviewScene Scene;
    Scene.SetMesh(MoveTemp(Data));
    int32 StartEdge = INDEX_NONE;
    StartEdge = Scene.GetTopology().Edges.IsEmpty() ? INDEX_NONE : 0;
    TestTrue(TEXT("Find valid start edge"), StartEdge != INDEX_NONE);
    TestTrue(TEXT("Loop follows only the quad strip"), Scene.SelectEdgeLoop(StartEdge, false));
    TestTrue(TEXT("Loop produced an edge selection"), Scene.DescribeSelection().ToString().Contains(TEXT("edge")));
    TestFalse(TEXT("Invalid edge is rejected"), Scene.SelectEdgeLoop(INDEX_NONE, false));
    return true;
}

#endif

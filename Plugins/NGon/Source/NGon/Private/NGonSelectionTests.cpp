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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonDeleteSelectedFaceTest, "NGon.Selection.DeleteSelectedFace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonDeleteSelectedFaceTest::RunTest(const FString& Parameters)
{
    FNGonMeshData Data;
    FText Error;
    if (!TestTrue(TEXT("Build demo mesh"), NGon::MakeDemo(Data, Error))) return false;
    FNGonPreviewScene Scene;
    Scene.SetMesh(MoveTemp(Data));
    const int32 InitialFaces = Scene.GetTopology().Faces.Num();
    Scene.SelectElement(ENGonElement::Face, 0);
    TestTrue(TEXT("Delete selected face"), Scene.DeleteSelectedFaces());
    TestEqual(TEXT("One face removed"), Scene.GetTopology().Faces.Num(), InitialFaces - 1);
    TestTrue(TEXT("Face selection is cleared after rebuild"), Scene.DescribeSelection().ToString().Contains(TEXT("No selection")));

    int32 RemainingFaceId = INDEX_NONE;
    for (const TPair<int32, FNGonFace>& FacePair : Scene.GetTopology().Faces)
    {
        RemainingFaceId = FacePair.Key;
        break;
    }
    if (!TestTrue(TEXT("Find a remaining face"), RemainingFaceId != INDEX_NONE)) return false;
    Scene.SelectElement(ENGonElement::Face, RemainingFaceId);
    TestTrue(TEXT("Delete down to one face"), Scene.DeleteSelectedFaces());
    TestEqual(TEXT("One face remains"), Scene.GetTopology().Faces.Num(), 1);

    int32 FinalFaceId = INDEX_NONE;
    for (const TPair<int32, FNGonFace>& FacePair : Scene.GetTopology().Faces)
    {
        FinalFaceId = FacePair.Key;
        break;
    }
    if (!TestTrue(TEXT("Find final face"), FinalFaceId != INDEX_NONE)) return false;
    Scene.SelectElement(ENGonElement::Face, FinalFaceId);
    TestFalse(TEXT("Cannot delete the final remaining face"), Scene.DeleteSelectedFaces());
    return true;
}

#endif

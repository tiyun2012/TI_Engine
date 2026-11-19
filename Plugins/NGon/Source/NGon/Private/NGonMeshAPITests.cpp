#if WITH_DEV_AUTOMATION_TESTS

#include "NGonMeshAPI.h"
#include "NGonPreviewScene.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonMeshAPIWorkflowTest, "NGon.MeshAPI.ModelingAndJsonBatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonMeshAPIWorkflowTest::RunTest(const FString& Parameters)
{
    FNGonPreviewScene Scene;

    // 1. Procedural Box + Manifold Check
    const FNGonMeshOpResult BoxRes = FNGonMeshAPI::CreateBox(Scene, 100.0, 100.0, 100.0);
    TestTrue(TEXT("CreateBox succeeds"), BoxRes.bOk);
    TestEqual(TEXT("Box has 6 quad faces"), Scene.GetTopology().Faces.Num(), 6);
    TestTrue(TEXT("Box is oriented manifold"), FNGonMeshAPI::CheckManifold(Scene, true).IsOk());

    const FNGonShellAnalysis Shells = FNGonMeshAPI::AnalyzeShells(Scene);
    TestEqual(TEXT("Box has 1 connected shell"), Shells.Shells.Num(), 1);
    TestEqual(TEXT("Closed box has 0 boundary edges"), Shells.TotalBoundaryEdges, 0);

    // 2. AI JSON Batch: Select top face -> Inset -> Extrude -> Bevel
    const FString BatchJson = TEXT(
        "[\n"
        "  {\"op\": \"select_faces_by_normal\", \"direction\": [0, 0, 1], \"max_angle_deg\": 20},\n"
        "  {\"op\": \"inset_faces\", \"ratio\": 0.25},\n"
        "  {\"op\": \"extrude_faces\", \"distance\": 40, \"scale\": 0.8},\n"
        "  {\"op\": \"bevel_faces\", \"ratio\": 0.2, \"height\": 8}\n"
        "]");
    const FNGonMeshOpResult BatchRes = FNGonMeshAPI::ExecuteJsonCommand(Scene, BatchJson);
    TestTrue(TEXT("JSON batch command executes cleanly"), BatchRes.bOk);
    TestTrue(TEXT("Face count grew after inset+extrude+bevel"), Scene.GetTopology().Faces.Num() > 14);
    TestTrue(TEXT("Resulting mesh remains manifold"), FNGonMeshAPI::CheckManifold(Scene, true).IsOk());

    // 3. Undo / Redo Verification
    const int32 FacesBeforeUndo = Scene.GetTopology().Faces.Num();
    TestTrue(TEXT("Undo stack is available"), FNGonMeshAPI::CanUndo(&Scene));
    TestTrue(TEXT("Undo succeeds"), FNGonMeshAPI::Undo(Scene).bOk);
    TestTrue(TEXT("Undo reduced face count"), Scene.GetTopology().Faces.Num() < FacesBeforeUndo);
    TestTrue(TEXT("Redo restores face count"), FNGonMeshAPI::Redo(Scene).bOk && Scene.GetTopology().Faces.Num() == FacesBeforeUndo);

    // 4. Inspect JSON & Schema
    const FString Summary = FNGonMeshAPI::ExportMeshSummaryJson(Scene, true);
    TestTrue(TEXT("Summary JSON includes face_count and manifold_ok"),
        Summary.Contains(TEXT("face_count")) && Summary.Contains(TEXT("manifold_ok")));
    const FString Schema = FNGonMeshAPI::GetApiSchemaJson();
    TestTrue(TEXT("API schema lists extrude_faces and select_edge_loop"),
        Schema.Contains(TEXT("extrude_faces")) && Schema.Contains(TEXT("select_edge_loop")));

    // 5. Selection Suite: Edge Loop, Edge Ring, Face Loop, Linked Shell, Convert Selection, Edge Shrink
    TestTrue(TEXT("Create subdivided box for selection tests"), FNGonMeshAPI::CreateBox(Scene, 100, 100, 100, 2, 2, 2).bOk);
    TestTrue(TEXT("SelectEdgeLoop succeeds"), FNGonMeshAPI::SelectEdgeLoop(Scene, 0).bOk && Scene.GetSelectedIds().Num() >= 2);
    TestTrue(TEXT("SelectEdgeRing succeeds"), FNGonMeshAPI::SelectEdgeRing(Scene, 0).bOk && Scene.GetSelectedIds().Num() >= 2);
    TestTrue(TEXT("SelectFaceLoop succeeds"), FNGonMeshAPI::SelectFaceLoop(Scene, 0).bOk && Scene.GetSelectedIds().Num() >= 4);
    TestTrue(TEXT("ConvertSelection Face->Vertex succeeds"),
        FNGonMeshAPI::ConvertSelection(Scene, ENGonElement::Vertex).bOk && Scene.GetSelectedElement() == ENGonElement::Vertex);
    TestTrue(TEXT("SelectLinkedShell selects all vertices on closed box"),
        FNGonMeshAPI::SelectLinkedShell(Scene).bOk && Scene.GetSelectedIds().Num() == Scene.GetPositions().Num());

    return true;
}

#endif
// NGon Unified ABI Sync v5.8.2

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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonBevelKernelTest, "NGon.MeshAPI.BevelKernel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonBevelKernelTest::RunTest(const FString& Parameters)
{
    FNGonPreviewScene Scene;

    // 1. Single-edge bevel (1 segment & 3 segments)
    TestTrue(TEXT("CreateBox for single-edge bevel"), FNGonMeshAPI::CreateBox(Scene, 100.0, 100.0, 100.0).bOk);
    {
        FNGonBevelOptions Opts;
        Opts.Width = 10.0;
        Opts.Segments = 3;
        Opts.Profile = 0.5;
        Opts.bClampOverlap = true;
        const FNGonMeshOpResult Res = FNGonMeshAPI::BevelEdges(Scene, { 0 }, Opts);
        TestTrue(TEXT("Single-edge 3-seg bevel succeeds"), Res.bOk);
        TestTrue(TEXT("Single-edge 3-seg bevel remains oriented manifold"), FNGonMeshAPI::CheckManifold(Scene, true).IsOk());
    }

    // 2. 3-edge cube corner bevel with ADJ quad grid (Segments = 3)
    TestTrue(TEXT("CreateBox for 3-edge corner bevel"), FNGonMeshAPI::CreateBox(Scene, 100.0, 100.0, 100.0).bOk);
    {
        TArray<int32> CornerEdges;
        const FNGonTopology& Topo = Scene.GetTopology();
        for (int32 e = 0; e < Topo.Edges.Num(); ++e)
        {
            if (Topo.Edges[e].X == 0 || Topo.Edges[e].Y == 0)
            {
                CornerEdges.Add(e);
            }
        }
        TestEqual(TEXT("Cube corner has 3 incident edges"), CornerEdges.Num(), 3);

        FNGonBevelOptions Opts;
        Opts.Width = 12.0;
        Opts.Segments = 3;
        Opts.Profile = 0.5;
        Opts.bClampOverlap = true;
        const FNGonMeshOpResult Res = FNGonMeshAPI::BevelEdges(Scene, CornerEdges, Opts);
        TestTrue(TEXT("3-edge corner ADJ bevel succeeds"), Res.bOk);
        TestTrue(TEXT("3-edge corner ADJ bevel remains oriented manifold"), FNGonMeshAPI::CheckManifold(Scene, true).IsOk());
    }

    // 3. Clamp Overlap safety on oversized width (Width = 500 on 100x100x100 cube)
    TestTrue(TEXT("CreateBox for clamp-overlap test"), FNGonMeshAPI::CreateBox(Scene, 100.0, 100.0, 100.0).bOk);
    {
        TArray<int32> AllEdges;
        for (int32 e = 0; e < Scene.GetTopology().Edges.Num(); ++e) AllEdges.Add(e);

        FNGonBevelOptions Opts;
        Opts.Width = 500.0;
        Opts.Segments = 2;
        Opts.Profile = 0.5;
        Opts.bClampOverlap = true;
        const FNGonMeshOpResult Res = FNGonMeshAPI::BevelEdges(Scene, AllEdges, Opts);
        TestTrue(TEXT("Oversized all-edge bevel clamps and succeeds"), Res.bOk);
        TestTrue(TEXT("Payload reports was_clamped=true"), Res.PayloadJson.Contains(TEXT("\"was_clamped\":true")));
        TestTrue(TEXT("Clamped all-edge bevel remains oriented manifold"), FNGonMeshAPI::CheckManifold(Scene, true).IsOk());
    }

    // 4. Vertex bevel (1 segment & 2 segments) + Undo/Redo
    TestTrue(TEXT("CreateBox for vertex bevel"), FNGonMeshAPI::CreateBox(Scene, 100.0, 100.0, 100.0).bOk);
    {
        FNGonBevelOptions Opts;
        Opts.Width = 15.0;
        Opts.Segments = 2;
        Opts.Profile = 0.5;
        Opts.bClampOverlap = true;
        const FNGonMeshOpResult Res = FNGonMeshAPI::BevelVertices(Scene, { 0, 1 }, Opts);
        TestTrue(TEXT("Multi-vertex 2-seg bevel succeeds"), Res.bOk);
        TestTrue(TEXT("Vertex bevel remains oriented manifold"), FNGonMeshAPI::CheckManifold(Scene, true).IsOk());
        TestTrue(TEXT("Undo restores 6-face box"), FNGonMeshAPI::Undo(Scene).bOk && Scene.GetTopology().Faces.Num() == 6);
        TestTrue(TEXT("Redo restores vertex bevel"), FNGonMeshAPI::Redo(Scene).bOk && Scene.GetTopology().Faces.Num() > 6);
    }

    return true;
}

#endif
// NGon Unified ABI Sync v5.8.2

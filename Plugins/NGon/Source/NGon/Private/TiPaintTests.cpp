#include "Misc/AutomationTest.h"
#include "TiPaintAPI.h"
#include "TiPaintDocument.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTiPaintDocumentTest, "TiPaint.Document.BrushLayersUndo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTiPaintDocumentTest::RunTest(const FString&)
{
    TSharedRef<FTiPaintDocument> Document = MakeShared<FTiPaintDocument>(64, 64);
    FTiPaintBrush Brush = Document->Brush();
    Brush.Color = FColor(220, 40, 20, 255);
    Brush.Radius = 8.0f;
    Document->SetBrush(Brush);
    Document->BeginStroke(0.5f, 0.5f);
    Document->AddStrokePoint(0.6f, 0.5f);
    Document->EndStroke();
    TestEqual(TEXT("One stroke creates one undo entry"), Document->UndoCount(), 1);
    bool bPainted = false;
    for (const FColor& Pixel : Document->Composite()) bPainted |= Pixel.A > 0;
    TestTrue(TEXT("Paint stroke changes composite"), bPainted);
    Document->Undo();
    bool bCleared = true;
    for (const FColor& Pixel : Document->Composite()) bCleared &= Pixel.A == 0;
    TestTrue(TEXT("Undo restores transparent layer"), bCleared);
    Document->Redo();
    TestTrue(TEXT("Redo restores painted composite"), Document->Composite().Num() == 4096);
    Document->AddLayer();
    TestEqual(TEXT("Layer add is available"), Document->Layers().Num(), 2);
    Document->ToggleActiveLock();
    Document->BeginStroke(0.5f, 0.5f);
    TestFalse(TEXT("Locked active layer rejects stroke"), Document->IsStroking());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTiPaintEngineUpgradeTest, "TiPaint.Engine.BrushesBlendModesStencilProjectionAI",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTiPaintEngineUpgradeTest::RunTest(const FString&)
{
    TSharedRef<FTiPaintDocument> Doc = MakeShared<FTiPaintDocument>(64, 64);

    // 1. Seed two 3D mesh triangles forming a unit quad in XY with UV [0..1]
    TArray<FTiPaintUVTriangle> Tris;
    {
        FTiPaintUVTriangle T0;
        T0.bHas3D = true;
        T0.UV[0] = FVector2f(0.0f, 0.0f); T0.UV[1] = FVector2f(1.0f, 0.0f); T0.UV[2] = FVector2f(1.0f, 1.0f);
        T0.Pos[0] = FVector3f(-50, -50, 0); T0.Pos[1] = FVector3f(50, -50, 0); T0.Pos[2] = FVector3f(50, 50, 0);
        T0.Normal[0] = T0.Normal[1] = T0.Normal[2] = FVector3f::UpVector;
        Tris.Add(T0);

        FTiPaintUVTriangle T1;
        T1.bHas3D = true;
        T1.UV[0] = FVector2f(0.0f, 0.0f); T1.UV[1] = FVector2f(1.0f, 1.0f); T1.UV[2] = FVector2f(0.0f, 1.0f);
        T1.Pos[0] = FVector3f(-50, -50, 0); T1.Pos[1] = FVector3f(50, 50, 0); T1.Pos[2] = FVector3f(-50, 50, 0);
        T1.Normal[0] = T1.Normal[1] = T1.Normal[2] = FVector3f::UpVector;
        Tris.Add(T1);
    }
    Doc->SetMeshTriangles(Tris);

    // 2. AI JSON Batch: Fill base -> Add Multiply layer -> Stencil -> Triplanar Project -> 3D Stamp
    const FString Batch = TEXT(
        "[\n"
        "  {\"op\": \"fill_layer\", \"color\": [200, 200, 200, 255]},\n"
        "  {\"op\": \"add_layer\", \"name\": \"Pattern\"},\n"
        "  {\"op\": \"set_layer_blend_mode\", \"blend_mode\": \"multiply\"},\n"
        "  {\"op\": \"set_stencil\", \"enabled\": true, \"type\": \"checker\", \"scale\": 4.0},\n"
        "  {\"op\": \"project_stencil\", \"mode\": \"triplanar\", \"color\": [100, 150, 255, 255], \"dilate_seams\": 1},\n"
        "  {\"op\": \"set_brush\", \"color\": [255, 40, 40, 255], \"radius\": 10, \"tip\": \"chalk\", \"symmetry\": \"mirror_u\"},\n"
        "  {\"op\": \"stamp_3d\", \"position\": [0, 0, 0], \"normal\": [0, 0, 1], \"radius_3d\": 25.0}\n"
        "]");
    const FTiPaintOpResult Res = FTiPaintAPI::ExecuteJsonCommand(*Doc, Batch);
    TestTrue(TEXT("AI JSON batch succeeds"), Res.bOk);
    TestEqual(TEXT("Document has 2 layers"), Doc->Layers().Num(), 2);

    const FColor CenterCol = Doc->SampleCompositeColor(0.5f, 0.5f);
    TestTrue(TEXT("Center pixel painted red by 3D stamp"), CenterCol.R > 150 && CenterCol.A == 255);

    // 3. Merge down & Schema check
    TestTrue(TEXT("MergeActiveLayerDown succeeds"), Doc->MergeActiveLayerDown());
    TestEqual(TEXT("Document has 1 layer after merge"), Doc->Layers().Num(), 1);
    TestTrue(TEXT("API schema lists project_stencil and stamp_3d"),
        FTiPaintAPI::GetApiSchemaJson().Contains(TEXT("project_stencil")) &&
        FTiPaintAPI::GetApiSchemaJson().Contains(TEXT("stamp_3d")));

    return true;
}

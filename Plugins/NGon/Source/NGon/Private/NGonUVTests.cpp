#include "NGonUVAPI.h"
#include "NGonUVEditor.h"
#include "NGonTopology.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNGonUVDocumentTest, "NGon.UV.DocumentSelectionAndTransforms",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNGonUVDocumentTest::RunTest(const FString&)
{
    FNGonMeshData Data;
    FText Error;
    TestTrue(TEXT("Demo mesh builds"), NGon::MakeDemo(Data, Error));
    if (!Error.IsEmpty()) AddError(Error.ToString());

    FNGonUVDocument Document;
    TestTrue(TEXT("Primary UV overlay loads"), Document.Load(Data, Error));
    TestTrue(TEXT("UV triangles are present"), Document.GetTriangles().Num() > 0);
    TestTrue(TEXT("UV islands are detected"), Document.GetIslandCount() >= 2);
    if (Document.GetTriangles().IsEmpty()) return true;

    const FNGonUVDocument::FTriangle First = Document.GetTriangles()[0];
    const FVector2f Original = Document.GetElementUV(First.Element[0]);
    Document.SetSelectionMode(ENGonUVSelectionMode::Corner);
    Document.SelectAtUV(Original, false, 100.0f);
    TestTrue(TEXT("Corner selection uses overlay element"), Document.GetSelectedElements().Num() > 0);

    Document.TranslateSelected(FVector2f(0.25f, -0.125f));
    if (Document.GetSelectedElements().Num() > 0)
    {
        const int32 SelectedId = *Document.GetSelectedElements().CreateConstIterator();
        TestTrue(TEXT("Move changes selected UV"), Document.GetElementUV(SelectedId).Equals(Original) == false);
    }
    const FVector2f Moved = Document.GetElementUV(First.Element[0]);
    Document.RotateSelected(90.0f);
    TestTrue(TEXT("Rotate preserves a finite UV"), Document.GetElementUV(First.Element[0]).ContainsNaN() == false);
    Document.ScaleSelected(0.5f);
    TestTrue(TEXT("Scale preserves a finite UV"), Document.GetElementUV(First.Element[0]).ContainsNaN() == false);

    Document.SetSelectionMode(ENGonUVSelectionMode::Island);
    Document.SelectAtUV(Moved, false, 0.5f);
    TestTrue(TEXT("Island selection selects overlay elements"), Document.GetSelectedElements().Num() > 0);
    Document.NormalizeSelected();
    TestTrue(TEXT("Normalize keeps bounds valid"), Document.GetBounds().Min.X <= Document.GetBounds().Max.X);

    // NGonUVAPI: JSON batch pack + relax + undo/redo + schema
    const FString BatchJson = TEXT(
        "[\n"
        "  {\"op\": \"select_all\"},\n"
        "  {\"op\": \"relax\", \"iterations\": 2, \"strength\": 0.4, \"pin_border\": true},\n"
        "  {\"op\": \"pack_islands\", \"padding\": 0.02, \"rotate_to_fit\": true},\n"
        "  {\"op\": \"inspect\", \"include_islands\": true}\n"
        "]");
    const FNGonUVOpResult BatchRes = FNGonUVAPI::ExecuteJsonCommand(Document, BatchJson);
    TestTrue(TEXT("UV API JSON batch succeeds"), BatchRes.bOk);
    TestTrue(TEXT("Packed UVs fit within [0,1] bounds"),
        Document.GetBounds().Min.X >= -0.01 && Document.GetBounds().Max.X <= 1.01
        && Document.GetBounds().Min.Y >= -0.01 && Document.GetBounds().Max.Y <= 1.01);
    TestTrue(TEXT("UV API Undo/Redo works"),
        FNGonUVAPI::CanUndo(&Document) && FNGonUVAPI::Undo(Document).bOk && FNGonUVAPI::Redo(Document).bOk);
    TestTrue(TEXT("UV API schema lists pack_islands"), FNGonUVAPI::GetApiSchemaJson().Contains(TEXT("pack_islands")));

    return true;
}
// NGon Unified ABI Sync v5.8.2

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
    return true;
}
// NGon Unified ABI Sync v5.8.2

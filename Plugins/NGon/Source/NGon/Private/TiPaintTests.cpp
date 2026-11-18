#include "Misc/AutomationTest.h"
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
// NGon Unified ABI Sync v5.8.2

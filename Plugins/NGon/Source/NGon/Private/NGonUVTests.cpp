#include "NGonUVAPI.h"
#include "NGonUVEditor.h"
#include "NGonTopology.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
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

    // Regression test: 3D-aware MVC Relax preserves undistorted non-uniform rectangular quads
    // and pulls a perturbed interior UV vertex back toward its exact 3D-proportional position.
    {
        FNGonMeshData GridData;
        GridData.Mesh.EnableAttributes();
        GridData.Mesh.EnableTriangleGroups();
        UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = GridData.Mesh.Attributes()->PrimaryUV();
        const double Xs[3] = { 0.0, 10.0, 35.0 };
        const double Ys[3] = { 0.0, 8.0, 16.0 };
        int32 VIds[9];
        int32 EIds[9];
        for (int32 iy = 0; iy < 3; ++iy)
        {
            for (int32 ix = 0; ix < 3; ++ix)
            {
                const int32 idx = iy * 3 + ix;
                VIds[idx] = GridData.Mesh.AppendVertex(FVector3d(Xs[ix], Ys[iy], 0.0));
                EIds[idx] = UVOverlay->AppendElement(FVector2f(static_cast<float>(Xs[ix] / 40.0), static_cast<float>(Ys[iy] / 40.0)));
            }
        }
        // Valid 2x2 quads with asymmetric diagonals around center vertex 4
        const int32 TrisDef[8][4] = {
            {0, 1, 4, 0}, {0, 4, 3, 0},
            {1, 2, 5, 1}, {1, 5, 4, 1},
            {3, 4, 7, 2}, {3, 7, 6, 2},
            {4, 5, 7, 3}, {5, 8, 7, 3}
        };
        for (const auto& T : TrisDef)
        {
            const int32 Tid = GridData.Mesh.AppendTriangle(VIds[T[0]], VIds[T[1]], VIds[T[2]], T[3]);
            UVOverlay->SetTriangle(Tid, UE::Geometry::FIndex3i(EIds[T[0]], EIds[T[1]], EIds[T[2]]));
        }
        FNGonUVDocument GridDoc;
        FText GridErr;
        if (TestTrue(TEXT("Non-uniform grid UV doc loads"), GridDoc.Load(GridData, GridErr)))
        {
            const FVector2f IdealCenter = GridDoc.GetElementUV(EIds[4]);
            TSet<int32> CenterSel;
            CenterSel.Add(EIds[4]);
            GridDoc.SetSelectedElements(CenterSel, false);

            FNGonUVAPI::RelaxSelected(GridDoc, 8, 0.8f, true);
            const float DriftOnGoodUV = FVector2f::Distance(GridDoc.GetElementUV(EIds[4]), IdealCenter);
            TestTrue(TEXT("Relax keeps already-good non-uniform quad UV undistorted"), DriftOnGoodUV < 1e-4f);

            // Perturb center vertex and verify Relax pulls it back toward IdealCenter
            TMap<int32, FVector2f> Perturbed = GridDoc.GetElementUVs();
            Perturbed[EIds[4]] = IdealCenter + FVector2f(0.12f, -0.06f);
            GridDoc.SetAllElementUVs(Perturbed);
            const float DistBefore = FVector2f::Distance(GridDoc.GetElementUV(EIds[4]), IdealCenter);
            FNGonUVAPI::RelaxSelected(GridDoc, 8, 0.8f, true);
            const float DistAfter = FVector2f::Distance(GridDoc.GetElementUV(EIds[4]), IdealCenter);
            TestTrue(TEXT("Relax pulls distorted UV vertex back toward 3D-proportional position"), DistAfter < DistBefore * 0.1f);
        }
    }

    return true;
}
// NGon Unified ABI Sync v5.8.2

#if WITH_DEV_AUTOMATION_TESTS

#include "NGonMeshFiles.h"
#include "NGonPicking.h"
#include "NGonPreviewScene.h"
#include "NGonTopology.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UDynamicMesh.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonDemoTopologyTest, "NGon.TopologyAndPicking",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonDemoTopologyTest::RunTest(const FString& Parameters)
{
    FNGonMeshData Data;
    FText Error;
    if (!NGon::MakeDemo(Data, Error))
    {
        AddError(Error.ToString());
        return false;
    }

    const FNGonTopology& Topology = Data.Topology;
    TestEqual(TEXT("Authored polygons"), Topology.Faces.Num(), 3);
    TestEqual(TEXT("Surface triangles"), Topology.TriangleCount, 8);
    TestEqual(TEXT("Authored edges, shared edges counted once"), Topology.Edges.Num(), 12);
    TestEqual(TEXT("Logical vertices, UV seam counted once"), Topology.RestPositions.Num(), 10);
    TestEqual(TEXT("Largest polygon"), Topology.MaxFaceCorners, 6);

    const auto& Mesh = Data.Mesh;
    const auto* UVs = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryUV() : nullptr;
    bool bSawSharedUVSeam = false;
    for (const int32 EdgeId : Mesh.EdgeIndicesItr())
    {
        const auto EdgeTriangles = Mesh.GetEdgeT(EdgeId);
        if (EdgeTriangles.B != INDEX_NONE && UVs && UVs->IsSeamEdge(EdgeId, nullptr))
        {
            bSawSharedUVSeam = true;
            break;
        }
    }
    TestTrue(TEXT("Demo contains a real shared-edge UV seam"), bSawSharedUVSeam);

    FNGonPicker Picker;
    Picker.SetMesh(&Data.Mesh);
    const FVector Down(0, 0, -1);
    for (const auto& Face : Topology.Faces)
    {
        for (const int32 TriangleId : Face.Value.Triangles)
        {
            const auto Vertices = Mesh.GetTriangle(TriangleId);
            const FVector Center = (Mesh.GetVertex(Vertices.A) + Mesh.GetVertex(Vertices.B)
                + Mesh.GetVertex(Vertices.C)) / 3.0;
            TestEqual(TEXT("Each constituent triangle selects its source polygon"),
                Picker.PickFace(Center + FVector(0, 0, 50), Down), Face.Key);
        }
    }
    TestEqual(TEXT("Ray through the concave notch misses"),
        Picker.PickFace(FVector(85, 48, 50), Down), INDEX_NONE);

    for (const int32 Vertex : Data.Mesh.VertexIndicesItr())
    {
        Data.Mesh.SetVertex(Vertex, Data.Mesh.GetVertex(Vertex) + FVector(0, 0, 100));
    }
    Picker.PositionsChanged();
    TestEqual(TEXT("Picking uses deformed positions"),
        Picker.PickFace(FVector(-100, 0, 150), Down), 0);
    TestEqual(TEXT("Ray from below the moved mesh misses"),
        Picker.PickFace(FVector(-100, 0, 50), Down), INDEX_NONE);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonPreviewMaterialTest, "NGon.Preview.PaintAndSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonPreviewMaterialTest::RunTest(const FString& Parameters)
{
    FNGonMeshData Data;
    FText Error;
    if (!TestTrue(TEXT("Build demo mesh"), NGon::MakeDemo(Data, Error)))
    {
        AddError(Error.ToString());
        return false;
    }

    FNGonPreviewScene Scene;
    Scene.SetMesh(MoveTemp(Data));
    Scene.SelectElement(ENGonElement::Face, 0);
    FNGonSoftSelectionSettings Settings;
    Settings.bEnabled = true;
    Scene.SetSoftSelection(Settings);
    Scene.SetHeatmapVisible(true);

    const UMaterialInterface* Material = Scene.Surface->GetMaterial(0);
    const auto& Mesh = Scene.Surface->GetDynamicMesh()->GetMeshRef();
    const auto* Colors = Mesh.Attributes()->PrimaryColors();
    const int32 Triangle = Scene.GetTopology().Faces.FindChecked(0).Triangles[0];
    const int32 ColorElement = Colors->GetTriangle(Triangle).A;
    const FVector4f SelectionAndHeat = Colors->GetElement(ColorElement);
    TestTrue(TEXT("Selected face and heatmap channels set"), SelectionAndHeat.Y == 1.0f && SelectionAndHeat.Z == 1.0f);

    TArray<FColor> Pixels;
    Pixels.Init(FColor(0, 0, 255, 255), 4);
    Scene.SetPaintTexture(Pixels, 2, 2);
    TestTrue(TEXT("Preview paint texture uploaded"), Scene.PaintTexture.IsValid());
    TestTrue(TEXT("Paint upload uses the existing surface material"), Scene.Surface->GetMaterial(0) == Material);
    TestTrue(TEXT("Paint preserves face selection and heatmap channels"), Colors->GetElement(ColorElement) == SelectionAndHeat);
    Scene.ClearPaintTexture();
    TestFalse(TEXT("Clearing paint releases the paint texture"), Scene.PaintTexture.IsValid());
    TestTrue(TEXT("Clearing paint preserves the surface material"), Scene.Surface->GetMaterial(0) == Material);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonDefaultFbxTest, "NGon.DefaultFbx",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonDefaultFbxTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("NGonMeshes"), TEXT(""));
    IFileManager& Files = IFileManager::Get();
    if (!TestTrue(TEXT("Create mesh fixture directory"), Files.MakeDirectory(*Directory, true)))
    {
        return false;
    }
    ON_SCOPE_EXIT { Files.DeleteDirectory(*Directory, false, true); };

    FString Picked;
    FText Error;
    TestFalse(TEXT("An empty folder has no default"), NGon::FindDefaultFbx(Directory, Picked, Error));
    TestTrue(TEXT("Explain absent default file"), !Error.IsEmpty());

    const FString Only = FPaths::Combine(Directory, TEXT("Character.FBX"));
    const FString Other = FPaths::Combine(Directory, TEXT("Chair.fbx"));
    const FString Preferred = FPaths::Combine(Directory, TEXT("Default.fbx"));
    if (!TestTrue(TEXT("Write a single FBX and a non-FBX file"),
        FFileHelper::SaveStringToFile(TEXT("fixture"), *Only)
        && FFileHelper::SaveStringToFile(TEXT("fixture"), *FPaths::Combine(Directory, TEXT("notes.txt")))))
    {
        return false;
    }
    TestTrue(TEXT("Accept the only FBX regardless of filename or extension case"),
        NGon::FindDefaultFbx(Directory, Picked, Error));
    TestEqual(TEXT("Select the single FBX"), Picked, Only);

    if (!TestTrue(TEXT("Write a second FBX"), FFileHelper::SaveStringToFile(TEXT("fixture"), *Other)))
    {
        return false;
    }
    TestFalse(TEXT("Two unnamed FBXs have no automatic default"), NGon::FindDefaultFbx(Directory, Picked, Error));
    TestTrue(TEXT("Clear a previous path when no default can be chosen"), Picked.IsEmpty());

    if (!TestTrue(TEXT("Write Default.fbx"), FFileHelper::SaveStringToFile(TEXT("fixture"), *Preferred)))
    {
        return false;
    }
    TestTrue(TEXT("Prefer Default.fbx in a folder with multiple FBXs"), NGon::FindDefaultFbx(Directory, Picked, Error));
    TestEqual(TEXT("Select the explicitly named default"), Picked, Preferred);
    return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

#include "NGonFbx.h"
#include "NGonPicking.h"
#include "NGonMeshFiles.h"
#include "NGonTopology.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END

namespace
{
struct FFbxTestManagerDeleter
{
    void operator()(FbxManager* Manager) const { Manager->Destroy(); }
};

bool WriteFbxFixture(const FString& Path, bool bUseCornerColors)
{
    TUniquePtr<FbxManager, FFbxTestManagerDeleter> Manager(FbxManager::Create());
    if (!Manager)
    {
        return false;
    }

    FbxIOSettings* Settings = FbxIOSettings::Create(Manager.Get(), IOSROOT);
    Manager->SetIOSettings(Settings);
    FbxScene* Scene = FbxScene::Create(Manager.Get(), "NGonTest");
    Scene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem::Max);
    Scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);
    FbxNode* Node = FbxNode::Create(Scene, "Mesh");
    Node->LclTranslation.Set(FbxDouble3(2.0, 3.0, 0.5));
    Node->SetGeometricTranslation(FbxNode::eSourcePivot, FbxVector4(0.25, -0.5, 0.5));
    Scene->GetRootNode()->AddChild(Node);

    FbxMesh* Mesh = FbxMesh::Create(Scene, "QuadAndConcaveFace");
    Node->SetNodeAttribute(Mesh);
    const FbxVector4 Points[] = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}, {2, 1, 0}, {1.5, 1, 0}, {1.5, 0.4, 0}
    };
    const int32 PointCount = UE_ARRAY_COUNT(Points);
    Mesh->InitControlPoints(PointCount);
    for (int32 Index = 0; Index < PointCount; ++Index)
    {
        Mesh->GetControlPoints()[Index] = Points[Index];
    }
    Mesh->BeginPolygon(-1, -1, -1, false);
    for (const int32 Index : {0, 1, 2, 3})
    {
        Mesh->AddPolygon(Index);
    }
    Mesh->EndPolygon();
    Mesh->BeginPolygon(-1, -1, -1, false);
    for (const int32 Index : {1, 4, 5, 6, 7, 2})
    {
        Mesh->AddPolygon(Index);
    }
    Mesh->EndPolygon();

    FbxGeometryElementVertexColor* Colors = Mesh->CreateElementVertexColor();
    if (bUseCornerColors)
    {
        Colors->SetMappingMode(FbxGeometryElement::eByPolygonVertex);
        Colors->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
        for (const double Red : {0.0, 0.2, 0.4, 0.8, 0.1, 1.0})
        {
            Colors->GetDirectArray().Add(FbxColor(Red, 0.0, 0.0, 1.0));
        }
        for (const int32 Index : {0, 1, 2, 0, 3, 4, 5, 0, 0, 0})
        {
            Colors->GetIndexArray().Add(Index);
        }
    }
    else
    {
        Colors->SetMappingMode(FbxGeometryElement::eByControlPoint);
        Colors->SetReferenceMode(FbxGeometryElement::eDirect);
        for (int32 Index = 0; Index < Mesh->GetControlPointsCount(); ++Index)
        {
            Colors->GetDirectArray().Add(FbxColor(0.0, 0.0, 0.0, 1.0));
        }
    }

    FbxExporter* Exporter = FbxExporter::Create(Manager.Get(), "");
    const int32 Writer = Manager->GetIOPluginRegistry()->GetNativeWriterFormat();
    return Exporter->Initialize(TCHAR_TO_UTF8(*Path), Writer, Settings) && Exporter->Export(Scene);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonRawFbxTest, "NGon.RawFbx",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonRawFbxTest::RunTest(const FString& Parameters)
{
    const FString Path = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("NGonFixture"), TEXT(".fbx"));
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); };

    FText Error;
    FNGonMeshData Data;
    if (!TestTrue(TEXT("Write indexed-corner FBX"), WriteFbxFixture(Path, true)))
    {
        return false;
    }
    if (!NGon::LoadFbx(Path, Data, Error))
    {
        AddError(Error.ToString());
        return false;
    }

    const FNGonTopology& Topology = Data.Topology;
    TestEqual(TEXT("Two authored polygons"), Topology.Faces.Num(), 2);
    TestEqual(TEXT("Quad and six-gon produce six triangles"), Topology.TriangleCount, 6);
    TestEqual(TEXT("Shared edge counted once"), Topology.Edges.Num(), 9);
    TestEqual(TEXT("Distinct control points"), Topology.RestPositions.Num(), 8);
    TestEqual(TEXT("The six-gon is preserved"), Topology.MaxFaceCorners, 6);
    TestTrue(TEXT("Apply scene units and node plus geometric offsets"),
        Topology.RestBounds.Min.Equals(FVector(225, -350, 100), 0.01)
        && Topology.RestBounds.Max.Equals(FVector(425, -250, 100), 0.01));
    const int32 LogicalVertex = Topology.SourceVertexIds.Find(1);
    if (TestTrue(TEXT("Imported color mapped back to its source vertex"),
        Data.ImportedHeatSamples.IsValidIndex(LogicalVertex)))
    {
        TestTrue(TEXT("Average 0.2 and 0.8 across FBX polygon corners"),
            FMath::IsNearlyEqual(Data.ImportedHeatSamples[LogicalVertex], 0.5f));
    }

    FNGonPicker Picker;
    Picker.SetMesh(&Data.Mesh);
    TestEqual(TEXT("Ray through the FBX notch misses"),
        Picker.PickFace(FVector(355, -338, 150), FVector(0, 0, -1)), INDEX_NONE);
    TestEqual(TEXT("Ray below the notch selects the six-gon"),
        Picker.PickFace(FVector(385, -260, 150), FVector(0, 0, -1)), 1);
    Picker.SetMesh(nullptr);

    if (!TestTrue(TEXT("Write direct control-point FBX with zero weights"), WriteFbxFixture(Path, false)))
    {
        return false;
    }
    if (!NGon::LoadFbx(Path, Data, Error))
    {
        AddError(Error.ToString());
        return false;
    }
    TestEqual(TEXT("Every logical vertex has an imported weight"), Data.ImportedHeatSamples.Num(), 8);
    for (const float Sample : Data.ImportedHeatSamples)
    {
        TestEqual(TEXT("A zero FBX red value remains zero"), Sample, 0.0f);
    }

    IFileManager::Get().Delete(*Path);
    TestFalse(TEXT("A missing FBX fails"), NGon::LoadFbx(Path, Data, Error));
    TestEqual(TEXT("A failed load preserves the prior mesh"), Data.Topology.TriangleCount, 6);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonProjectFbxTest, "NGon.ProjectFbx",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonProjectFbxTest::RunTest(const FString& Parameters)
{
    FString Filename;
    FText Error;
    if (!TestTrue(TEXT("Project mesh is selected as the default"),
        NGon::FindDefaultFbx(NGon::GetMeshesDirectory(), Filename, Error)))
    {
        AddError(Error.ToString());
        return false;
    }

    FNGonMeshData Data;
    if (!TestTrue(TEXT("Project FBX loads"), NGon::LoadFbx(Filename, Data, Error)))
    {
        AddError(Error.ToString());
        return false;
    }

    TestEqual(TEXT("Default FBX filename"), FPaths::GetCleanFilename(Filename), TEXT("v1an6m.fbx"));
    TestTrue(TEXT("Project FBX contains authored polygons"), Data.Topology.Faces.Num() > 0);
    TestTrue(TEXT("Project FBX produces preview triangles"), Data.Topology.TriangleCount > 0);
    return true;
}

#endif

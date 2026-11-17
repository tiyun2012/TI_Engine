#if WITH_DEV_AUTOMATION_TESTS

#include "NGonMeshFiles.h"
#include "NGonPreviewScene.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonGizmoTransformInteractionTest, "NGon.Gizmo.TransformInteraction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonGizmoTransformInteractionTest::RunTest(const FString& Parameters)
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

    const FTransform Initial = Scene.GetSelectionGizmoTransform();
    const FVector InitialCenter = Initial.GetTranslation();
    TestTrue(TEXT("Begin transform for selected face"),
        Scene.BeginSoftTransform(Initial, ENGonSoftTransformMode::Fixed));
    TestTrue(TEXT("Transform session is active"), Scene.IsSoftTransformActive());

    const FVector Delta = Initial.GetUnitAxis(EAxis::X) * 10.0;
    const FTransform Moved(Initial.GetRotation(), InitialCenter + Delta);
    TestTrue(TEXT("Apply gizmo translation"), Scene.ApplyGizmoTransform(Moved));
    const FVector MovedCenter = Scene.GetSelectionGizmoTransform().GetTranslation();
    TestTrue(TEXT("Selection follows gizmo translation"),
        !MovedCenter.Equals(InitialCenter, 0.01) &&
        FMath::IsNearlyEqual(FVector::Distance(MovedCenter, InitialCenter), Delta.Size(), 0.01));

    Scene.EndSoftTransform();
    TestFalse(TEXT("End transform closes the session"), Scene.IsSoftTransformActive());

    const FTransform BeforeCancel = Scene.GetSelectionGizmoTransform();
    TestTrue(TEXT("Begin second transform"),
        Scene.BeginSoftTransform(BeforeCancel, ENGonSoftTransformMode::Fixed));
    TestTrue(TEXT("Apply second gizmo translation"),
        Scene.ApplyGizmoTransform(FTransform(BeforeCancel.GetRotation(), BeforeCancel.GetTranslation() + Delta)));
    Scene.CancelSoftTransform();
    TestFalse(TEXT("Cancel closes the session"), Scene.IsSoftTransformActive());
    const FVector RestoredCenter = Scene.GetSelectionGizmoTransform().GetTranslation();
    TestTrue(TEXT("Cancel restores the pre-gesture mesh"),
        RestoredCenter.Equals(BeforeCancel.GetTranslation(), 0.01));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonGizmoComponentFrameTest, "NGon.Gizmo.ComponentFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonGizmoComponentFrameTest::RunTest(const FString& Parameters)
{
    FNGonMeshData Data;
    FText Error;
    const TArray<FVector> Positions = {
        FVector(0, 0, 0), FVector(0, 100, 0), FVector(0, 0, 100)};
    const TArray<TArray<int32>> Faces = {{0, 1, 2}};
    if (!TestTrue(TEXT("Build oriented component mesh"), NGon::BuildPolygonMesh(Positions, Faces, Data, Error)))
    {
        AddError(Error.ToString());
        return false;
    }

    FNGonPreviewScene Scene;
    Scene.SetMesh(MoveTemp(Data));
    Scene.SelectElement(ENGonElement::Face, 0);
    const FTransform FaceFrame = Scene.GetSelectionGizmoTransform();
    TestTrue(TEXT("Face pivot is polygon centroid"),
        FaceFrame.GetTranslation().Equals(FVector(0, 100.0 / 3.0, 100.0 / 3.0), 0.01));
    TestTrue(TEXT("Face normal aligns gizmo Z"),
        FaceFrame.GetUnitAxis(EAxis::Z).Equals(FVector(1, 0, 0), 0.01));
    TestTrue(TEXT("Face perimeter tangent aligns gizmo X"),
        FaceFrame.GetUnitAxis(EAxis::X).Equals(FVector(0, 1, 0), 0.01));

    Scene.SelectElement(ENGonElement::Edge, 0);
    const FTransform EdgeFrame = Scene.GetSelectionGizmoTransform();
    TestTrue(TEXT("Edge pivot is midpoint"),
        EdgeFrame.GetTranslation().Equals(FVector(0, 50, 0), 0.01));
    TestTrue(TEXT("Edge tangent aligns gizmo X"),
        EdgeFrame.GetUnitAxis(EAxis::X).Equals(FVector(0, 1, 0), 0.01));
    TestTrue(TEXT("Component frame is orthonormal"),
        FMath::IsNearlyEqual(EdgeFrame.GetUnitAxis(EAxis::X).Size(), 1.0, 0.001) &&
        FMath::IsNearlyEqual(EdgeFrame.GetUnitAxis(EAxis::Y).Size(), 1.0, 0.001) &&
        FMath::IsNearlyEqual(EdgeFrame.GetUnitAxis(EAxis::Z).Size(), 1.0, 0.001));

    Scene.SetGizmoFramePolicy(ENGonGizmoFramePolicy::ActiveElement);
    Scene.SelectElement(ENGonElement::Vertex, 2);
    const FTransform VertexFrame = Scene.GetSelectionGizmoTransform();
    TestTrue(TEXT("Active vertex pivot is vertex position"),
        VertexFrame.GetTranslation().Equals(FVector(0, 0, 100), 0.01));
    TestTrue(TEXT("Vertex frame has a usable normal"),
        VertexFrame.GetUnitAxis(EAxis::Z).IsNormalized());
    return true;
}

#endif

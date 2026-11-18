#if WITH_DEV_AUTOMATION_TESTS

#include "NGonMeshFiles.h"
#include "NGonPreviewScene.h"
#include "NGonSceneSkeleton.h"
#include "NGonViewportGizmo.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonViewportGizmoModesTest, "NGon.Gizmo.ViewportModes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonViewportGizmoModesTest::RunTest(const FString& Parameters)
{
    FNGonViewportGizmo Gizmo;
    const FVector2D Center(300.0, 220.0);
    const auto Project = [&Center](const FVector& Point, FVector2D& Pixel)
    {
        Pixel = Center + FVector2D(Point.X, -Point.Y);
        return true;
    };
    auto Build = [&Gizmo, &Project](ENGonGizmoMode Mode)
    {
        Gizmo.Build(
            FTransform::Identity,
            Mode,
            1.0,
            1.0,
            FVector(0.0, 0.0, 500.0),
            -FVector::UpVector,
            FVector::ForwardVector,
            FVector::RightVector,
            true,
            Project);
    };

    Build(ENGonGizmoMode::None);
    TestFalse(TEXT("Q leaves no selectable handles"), Gizmo.BeginDrag(Center));
    Build(ENGonGizmoMode::Translate);
    const FVector2D MoveHandle = Center + FVector2D(60, 0);
    TestFalse(TEXT("Picking away from the projected gizmo misses"), Gizmo.BeginDrag(Center + FVector2D(120, 110)));
    if (TestTrue(TEXT("W picks the drawn X arrow"), Gizmo.BeginDrag(MoveHandle)))
    {
        const FTransform Moved = Gizmo.DragTo(MoveHandle + FVector2D(20, -12));
        TestTrue(TEXT("Axis drag only translates along X"), Moved.GetTranslation().Equals(FVector(20, 0, 0), 0.01));
        Gizmo.EndDrag();
    }
    const FVector2D PlaneHandle = Center + FVector2D(24, -24);
    if (TestTrue(TEXT("W picks the XY plane"), Gizmo.BeginDrag(PlaneHandle)))
    {
        const FTransform Moved = Gizmo.DragTo(PlaneHandle + FVector2D(7, -9));
        TestTrue(TEXT("Plane drag translates in X and Y"), Moved.GetTranslation().Equals(FVector(7, 9, 0), 0.01));
        Gizmo.EndDrag();
    }

    Build(ENGonGizmoMode::Rotate);
    const FVector2D Ring = Center + FVector2D(61, -61);
    if (TestTrue(TEXT("E picks the drawn Z rotation ring"), Gizmo.BeginDrag(Ring)))
    {
        const FTransform Rotated = Gizmo.DragTo(Ring + FVector2D(-24, -24));
        TestTrue(TEXT("Ring drag rotates around Z"), Rotated.GetRotation().RotateVector(FVector::ForwardVector).Y > 0.2
            && Rotated.GetRotation().RotateVector(FVector::UpVector).Equals(FVector::UpVector));
        Gizmo.EndDrag();
    }

    Build(ENGonGizmoMode::Scale);
    const FVector2D ScaleHandle = Center + FVector2D(82, 0);
    if (TestTrue(TEXT("R picks the drawn X scale box"), Gizmo.BeginDrag(ScaleHandle)))
    {
        TestTrue(TEXT("Scale drag changes only X"),
            Gizmo.DragTo(ScaleHandle + FVector2D(43, 0)).GetScale3D().Equals(FVector(1.5, 1, 1), 0.01));
        Gizmo.EndDrag();
    }
    if (TestTrue(TEXT("R also provides uniform scale"), Gizmo.BeginDrag(Center)))
    {
        const FVector Scale = Gizmo.DragTo(Center + FVector2D(45, -45)).GetScale3D();
        TestTrue(TEXT("Uniform scale changes all axes equally"), Scale.X > 1.5 && Scale.Equals(FVector(Scale.X)));
        Gizmo.EndDrag();
    }
    Build(ENGonGizmoMode::Combined);
    TestTrue(TEXT("Combined preserves a move handle"), Gizmo.BeginDrag(MoveHandle + FVector2D(18, 0)));
    Gizmo.EndDrag();
    TestTrue(TEXT("Combined preserves the rotation ring"), Gizmo.BeginDrag(Center + FVector2D(47, -47)));
    Gizmo.EndDrag();

    FNGonMeshData Data;
    FText Error;
    if (!TestTrue(TEXT("Build demo mesh"), NGon::MakeDemo(Data, Error)))
    {
        AddError(Error.ToString());
        return false;
    }
    FNGonPreviewScene Scene;
    Scene.SetMesh(MoveTemp(Data));
    TestFalse(TEXT("No gizmo target without a selection"), Scene.HasGizmoTarget());
    TArray<NGon::FNGonSceneBoneInit> Bones;
    NGon::FNGonSceneBoneInit& Bone = Bones.AddDefaulted_GetRef();
    Bone.BoneIndex = 0;
    Bone.Name = TEXT("TestJoint");
    Bone.RefLocalTransform = FTransform(FVector(10.0, 20.0, 30.0));
    Bone.RefComponentTransform = Bone.RefLocalTransform;
    NGon::SetSceneSkeleton(&Scene, Bones, {});
    NGon::SelectSceneBone(&Scene, 0, true);
    TestTrue(TEXT("A selected joint inside the mesh provides a gizmo target"), Scene.HasGizmoTarget());
    TestTrue(TEXT("The gizmo frame uses the joint position"),
        Scene.GetGizmoDisplayTransform().GetLocation().Equals(Bone.RefComponentTransform.GetLocation(), 0.01));
    Scene.SetGizmoMode(ENGonGizmoMode::None);
    TestFalse(TEXT("Q hides the selected joint's gizmo"), Scene.HasGizmoTarget());
    Scene.SetGizmoMode(ENGonGizmoMode::Rotate);
    TestTrue(TEXT("E shows the gizmo for the existing selected joint"), Scene.HasGizmoTarget());
    NGon::SetBoneGizmoActive(&Scene, false);
    TestFalse(TEXT("Switching to empty component selection hides gizmo"), Scene.HasGizmoTarget());
    return true;
}

#endif
// NGon Unified ABI Sync v5.8.2

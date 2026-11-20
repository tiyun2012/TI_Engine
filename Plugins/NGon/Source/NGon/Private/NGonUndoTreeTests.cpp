#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "NGonUndoTree.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNGonUndoTreeTest, "NGon.UndoTree.SelectiveBranchingAndEpochSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNGonUndoTreeTest::RunTest(const FString&)
{
    TSharedRef<FNGonUndoSession> Session = MakeShared<FNGonUndoSession>(TEXT("Test Window"));
    TestEqual(TEXT("Default MaxUndoSteps is 20"), Session->GetPolicy().MaxUndoSteps, 20);

    int32 MeshVal = 0;
    int32 SkelVal = 0;
    int32 UVVal = 0;
    int32 PaintVal = 0;

    auto MakeIntPayload = [](int32* Target, int32 Before, int32 After) -> TSharedPtr<INGonUndoPayload>
    {
        return MakeShared<FNGonLambdaUndoPayload>(
            [Target, Before]() { *Target = Before; return true; },
            [Target, After]() { *Target = After; return true; },
            1024);
    };

    // 1. Push structural Mesh Bevel -> UV Pack -> Paint 3D Projection -> Skeleton Pose
    MeshVal = 1;
    const uint64 MeshBevelId = Session->PushEdit(ENGonUndoContextId::Mesh, TEXT("Bevel Edges"), ENGonUndoEditKind::Structural, MakeIntPayload(&MeshVal, 0, 1));
    UVVal = 1;
    const uint64 UVPackId = Session->PushEdit(ENGonUndoContextId::UV, TEXT("Pack UVs"), ENGonUndoEditKind::Structural, MakeIntPayload(&UVVal, 0, 1));
    PaintVal = 1;
    const uint64 PaintProjId = Session->PushEdit(ENGonUndoContextId::Paint, TEXT("Triplanar Project"), ENGonUndoEditKind::Projection, MakeIntPayload(&PaintVal, 0, 1));
    SkelVal = 1;
    Session->PushEdit(ENGonUndoContextId::Skeleton, TEXT("Rotate Jaw"), ENGonUndoEditKind::Value, MakeIntPayload(&SkelVal, 0, 1));

    // 2. Check ONLY Mesh -> verify Auto-Link automatically links UV and Paint, leaving Skeleton frozen!
    Session->SetAllContextsChecked(false);
    Session->SetContextUserChecked(ENGonUndoContextId::Mesh, true);
    TMap<ENGonUndoContextId, ENGonUndoSelectState> Eff;
    TMap<ENGonUndoContextId, FString> Reasons;
    Session->ComputeEffectiveSelection(Eff, Reasons);
    TestEqual(TEXT("Mesh is UserChecked"), Eff.FindRef(ENGonUndoContextId::Mesh), ENGonUndoSelectState::UserChecked);
    TestEqual(TEXT("UV is AutoLinked"), Eff.FindRef(ENGonUndoContextId::UV), ENGonUndoSelectState::AutoLinked);
    TestEqual(TEXT("Paint is AutoLinked"), Eff.FindRef(ENGonUndoContextId::Paint), ENGonUndoSelectState::AutoLinked);
    TestEqual(TEXT("Skeleton stays None (Frozen)"), Eff.FindRef(ENGonUndoContextId::Skeleton), ENGonUndoSelectState::None);

    // 3. UndoChecked steps Paint, UV, Mesh while preserving Skeleton!
    TestTrue(TEXT("UndoChecked succeeds"), Session->UndoChecked());
    TestEqual(TEXT("Mesh reverted to 0"), MeshVal, 0);
    TestEqual(TEXT("UV reverted to 0"), UVVal, 0);
    TestEqual(TEXT("Paint reverted to 0"), PaintVal, 0);
    TestEqual(TEXT("Skeleton preserved at 1"), SkelVal, 1);

    // 4. Epoch lock check: UVPackId is locked while Mesh is at TopoEpoch 1
    TestTrue(TEXT("UVPackId is Epoch-Locked when Mesh is undone"), Session->IsNodeEpochLocked(ENGonUndoContextId::UV, UVPackId));
    TestFalse(TEXT("JumpToNode rejects Epoch-Locked UV node"), Session->JumpToNode(ENGonUndoContextId::UV, UVPackId));

    // 5. Redo Mesh -> unlocks UV & Paint!
    TestTrue(TEXT("RedoChecked restores Mesh+UV+Paint"), Session->RedoChecked());
    TestEqual(TEXT("Mesh restored to 1"), MeshVal, 1);
    TestFalse(TEXT("UVPackId unlocked after Mesh Redo"), Session->IsNodeEpochLocked(ENGonUndoContextId::UV, UVPackId));
    TestTrue(TEXT("JumpToNode succeeds on unlocked Paint node"), Session->JumpToNode(ENGonUndoContextId::Paint, PaintProjId));

    // 6. Branching test: Undo Mesh -> push Alt edit -> both sibling branches exist
    TestTrue(TEXT("UndoContext Mesh"), Session->UndoContext(ENGonUndoContextId::Mesh));
    MeshVal = 2;
    const uint64 AltMeshId = Session->PushEdit(ENGonUndoContextId::Mesh, TEXT("Extrude Alt"), ENGonUndoEditKind::Value, MakeIntPayload(&MeshVal, 0, 2));
    TestTrue(TEXT("Jump back to original Bevel branch"), Session->JumpToNode(ENGonUndoContextId::Mesh, MeshBevelId));
    TestEqual(TEXT("Mesh value switched to original branch"), MeshVal, 1);
    TestTrue(TEXT("Jump back to Alt branch"), Session->JumpToNode(ENGonUndoContextId::Mesh, AltMeshId));
    TestEqual(TEXT("Mesh value switched to Alt branch"), MeshVal, 2);

    // 7. MaxUndoSteps limit pruning test
    Session->SetMaxUndoSteps(3);
    for (int32 i = 0; i < 8; ++i)
    {
        SkelVal = i + 2;
        Session->PushEdit(ENGonUndoContextId::Skeleton, FString::Printf(TEXT("Pose %d"), i), ENGonUndoEditKind::Value, MakeIntPayload(&SkelVal, i + 1, i + 2));
    }
    const FNGonUndoContext* SkelCtx = Session->GetContext(ENGonUndoContextId::Skeleton);
    TestTrue(TEXT("Skeleton node count bounded by MaxUndoSteps + 1"), SkelCtx && SkelCtx->Nodes.Num() <= 4);

    return true;
}

#endif

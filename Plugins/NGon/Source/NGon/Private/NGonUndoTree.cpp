#include "NGonUndoTree.h"

#include "Algo/Reverse.h"
#include "HAL/PlatformTime.h"

namespace
{
TMap<const void*, TWeakPtr<FNGonUndoSession>> GUndoSessionRegistry;
TWeakPtr<FNGonUndoSession> GActiveUndoSession;
}

int32 FNGonUndoContext::GetDepthOfActive() const
{
    int32 Depth = 0;
    uint64 Cur = ActiveNodeId;
    int32 Guard = 0;
    while (Cur != 0 && Guard < 2048)
    {
        const FNGonUndoNode* Node = Nodes.Find(Cur);
        if (!Node || Node->ParentId == 0) break;
        ++Depth;
        Cur = Node->ParentId;
        ++Guard;
    }
    return Depth;
}

SIZE_T FNGonUndoContext::GetTotalBytes() const
{
    SIZE_T Sum = 0;
    for (const TPair<uint64, FNGonUndoNode>& KV : Nodes)
    {
        Sum += KV.Value.CachedBytes;
    }
    return Sum;
}

FNGonUndoSession::FNGonUndoSession(const FString& InSessionName)
    : SessionName(InSessionName)
{
    ResetSession(InSessionName);
}

void FNGonUndoSession::InitContextRoot(
    ENGonUndoContextId ContextId,
    const FString& Name,
    const TArray<ENGonUndoContextId>& Deps,
    bool bDefaultChecked,
    const FString& RootLabel)
{
    FNGonUndoContext Ctx;
    Ctx.Id = ContextId;
    Ctx.DisplayName = Name;
    Ctx.DependsOn = Deps;
    Ctx.bUserChecked = bDefaultChecked;

    FNGonUndoNode Root;
    Root.NodeId = NextNodeId++;
    Root.ParentId = 0;
    Root.Context = ContextId;
    Root.EditKind = ENGonUndoEditKind::Base;
    Root.Label = RootLabel;
    Root.TimestampSec = FPlatformTime::Seconds();
    Root.RequiredEpochs = FNGonEpochVector{ 1, 1, 1 };
    Root.ProducedEpochs = FNGonEpochVector{ 1, 1, 1 };
    Root.CachedBytes = 0;

    Ctx.RootNodeId = Root.NodeId;
    Ctx.ActiveNodeId = Root.NodeId;
    Ctx.Nodes.Add(Root.NodeId, MoveTemp(Root));
    Contexts.Add(ContextId, MoveTemp(Ctx));
}

void FNGonUndoSession::ResetSession(const FString& NewSessionName)
{
    if (!NewSessionName.IsEmpty())
    {
        SessionName = NewSessionName;
    }
    Contexts.Reset();
    NextNodeId = 1;
    NextTopoEpoch = 2;
    NextUVEpoch = 2;
    NextRigEpoch = 2;

    InitContextRoot(ENGonUndoContextId::Mesh, TEXT("Mesh Topology"), {}, true, TEXT("Base Mesh"));
    InitContextRoot(ENGonUndoContextId::Skeleton, TEXT("Skeleton Rig"), {}, false, TEXT("Bind Pose"));
    InitContextRoot(ENGonUndoContextId::UV, TEXT("UV Editor"), { ENGonUndoContextId::Mesh }, false, TEXT("Base UVs"));
    InitContextRoot(ENGonUndoContextId::Paint, TEXT("TiPaint Canvas"), { ENGonUndoContextId::UV }, false, TEXT("Base Canvas"));
    NotifyChanged();
}

void FNGonUndoSession::SetMaxUndoSteps(int32 NewMaxSteps)
{
    const int32 Clamped = FMath::Clamp(NewMaxSteps, 1, 256);
    if (Policy.MaxUndoSteps == Clamped) return;
    Policy.MaxUndoSteps = Clamped;
    EnforcePolicy();
    NotifyChanged();
}

void FNGonUndoSession::SetAutoLinkDependencies(bool bEnable)
{
    if (Policy.bAutoLinkDependencies == bEnable) return;
    Policy.bAutoLinkDependencies = bEnable;
    NotifyChanged();
}

void FNGonUndoSession::SetMaxMemoryBytes(SIZE_T NewMaxBytes)
{
    Policy.MaxMemoryBytes = FMath::Max<SIZE_T>(4ull * 1024ull * 1024ull, NewMaxBytes);
    EnforcePolicy();
    NotifyChanged();
}

FNGonEpochVector FNGonUndoSession::GetCurrentEpochs() const
{
    FNGonEpochVector Out;
    if (const FNGonUndoContext* MeshCtx = Contexts.Find(ENGonUndoContextId::Mesh))
    {
        if (const FNGonUndoNode* N = MeshCtx->FindActiveNode()) Out.TopoEpoch = N->ProducedEpochs.TopoEpoch;
    }
    if (const FNGonUndoContext* UVCtx = Contexts.Find(ENGonUndoContextId::UV))
    {
        if (const FNGonUndoNode* N = UVCtx->FindActiveNode()) Out.UVEpoch = N->ProducedEpochs.UVEpoch;
    }
    if (const FNGonUndoContext* RigCtx = Contexts.Find(ENGonUndoContextId::Skeleton))
    {
        if (const FNGonUndoNode* N = RigCtx->FindActiveNode()) Out.RigEpoch = N->ProducedEpochs.RigEpoch;
    }
    return Out;
}

SIZE_T FNGonUndoSession::GetTotalMemoryBytes() const
{
    SIZE_T Sum = 0;
    for (const TPair<ENGonUndoContextId, FNGonUndoContext>& KV : Contexts)
    {
        Sum += KV.Value.GetTotalBytes();
    }
    return Sum;
}

TArray<ENGonUndoContextId> FNGonUndoSession::GetOrderedContextIds() const
{
    return {
        ENGonUndoContextId::Mesh,
        ENGonUndoContextId::Skeleton,
        ENGonUndoContextId::UV,
        ENGonUndoContextId::Paint
    };
}

uint64 FNGonUndoSession::PushEdit(
    ENGonUndoContextId ContextId,
    const FString& Label,
    ENGonUndoEditKind EditKind,
    TSharedPtr<INGonUndoPayload> Payload,
    SIZE_T ApproxBytes)
{
    if (bIsApplyingHistory) return 0;
    FNGonUndoContext* Ctx = Contexts.Find(ContextId);
    if (!Ctx) return 0;

    FNGonUndoNode* Parent = Ctx->Nodes.Find(Ctx->ActiveNodeId);
    if (!Parent) return 0;

    const FNGonEpochVector CurEpochs = GetCurrentEpochs();
    FNGonEpochVector Produced = CurEpochs;

    if (ContextId == ENGonUndoContextId::Mesh && EditKind == ENGonUndoEditKind::Structural)
    {
        Produced.TopoEpoch = NextTopoEpoch++;
    }
    else if (ContextId == ENGonUndoContextId::UV && EditKind == ENGonUndoEditKind::Structural)
    {
        Produced.UVEpoch = NextUVEpoch++;
    }
    else if (ContextId == ENGonUndoContextId::Skeleton && EditKind == ENGonUndoEditKind::Structural)
    {
        Produced.RigEpoch = NextRigEpoch++;
    }

    FNGonUndoNode Node;
    Node.NodeId = NextNodeId++;
    Node.ParentId = Parent->NodeId;
    Node.Context = ContextId;
    Node.EditKind = EditKind;
    Node.Label = Label.IsEmpty() ? TEXT("Edit") : Label;
    if (Parent->ChildIds.Num() >= 1)
    {
        Node.Label += FString::Printf(TEXT(" (Alt #%d)"), Parent->ChildIds.Num() + 1);
    }
    Node.TimestampSec = FPlatformTime::Seconds();
    Node.RequiredEpochs = CurEpochs;
    Node.ProducedEpochs = Produced;
    Node.Payload = MoveTemp(Payload);
    Node.CachedBytes = Node.Payload.IsValid() ? FMath::Max(ApproxBytes, Node.Payload->GetMemoryBytes()) : ApproxBytes;

    Parent->ChildIds.Add(Node.NodeId);
    Parent->ActiveChildId = Node.NodeId;
    const uint64 CreatedId = Node.NodeId;
    Ctx->ActiveNodeId = CreatedId;
    Ctx->Nodes.Add(CreatedId, MoveTemp(Node));

    EnforcePolicy();
    NotifyChanged();
    return CreatedId;
}

void FNGonUndoSession::SetContextUserChecked(ENGonUndoContextId ContextId, bool bChecked)
{
    if (FNGonUndoContext* Ctx = Contexts.Find(ContextId))
    {
        if (Ctx->bUserChecked != bChecked)
        {
            Ctx->bUserChecked = bChecked;
            NotifyChanged();
        }
    }
}

bool FNGonUndoSession::IsContextUserChecked(ENGonUndoContextId ContextId) const
{
    const FNGonUndoContext* Ctx = Contexts.Find(ContextId);
    return Ctx ? Ctx->bUserChecked : false;
}

void FNGonUndoSession::SetAllContextsChecked(bool bChecked)
{
    bool bChanged = false;
    for (TPair<ENGonUndoContextId, FNGonUndoContext>& KV : Contexts)
    {
        if (KV.Value.bUserChecked != bChecked)
        {
            KV.Value.bUserChecked = bChecked;
            bChanged = true;
        }
    }
    if (bChanged) NotifyChanged();
}

bool FNGonUndoSession::IsNodeEpochLocked(ENGonUndoContextId ContextId, uint64 NodeId, FString* OutReason) const
{
    const FNGonUndoContext* Ctx = Contexts.Find(ContextId);
    if (!Ctx) return false;
    const FNGonUndoNode* Node = Ctx->FindNode(NodeId);
    if (!Node || Node->ParentId == 0) return false;

    const FNGonEpochVector Cur = GetCurrentEpochs();
    if (ContextId == ENGonUndoContextId::UV && Node->RequiredEpochs.TopoEpoch > Cur.TopoEpoch)
    {
        if (OutReason)
        {
            *OutReason = FString::Printf(TEXT("Requires Mesh TopoEpoch %llu (current %llu)"),
                Node->RequiredEpochs.TopoEpoch, Cur.TopoEpoch);
        }
        return true;
    }
    if (ContextId == ENGonUndoContextId::Paint && Node->RequiredEpochs.UVEpoch > Cur.UVEpoch)
    {
        if (OutReason)
        {
            *OutReason = FString::Printf(TEXT("Requires UV Epoch %llu (current %llu)"),
                Node->RequiredEpochs.UVEpoch, Cur.UVEpoch);
        }
        return true;
    }
    return false;
}

void FNGonUndoSession::ComputeEffectiveSelection(
    TMap<ENGonUndoContextId, ENGonUndoSelectState>& OutStates,
    TMap<ENGonUndoContextId, FString>& OutAutoReasons) const
{
    OutStates.Reset();
    OutAutoReasons.Reset();
    for (const TPair<ENGonUndoContextId, FNGonUndoContext>& KV : Contexts)
    {
        OutStates.Add(KV.Key, KV.Value.bUserChecked ? ENGonUndoSelectState::UserChecked : ENGonUndoSelectState::None);
    }

    if (!Policy.bAutoLinkDependencies)
    {
        return;
    }

    // 1. Check if Mesh is selected and its next Undo drops TopoEpoch below UV's active requirement
    if (OutStates.FindRef(ENGonUndoContextId::Mesh) != ENGonUndoSelectState::None)
    {
        const FNGonUndoContext* MeshCtx = Contexts.Find(ENGonUndoContextId::Mesh);
        const FNGonUndoContext* UVCtx = Contexts.Find(ENGonUndoContextId::UV);
        if (MeshCtx && UVCtx)
        {
            const FNGonUndoNode* MeshCur = MeshCtx->FindActiveNode();
            const FNGonUndoNode* MeshPrev = MeshCur ? MeshCtx->FindNode(MeshCur->ParentId) : nullptr;
            const FNGonUndoNode* UVCur = UVCtx->FindActiveNode();
            if (MeshCur && MeshPrev && UVCur && MeshPrev->ProducedEpochs.TopoEpoch < MeshCur->ProducedEpochs.TopoEpoch)
            {
                if (UVCur->RequiredEpochs.TopoEpoch > MeshPrev->ProducedEpochs.TopoEpoch &&
                    OutStates.FindRef(ENGonUndoContextId::UV) == ENGonUndoSelectState::None)
                {
                    OutStates.Add(ENGonUndoContextId::UV, ENGonUndoSelectState::AutoLinked);
                    OutAutoReasons.Add(ENGonUndoContextId::UV,
                        FString::Printf(TEXT("Auto-linked: Undoing '%s' reverts Mesh TopoEpoch; UV '%s' depends on it."),
                            *MeshCur->Label, *UVCur->Label));
                }
            }
        }
    }

    // 2. Check if UV is selected or AutoLinked and its next Undo drops UVEpoch below Paint's active requirement
    if (OutStates.FindRef(ENGonUndoContextId::UV) != ENGonUndoSelectState::None)
    {
        const FNGonUndoContext* UVCtx = Contexts.Find(ENGonUndoContextId::UV);
        const FNGonUndoContext* PaintCtx = Contexts.Find(ENGonUndoContextId::Paint);
        if (UVCtx && PaintCtx)
        {
            const FNGonUndoNode* UVCur = UVCtx->FindActiveNode();
            const FNGonUndoNode* UVPrev = UVCur ? UVCtx->FindNode(UVCur->ParentId) : nullptr;
            const FNGonUndoNode* PaintCur = PaintCtx->FindActiveNode();
            if (UVCur && UVPrev && PaintCur && UVPrev->ProducedEpochs.UVEpoch < UVCur->ProducedEpochs.UVEpoch)
            {
                if (PaintCur->RequiredEpochs.UVEpoch > UVPrev->ProducedEpochs.UVEpoch &&
                    OutStates.FindRef(ENGonUndoContextId::Paint) == ENGonUndoSelectState::None)
                {
                    OutStates.Add(ENGonUndoContextId::Paint, ENGonUndoSelectState::AutoLinked);
                    OutAutoReasons.Add(ENGonUndoContextId::Paint,
                        FString::Printf(TEXT("Auto-linked: UV reverts to Epoch %llu; TiPaint '%s' requires syncing."),
                            UVPrev->ProducedEpochs.UVEpoch, *PaintCur->Label));
                }
            }
        }
    }
}

void FNGonUndoSession::QuarantineLockedDownstream(TArray<FString>& OutNotes)
{
    const ENGonUndoContextId DownstreamOrder[] = { ENGonUndoContextId::UV, ENGonUndoContextId::Paint };
    for (const ENGonUndoContextId DepId : DownstreamOrder)
    {
        FNGonUndoContext* Ctx = Contexts.Find(DepId);
        if (!Ctx) continue;
        while (IsNodeEpochLocked(DepId, Ctx->ActiveNodeId))
        {
            const FNGonUndoNode* Cur = Ctx->FindActiveNode();
            if (!Cur || Cur->ParentId == 0) break;
            if (Cur->Payload.IsValid())
            {
                Cur->Payload->ApplyUndo();
            }
            Ctx->ActiveNodeId = Cur->ParentId;
            if (const FNGonUndoNode* NewCur = Ctx->FindActiveNode())
            {
                OutNotes.Add(FString::Printf(TEXT("Epoch-Locked %s at '%s'"), *Ctx->DisplayName, *NewCur->Label));
            }
        }
    }
}

bool FNGonUndoSession::CanUndoChecked() const
{
    TMap<ENGonUndoContextId, ENGonUndoSelectState> States;
    TMap<ENGonUndoContextId, FString> Reasons;
    ComputeEffectiveSelection(States, Reasons);
    for (const TPair<ENGonUndoContextId, ENGonUndoSelectState>& KV : States)
    {
        if (KV.Value == ENGonUndoSelectState::None) continue;
        if (const FNGonUndoContext* Ctx = Contexts.Find(KV.Key))
        {
            if (const FNGonUndoNode* Active = Ctx->FindActiveNode(); Active && Active->ParentId != 0)
            {
                return true;
            }
        }
    }
    return false;
}

bool FNGonUndoSession::CanRedoChecked() const
{
    TMap<ENGonUndoContextId, ENGonUndoSelectState> States;
    TMap<ENGonUndoContextId, FString> Reasons;
    ComputeEffectiveSelection(States, Reasons);
    for (const TPair<ENGonUndoContextId, ENGonUndoSelectState>& KV : States)
    {
        if (KV.Value == ENGonUndoSelectState::None) continue;
        if (const FNGonUndoContext* Ctx = Contexts.Find(KV.Key))
        {
            if (const FNGonUndoNode* Active = Ctx->FindActiveNode(); Active && !Active->ChildIds.IsEmpty())
            {
                const uint64 NextId = (Active->ActiveChildId != 0 && Active->ChildIds.Contains(Active->ActiveChildId))
                    ? Active->ActiveChildId
                    : Active->ChildIds.Last();
                if (!IsNodeEpochLocked(KV.Key, NextId))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool FNGonUndoSession::UndoChecked(FString* OutSummary)
{
    if (bIsApplyingHistory) return false;
    TMap<ENGonUndoContextId, ENGonUndoSelectState> States;
    TMap<ENGonUndoContextId, FString> Reasons;
    ComputeEffectiveSelection(States, Reasons);

    // Downstream first so dependent contexts step back cleanly before upstream topology
    const ENGonUndoContextId Order[] = {
        ENGonUndoContextId::Paint,
        ENGonUndoContextId::UV,
        ENGonUndoContextId::Skeleton,
        ENGonUndoContextId::Mesh
    };

    TGuardValue<bool> ApplyGuard(bIsApplyingHistory, true);
    TArray<FString> UndoneNotes;
    for (const ENGonUndoContextId CtxId : Order)
    {
        if (States.FindRef(CtxId) == ENGonUndoSelectState::None) continue;
        FNGonUndoContext* Ctx = Contexts.Find(CtxId);
        if (!Ctx) continue;
        FNGonUndoNode* Cur = Ctx->Nodes.Find(Ctx->ActiveNodeId);
        if (!Cur || Cur->ParentId == 0) continue;

        if (Cur->Payload.IsValid())
        {
            Cur->Payload->ApplyUndo();
        }
        if (FNGonUndoNode* Parent = Ctx->Nodes.Find(Cur->ParentId))
        {
            Parent->ActiveChildId = Cur->NodeId;
        }
        Ctx->ActiveNodeId = Cur->ParentId;
        UndoneNotes.Add(FString::Printf(TEXT("%s: %s"), *Ctx->DisplayName, *Cur->Label));
    }

    QuarantineLockedDownstream(UndoneNotes);

    if (OutSummary)
    {
        *OutSummary = UndoneNotes.IsEmpty()
            ? TEXT("Nothing to undo on checked branches.")
            : (TEXT("Undid: ") + FString::Join(UndoneNotes, TEXT(" | ")));
    }
    if (!UndoneNotes.IsEmpty())
    {
        NotifyChanged();
        return true;
    }
    return false;
}

bool FNGonUndoSession::RedoChecked(FString* OutSummary)
{
    if (bIsApplyingHistory) return false;
    TMap<ENGonUndoContextId, ENGonUndoSelectState> States;
    TMap<ENGonUndoContextId, FString> Reasons;
    ComputeEffectiveSelection(States, Reasons);

    // Upstream first so epochs unlock before downstream nodes re-apply
    const ENGonUndoContextId Order[] = {
        ENGonUndoContextId::Mesh,
        ENGonUndoContextId::Skeleton,
        ENGonUndoContextId::UV,
        ENGonUndoContextId::Paint
    };

    TGuardValue<bool> ApplyGuard(bIsApplyingHistory, true);
    TArray<FString> RedoneNotes;
    for (const ENGonUndoContextId CtxId : Order)
    {
        if (States.FindRef(CtxId) == ENGonUndoSelectState::None) continue;
        FNGonUndoContext* Ctx = Contexts.Find(CtxId);
        if (!Ctx) continue;
        FNGonUndoNode* Cur = Ctx->Nodes.Find(Ctx->ActiveNodeId);
        if (!Cur || Cur->ChildIds.IsEmpty()) continue;

        const uint64 NextId = (Cur->ActiveChildId != 0 && Cur->ChildIds.Contains(Cur->ActiveChildId))
            ? Cur->ActiveChildId
            : Cur->ChildIds.Last();

        if (IsNodeEpochLocked(CtxId, NextId))
        {
            continue;
        }
        FNGonUndoNode* NextNode = Ctx->Nodes.Find(NextId);
        if (!NextNode) continue;

        if (NextNode->Payload.IsValid())
        {
            NextNode->Payload->ApplyRedo();
        }
        Cur->ActiveChildId = NextId;
        Ctx->ActiveNodeId = NextId;
        RedoneNotes.Add(FString::Printf(TEXT("%s: %s"), *Ctx->DisplayName, *NextNode->Label));
    }

    if (OutSummary)
    {
        *OutSummary = RedoneNotes.IsEmpty()
            ? TEXT("Nothing to redo on checked branches.")
            : (TEXT("Redid: ") + FString::Join(RedoneNotes, TEXT(" | ")));
    }
    if (!RedoneNotes.IsEmpty())
    {
        NotifyChanged();
        return true;
    }
    return false;
}

bool FNGonUndoSession::UndoContext(ENGonUndoContextId ContextId, FString* OutMessage)
{
    if (bIsApplyingHistory) return false;
    FNGonUndoContext* Ctx = Contexts.Find(ContextId);
    if (!Ctx) return false;
    FNGonUndoNode* Cur = Ctx->Nodes.Find(Ctx->ActiveNodeId);
    if (!Cur || Cur->ParentId == 0)
    {
        if (OutMessage) *OutMessage = TEXT("Already at context root.");
        return false;
    }

    TGuardValue<bool> ApplyGuard(bIsApplyingHistory, true);
    if (Cur->Payload.IsValid())
    {
        Cur->Payload->ApplyUndo();
    }
    if (FNGonUndoNode* Parent = Ctx->Nodes.Find(Cur->ParentId))
    {
        Parent->ActiveChildId = Cur->NodeId;
    }
    Ctx->ActiveNodeId = Cur->ParentId;

    TArray<FString> Notes;
    QuarantineLockedDownstream(Notes);
    if (OutMessage)
    {
        *OutMessage = FString::Printf(TEXT("Undid %s: %s"), *Ctx->DisplayName, *Cur->Label);
    }
    NotifyChanged();
    return true;
}

bool FNGonUndoSession::RedoContext(ENGonUndoContextId ContextId, FString* OutMessage)
{
    if (bIsApplyingHistory) return false;
    FNGonUndoContext* Ctx = Contexts.Find(ContextId);
    if (!Ctx) return false;
    FNGonUndoNode* Cur = Ctx->Nodes.Find(Ctx->ActiveNodeId);
    if (!Cur || Cur->ChildIds.IsEmpty())
    {
        if (OutMessage) *OutMessage = TEXT("No redo branch available.");
        return false;
    }

    const uint64 NextId = (Cur->ActiveChildId != 0 && Cur->ChildIds.Contains(Cur->ActiveChildId))
        ? Cur->ActiveChildId
        : Cur->ChildIds.Last();

    FString LockReason;
    if (IsNodeEpochLocked(ContextId, NextId, &LockReason))
    {
        if (OutMessage) *OutMessage = LockReason;
        return false;
    }

    FNGonUndoNode* NextNode = Ctx->Nodes.Find(NextId);
    if (!NextNode) return false;

    TGuardValue<bool> ApplyGuard(bIsApplyingHistory, true);
    if (NextNode->Payload.IsValid())
    {
        NextNode->Payload->ApplyRedo();
    }
    Cur->ActiveChildId = NextId;
    Ctx->ActiveNodeId = NextId;
    if (OutMessage)
    {
        *OutMessage = FString::Printf(TEXT("Redid %s: %s"), *Ctx->DisplayName, *NextNode->Label);
    }
    NotifyChanged();
    return true;
}

bool FNGonUndoSession::JumpToNode(ENGonUndoContextId ContextId, uint64 TargetNodeId, FString* OutMessage)
{
    if (bIsApplyingHistory) return false;
    FNGonUndoContext* Ctx = Contexts.Find(ContextId);
    if (!Ctx || !Ctx->Nodes.Contains(TargetNodeId)) return false;
    if (Ctx->ActiveNodeId == TargetNodeId) return true;

    FString LockReason;
    if (IsNodeEpochLocked(ContextId, TargetNodeId, &LockReason))
    {
        if (OutMessage) *OutMessage = LockReason;
        return false;
    }

    // Build ancestor chains for ActiveNodeId and TargetNodeId to find LCA
    TArray<uint64> ActiveChain;
    for (uint64 Id = Ctx->ActiveNodeId; Id != 0;)
    {
        ActiveChain.Add(Id);
        const FNGonUndoNode* N = Ctx->Nodes.Find(Id);
        Id = N ? N->ParentId : 0;
    }

    TSet<uint64> ActiveSet(ActiveChain);
    TArray<uint64> RedoPath; // From LCA child down to TargetNodeId
    uint64 LCA = 0;
    for (uint64 Id = TargetNodeId; Id != 0;)
    {
        if (ActiveSet.Contains(Id))
        {
            LCA = Id;
            break;
        }
        RedoPath.Add(Id);
        const FNGonUndoNode* N = Ctx->Nodes.Find(Id);
        Id = N ? N->ParentId : 0;
    }
    if (LCA == 0) return false;
    Algo::Reverse(RedoPath);

    TGuardValue<bool> ApplyGuard(bIsApplyingHistory, true);

    // 1. Undo from ActiveNodeId up to LCA
    while (Ctx->ActiveNodeId != LCA)
    {
        FNGonUndoNode* Cur = Ctx->Nodes.Find(Ctx->ActiveNodeId);
        if (!Cur || Cur->ParentId == 0) break;
        if (Cur->Payload.IsValid()) Cur->Payload->ApplyUndo();
        Ctx->ActiveNodeId = Cur->ParentId;
    }

    // 2. Redo from LCA down to TargetNodeId
    for (const uint64 StepId : RedoPath)
    {
        FNGonUndoNode* StepNode = Ctx->Nodes.Find(StepId);
        if (!StepNode) break;
        if (StepNode->Payload.IsValid()) StepNode->Payload->ApplyRedo();
        if (FNGonUndoNode* P = Ctx->Nodes.Find(StepNode->ParentId))
        {
            P->ActiveChildId = StepId;
        }
        Ctx->ActiveNodeId = StepId;
    }

    TArray<FString> Notes;
    QuarantineLockedDownstream(Notes);

    if (OutMessage)
    {
        const FNGonUndoNode* FinalNode = Ctx->FindActiveNode();
        *OutMessage = FString::Printf(TEXT("Jumped %s to '%s'"), *Ctx->DisplayName, FinalNode ? *FinalNode->Label : TEXT("Root"));
    }
    NotifyChanged();
    return true;
}

bool FNGonUndoSession::PruneOneOldestNodeInContext(FNGonUndoContext& Ctx)
{
    // Build active lineage set so we never prune ActiveNodeId or its immediate parent
    TSet<uint64> ActiveLineage;
    for (uint64 Id = Ctx.ActiveNodeId; Id != 0;)
    {
        ActiveLineage.Add(Id);
        const FNGonUndoNode* N = Ctx.Nodes.Find(Id);
        Id = N ? N->ParentId : 0;
    }

    // 1. Prefer pruning oldest inactive-branch leaf first
    if (Policy.bPruneInactiveBranchesFirst)
    {
        uint64 VictimId = 0;
        double OldestTime = TNumericLimits<double>::Max();
        for (const TPair<uint64, FNGonUndoNode>& KV : Ctx.Nodes)
        {
            const FNGonUndoNode& N = KV.Value;
            if (N.ParentId == 0 || !N.ChildIds.IsEmpty() || ActiveLineage.Contains(N.NodeId)) continue;
            if (N.TimestampSec < OldestTime)
            {
                OldestTime = N.TimestampSec;
                VictimId = N.NodeId;
            }
        }
        if (VictimId != 0)
        {
            const uint64 ParentId = Ctx.Nodes[VictimId].ParentId;
            if (FNGonUndoNode* P = Ctx.Nodes.Find(ParentId))
            {
                P->ChildIds.Remove(VictimId);
                if (P->ActiveChildId == VictimId)
                {
                    P->ActiveChildId = P->ChildIds.IsEmpty() ? 0 : P->ChildIds.Last();
                }
            }
            Ctx.Nodes.Remove(VictimId);
            return true;
        }
    }

    // 2. Collapse oldest linear root node (promote its sole child to be the new root)
    FNGonUndoNode* Root = Ctx.Nodes.Find(Ctx.RootNodeId);
    if (Root && Root->ChildIds.Num() == 1)
    {
        const uint64 SoleChildId = Root->ChildIds[0];
        if (SoleChildId != Ctx.ActiveNodeId)
        {
            if (FNGonUndoNode* Child = Ctx.Nodes.Find(SoleChildId))
            {
                const uint64 OldRootId = Ctx.RootNodeId;
                Child->ParentId = 0;
                Child->EditKind = ENGonUndoEditKind::Base;
                Child->Payload.Reset();
                Child->CachedBytes = 0;
                Ctx.RootNodeId = SoleChildId;
                Ctx.Nodes.Remove(OldRootId);
                return true;
            }
        }
    }
    return false;
}

void FNGonUndoSession::EnforcePolicy()
{
    // 1. Step-count cap per context (MaxUndoSteps + 1 root node)
    const int32 MaxNodes = FMath::Max(1, Policy.MaxUndoSteps) + 1;
    for (TPair<ENGonUndoContextId, FNGonUndoContext>& KV : Contexts)
    {
        FNGonUndoContext& Ctx = KV.Value;
        while (Ctx.Nodes.Num() > MaxNodes)
        {
            if (!PruneOneOldestNodeInContext(Ctx)) break;
        }
    }

    // 2. Global byte cap across all contexts
    int32 Guard = 0;
    while (GetTotalMemoryBytes() > Policy.MaxMemoryBytes && Guard < 1024)
    {
        ++Guard;
        ENGonUndoContextId HeaviestCtx = ENGonUndoContextId::Mesh;
        SIZE_T MaxBytes = 0;
        for (const TPair<ENGonUndoContextId, FNGonUndoContext>& KV : Contexts)
        {
            const SIZE_T B = KV.Value.GetTotalBytes();
            if (B > MaxBytes && KV.Value.Nodes.Num() > 2)
            {
                MaxBytes = B;
                HeaviestCtx = KV.Key;
            }
        }
        if (MaxBytes == 0) break;
        if (!PruneOneOldestNodeInContext(Contexts[HeaviestCtx])) break;
    }
}

void FNGonUndoSession::NotifyChanged()
{
    OnTreeChanged.Broadcast();
}

FString FNGonUndoSession::ContextIdToName(ENGonUndoContextId Id)
{
    switch (Id)
    {
    case ENGonUndoContextId::Root:     return TEXT("Root");
    case ENGonUndoContextId::Mesh:     return TEXT("Mesh Topology");
    case ENGonUndoContextId::Skeleton: return TEXT("Skeleton Rig");
    case ENGonUndoContextId::UV:       return TEXT("UV Editor");
    case ENGonUndoContextId::Paint:    return TEXT("TiPaint Canvas");
    default:                           return TEXT("Context");
    }
}

FString FNGonUndoSession::EditKindToTag(ENGonUndoEditKind Kind)
{
    switch (Kind)
    {
    case ENGonUndoEditKind::Base:       return TEXT("BASE");
    case ENGonUndoEditKind::Structural: return TEXT("STRUCT");
    case ENGonUndoEditKind::Projection: return TEXT("3D PROJ");
    case ENGonUndoEditKind::Value:
    default:                            return TEXT("VALUE");
    }
}

namespace NGon::Undo
{
void RegisterSession(const void* OwnerKey, const TSharedPtr<FNGonUndoSession>& Session)
{
    if (!OwnerKey || !Session.IsValid()) return;
    GUndoSessionRegistry.Add(OwnerKey, Session);
    GActiveUndoSession = Session;
}

void UnregisterSession(const void* OwnerKey)
{
    if (!OwnerKey) return;
    GUndoSessionRegistry.Remove(OwnerKey);
}

TSharedPtr<FNGonUndoSession> FindSession(const void* OwnerKey)
{
    if (!OwnerKey) return nullptr;
    if (const TWeakPtr<FNGonUndoSession>* Found = GUndoSessionRegistry.Find(OwnerKey))
    {
        return Found->Pin();
    }
    return nullptr;
}

void SetActiveSession(const TSharedPtr<FNGonUndoSession>& Session)
{
    GActiveUndoSession = Session;
}

TSharedPtr<FNGonUndoSession> GetActiveSession()
{
    return GActiveUndoSession.Pin();
}
} // namespace NGon::Undo

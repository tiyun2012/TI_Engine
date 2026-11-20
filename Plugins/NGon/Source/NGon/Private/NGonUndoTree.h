#pragma once

#include "CoreMinimal.h"

/** Identifies the root session or one of its isolated child undo contexts. */
enum class ENGonUndoContextId : uint8
{
    Root = 0,
    Mesh,
    Skeleton,
    UV,
    Paint
};

/** Classifies whether an edit changes index topology or only coordinate/pixel values. */
enum class ENGonUndoEditKind : uint8
{
    Base,
    Value,
    Structural,
    Projection
};

/** 3-state checkbox participation for Selective Branch Undo/Redo. */
enum class ENGonUndoSelectState : uint8
{
    None,
    UserChecked,
    AutoLinked
};

/** Monotonic dependency epochs tracked across parent/child contexts. */
struct FNGonEpochVector
{
    uint64 TopoEpoch = 1;
    uint64 UVEpoch = 1;
    uint64 RigEpoch = 1;
};

/** Dual-ceiling buffer policy: step limit (default 20) + global memory cap (256 MB). */
struct FNGonUndoPolicy
{
    int32 MaxUndoSteps = 20;
    SIZE_T MaxMemoryBytes = 256ull * 1024ull * 1024ull;
    bool bAutoLinkDependencies = true;
    bool bPruneInactiveBranchesFirst = true;
};

/** Polymorphic undo/redo payload stored on each node in a context tree. */
class INGonUndoPayload
{
public:
    virtual ~INGonUndoPayload() = default;
    virtual SIZE_T GetMemoryBytes() const = 0;
    virtual bool ApplyUndo() = 0;
    virtual bool ApplyRedo() = 0;
};

/** Convenience lambda-backed payload for clean adapter integration across NGon modules. */
class FNGonLambdaUndoPayload final : public INGonUndoPayload
{
public:
    FNGonLambdaUndoPayload(TFunction<bool()> InUndo, TFunction<bool()> InRedo, SIZE_T InBytes)
        : OnUndo(MoveTemp(InUndo)), OnRedo(MoveTemp(InRedo)), Bytes(InBytes) {}

    virtual SIZE_T GetMemoryBytes() const override { return Bytes; }
    virtual bool ApplyUndo() override { return OnUndo ? OnUndo() : true; }
    virtual bool ApplyRedo() override { return OnRedo ? OnRedo() : true; }

private:
    TFunction<bool()> OnUndo;
    TFunction<bool()> OnRedo;
    SIZE_T Bytes = 0;
};

/** Single node in a context's branching undo tree. */
struct FNGonUndoNode
{
    uint64 NodeId = 0;
    uint64 ParentId = 0;
    TArray<uint64> ChildIds;
    uint64 ActiveChildId = 0;
    ENGonUndoContextId Context = ENGonUndoContextId::Mesh;
    ENGonUndoEditKind EditKind = ENGonUndoEditKind::Value;
    FString Label;
    double TimestampSec = 0.0;
    FNGonEpochVector RequiredEpochs;
    FNGonEpochVector ProducedEpochs;
    TSharedPtr<INGonUndoPayload> Payload;
    SIZE_T CachedBytes = 0;
};

/** Isolated branching undo tree for a single editor context (Mesh, Skeleton, UV, or TiPaint). */
struct FNGonUndoContext
{
    ENGonUndoContextId Id = ENGonUndoContextId::Mesh;
    FString DisplayName;
    TArray<ENGonUndoContextId> DependsOn;
    bool bUserChecked = false;
    uint64 RootNodeId = 0;
    uint64 ActiveNodeId = 0;
    TMap<uint64, FNGonUndoNode> Nodes;

    const FNGonUndoNode* FindActiveNode() const { return Nodes.Find(ActiveNodeId); }
    const FNGonUndoNode* FindNode(uint64 IdToFind) const { return Nodes.Find(IdToFind); }
    int32 GetDepthOfActive() const;
    SIZE_T GetTotalBytes() const;
};

/**
 * Root-owned Context-Scoped Undo Tree Session.
 * Manages Mesh, Skeleton, UV, and TiPaint child trees, enforces monotonic dependency epochs,
 * supports 3-state checkbox selective undo with structural auto-linking, and caps memory + step depth.
 */
class FNGonUndoSession : public TSharedFromThis<FNGonUndoSession>
{
public:
    explicit FNGonUndoSession(const FString& InSessionName = TEXT("Main Window"));

    const FString& GetSessionName() const { return SessionName; }
    void SetSessionName(const FString& InName) { SessionName = InName; NotifyChanged(); }

    const FNGonUndoPolicy& GetPolicy() const { return Policy; }
    void SetMaxUndoSteps(int32 NewMaxSteps);
    void SetAutoLinkDependencies(bool bEnable);
    void SetMaxMemoryBytes(SIZE_T NewMaxBytes);

    /** Resets all four child contexts to clean root base nodes (e.g., on new FBX load). */
    void ResetSession(const FString& NewSessionName = FString());

    /** Pushes a new edit node under the context's active node (creates a sibling branch if undone). */
    uint64 PushEdit(
        ENGonUndoContextId ContextId,
        const FString& Label,
        ENGonUndoEditKind EditKind,
        TSharedPtr<INGonUndoPayload> Payload,
        SIZE_T ApproxBytes = 0);

    /** Checkbox control for selective branch undo. */
    void SetContextUserChecked(ENGonUndoContextId ContextId, bool bChecked);
    bool IsContextUserChecked(ENGonUndoContextId ContextId) const;
    void SetAllContextsChecked(bool bChecked);

    /** Evaluates 3-state selection (None / UserChecked / AutoLinked) and dependency reasons. */
    void ComputeEffectiveSelection(
        TMap<ENGonUndoContextId, ENGonUndoSelectState>& OutStates,
        TMap<ENGonUndoContextId, FString>& OutAutoReasons) const;

    /** Executes Undo / Redo across all checked & auto-linked contexts in dependency-safe order. */
    bool UndoChecked(FString* OutSummary = nullptr);
    bool RedoChecked(FString* OutSummary = nullptr);
    bool CanUndoChecked() const;
    bool CanRedoChecked() const;

    /** Single-context Undo / Redo / Direct Tree Jump. */
    bool UndoContext(ENGonUndoContextId ContextId, FString* OutMessage = nullptr);
    bool RedoContext(ENGonUndoContextId ContextId, FString* OutMessage = nullptr);
    bool JumpToNode(ENGonUndoContextId ContextId, uint64 TargetNodeId, FString* OutMessage = nullptr);

    /** Checks whether a node is locked due to an upstream epoch mismatch. */
    bool IsNodeEpochLocked(ENGonUndoContextId ContextId, uint64 NodeId, FString* OutReason = nullptr) const;

    /** Current active epochs across all contexts. */
    FNGonEpochVector GetCurrentEpochs() const;
    SIZE_T GetTotalMemoryBytes() const;

    /** Guard flag so Undo/Redo execution does not recursively push new undo nodes. */
    bool IsApplyingHistory() const { return bIsApplyingHistory; }

    const FNGonUndoContext* GetContext(ENGonUndoContextId ContextId) const { return Contexts.Find(ContextId); }
    TArray<ENGonUndoContextId> GetOrderedContextIds() const;

    /** Notification callback fired whenever tree structure, selection, or active nodes change. */
    FSimpleMulticastDelegate OnTreeChanged;

    static FString ContextIdToName(ENGonUndoContextId Id);
    static FString EditKindToTag(ENGonUndoEditKind Kind);

private:
    void InitContextRoot(ENGonUndoContextId ContextId, const FString& Name, const TArray<ENGonUndoContextId>& Deps, bool bDefaultChecked, const FString& RootLabel);
    void QuarantineLockedDownstream(TArray<FString>& OutNotes);
    void EnforcePolicy();
    bool PruneOneOldestNodeInContext(FNGonUndoContext& Ctx);
    void NotifyChanged();

    FString SessionName;
    FNGonUndoPolicy Policy;
    TMap<ENGonUndoContextId, FNGonUndoContext> Contexts;
    uint64 NextNodeId = 1;
    uint64 NextTopoEpoch = 2;
    uint64 NextUVEpoch = 2;
    uint64 NextRigEpoch = 2;
    bool bIsApplyingHistory = false;
};

namespace NGon::Undo
{
    /** Global registry linking a scene/editor pointer (e.g. FNGonPreviewScene*) to its Root Undo Session. */
    void RegisterSession(const void* OwnerKey, const TSharedPtr<FNGonUndoSession>& Session);
    void UnregisterSession(const void* OwnerKey);
    TSharedPtr<FNGonUndoSession> FindSession(const void* OwnerKey);
    /** Tracks the currently active session for tools (like standalone UV Editor) that operate on the active mesh. */
    void SetActiveSession(const TSharedPtr<FNGonUndoSession>& Session);
    TSharedPtr<FNGonUndoSession> GetActiveSession();
}

#pragma once

#include "CoreMinimal.h"
#include "NGonUndoTree.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

/** One row item in the Outliner / Bone-Tree style Undo History STreeView. */
struct FNGonUndoTreeRow
{
    enum class ERowKind : uint8
    {
        RootSession,
        ContextBranch,
        HistoryNode
    };

    ERowKind RowKind = ERowKind::HistoryNode;
    ENGonUndoContextId ContextId = ENGonUndoContextId::Root;
    uint64 NodeId = 0;
    FString Label;
    FString TypeTag;
    FString StepsText;
    FString MemText;
    bool bIsActive = false;
    bool bIsEpochLocked = false;
    FString LockTooltip;
    ENGonUndoSelectState SelectState = ENGonUndoSelectState::None;
    TArray<TSharedPtr<FNGonUndoTreeRow>> Children;
};

/**
 * Outliner / Skeleton-Tree style Undo History Panel with 3-state branch checkboxes,
 * structural auto-link toggle, configurable MaxUndoSteps (default 20), and live memory meter.
 */
class SNGonUndoTreePanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SNGonUndoTreePanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, const TSharedPtr<FNGonUndoSession>& InSession);
    virtual ~SNGonUndoTreePanel();

    void SetSession(const TSharedPtr<FNGonUndoSession>& InSession);
    void RebuildTree();

private:
    TSharedRef<ITableRow> GenerateRow(TSharedPtr<FNGonUndoTreeRow> Item, const TSharedRef<STableViewBase>& OwnerTable);
    void GetChildrenForRow(TSharedPtr<FNGonUndoTreeRow> Item, TArray<TSharedPtr<FNGonUndoTreeRow>>& OutChildren);
    void OnRowSelectionChanged(TSharedPtr<FNGonUndoTreeRow> Item, ESelectInfo::Type SelectInfo);
    void BuildNodeRowsRecursive(
        const FNGonUndoContext& Ctx,
        uint64 NodeId,
        TArray<TSharedPtr<FNGonUndoTreeRow>>& OutSiblings);
    void ExpandAllRowsRecursive(const TArray<TSharedPtr<FNGonUndoTreeRow>>& Items);

    TSharedPtr<FNGonUndoSession> Session;
    FDelegateHandle TreeChangedHandle;
    TSharedPtr<STreeView<TSharedPtr<FNGonUndoTreeRow>>> TreeView;
    TArray<TSharedPtr<FNGonUndoTreeRow>> RootRows;
    FString LastStatusText;
    bool bSuppressSelectionJump = false;
};

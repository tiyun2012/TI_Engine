#include "SNGonUndoTreePanel.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "NGonUndoTreePanel"

namespace
{
static const FName ColTreeName(TEXT("UndoTree"));
static const FName ColTypeName(TEXT("UndoType"));
static const FName ColStepsName(TEXT("UndoSteps"));
static const FName ColMemName(TEXT("UndoMem"));

FString FormatBytesCompact(SIZE_T Bytes)
{
    if (Bytes >= 1024ull * 1024ull)
    {
        return FString::Printf(TEXT("%.1f MB"), static_cast<double>(Bytes) / (1024.0 * 1024.0));
    }
    if (Bytes >= 1024ull)
    {
        return FString::Printf(TEXT("%.0f KB"), static_cast<double>(Bytes) / 1024.0);
    }
    return FString::Printf(TEXT("%llu B"), static_cast<uint64>(Bytes));
}

class SNGonUndoTreeTableRow final : public SMultiColumnTableRow<TSharedPtr<FNGonUndoTreeRow>>
{
public:
    SLATE_BEGIN_ARGS(SNGonUndoTreeTableRow) {}
    SLATE_END_ARGS()

    void Construct(
        const FArguments& InArgs,
        const TSharedRef<STableViewBase>& OwnerTable,
        const TSharedPtr<FNGonUndoTreeRow>& InItem,
        const TWeakPtr<FNGonUndoSession>& InSession)
    {
        Item = InItem;
        WeakSession = InSession;
        SMultiColumnTableRow<TSharedPtr<FNGonUndoTreeRow>>::Construct(
            FSuperRowType::FArguments().Padding(FMargin(0.0f, 1.0f)),
            OwnerTable);
    }

    virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
    {
        const TSharedPtr<FNGonUndoTreeRow> Row = Item;
        if (!Row.IsValid())
        {
            return SNullWidget::NullWidget;
        }

        if (ColumnName == ColTreeName)
        {
            TSharedRef<SHorizontalBox> RowBox = SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SExpanderArrow, SharedThis(this)).IndentAmount(14.0f)
                ];

            // Checkbox on Root Session and Child Context Branches
            if (Row->RowKind == FNGonUndoTreeRow::ERowKind::RootSession ||
                Row->RowKind == FNGonUndoTreeRow::ERowKind::ContextBranch)
            {
                const ECheckBoxState CbState =
                    (Row->SelectState == ENGonUndoSelectState::UserChecked) ? ECheckBoxState::Checked :
                    (Row->SelectState == ENGonUndoSelectState::AutoLinked) ? ECheckBoxState::Undetermined :
                    ECheckBoxState::Unchecked;

                RowBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 5.0f, 0.0f)
                [
                    SNew(SCheckBox)
                    .IsChecked(CbState)
                    .ToolTipText(Row->SelectState == ENGonUndoSelectState::AutoLinked
                        ? LOCTEXT("AutoLinkTip", "Auto-linked due to upstream structural dependency")
                        : LOCTEXT("BranchCbTip", "Include or exclude this branch in Undo Checked / Redo Checked"))
                    .OnCheckStateChanged_Lambda([Weak = WeakSession, Row](ECheckBoxState NewState)
                    {
                        if (const TSharedPtr<FNGonUndoSession> Sess = Weak.Pin())
                        {
                            const bool bCheck = (NewState == ECheckBoxState::Checked);
                            if (Row->RowKind == FNGonUndoTreeRow::ERowKind::RootSession)
                            {
                                Sess->SetAllContextsChecked(bCheck);
                            }
                            else
                            {
                                Sess->SetContextUserChecked(Row->ContextId, bCheck);
                            }
                        }
                    })
                ];
            }
            else
            {
                // Active / Locked / Inactive node glyph
                const FLinearColor DotColor = Row->bIsEpochLocked
                    ? FLinearColor(0.25f, 0.78f, 0.72f, 0.7f)
                    : Row->bIsActive
                    ? FLinearColor(0.25f, 0.62f, 1.0f, 1.0f)
                    : FLinearColor(0.45f, 0.50f, 0.58f, 0.85f);

                RowBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 5.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Row->bIsEpochLocked ? TEXT("[L]") : (Row->bIsActive ? TEXT("*") : TEXT("-"))))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                    .ColorAndOpacity(DotColor)
                ];
            }

            const FLinearColor LabelColor = Row->bIsEpochLocked
                ? FLinearColor(0.45f, 0.52f, 0.60f, 0.75f)
                : (Row->bIsActive || Row->RowKind != FNGonUndoTreeRow::ERowKind::HistoryNode)
                ? FLinearColor(0.94f, 0.96f, 0.99f, 1.0f)
                : FLinearColor(0.74f, 0.78f, 0.84f, 1.0f);

            RowBox->AddSlot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Row->Label))
                .Font(FCoreStyle::GetDefaultFontStyle((Row->bIsActive || Row->RowKind != FNGonUndoTreeRow::ERowKind::HistoryNode) ? "Bold" : "Regular", 8))
                .ColorAndOpacity(LabelColor)
                .ToolTipText(Row->bIsEpochLocked ? FText::FromString(Row->LockTooltip) : FText::FromString(Row->Label))
            ];

            return SNew(SBorder)
                .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                .BorderBackgroundColor(Row->bIsActive
                    ? FLinearColor(0.14f, 0.36f, 0.78f, 0.28f)
                    : (Row->RowKind == FNGonUndoTreeRow::ERowKind::ContextBranch
                        ? FLinearColor(0.11f, 0.14f, 0.20f, 0.65f)
                        : FLinearColor::Transparent))
                .Padding(FMargin(2.0f, 2.0f))
                [
                    RowBox
                ];
        }

        if (ColumnName == ColTypeName)
        {
            FLinearColor TagCol(0.55f, 0.60f, 0.68f, 1.0f);
            if (Row->TypeTag == TEXT("STRUCT")) TagCol = FLinearColor(0.95f, 0.72f, 0.25f, 1.0f);
            else if (Row->TypeTag == TEXT("LINKED") || Row->TypeTag == TEXT("3D PROJ")) TagCol = FLinearColor(0.18f, 0.85f, 0.76f, 1.0f);
            else if (Row->TypeTag == TEXT("ACTIVE") || Row->TypeTag == TEXT("ROOT")) TagCol = FLinearColor(0.38f, 0.68f, 1.0f, 1.0f);

            return SNew(SBox).VAlign(VAlign_Center).HAlign(HAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Row->TypeTag))
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 7))
                .ColorAndOpacity(TagCol)
            ];
        }

        if (ColumnName == ColStepsName)
        {
            return SNew(SBox).VAlign(VAlign_Center).HAlign(HAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Row->StepsText))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
                .ColorAndOpacity(FLinearColor(0.62f, 0.66f, 0.74f, 1.0f))
            ];
        }

        if (ColumnName == ColMemName)
        {
            return SNew(SBox).VAlign(VAlign_Center).HAlign(HAlign_Right).Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Row->MemText))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
                .ColorAndOpacity(FLinearColor(0.58f, 0.64f, 0.72f, 1.0f))
            ];
        }

        return SNullWidget::NullWidget;
    }

private:
    TSharedPtr<FNGonUndoTreeRow> Item;
    TWeakPtr<FNGonUndoSession> WeakSession;
};
} // namespace

void SNGonUndoTreePanel::Construct(const FArguments&, const TSharedPtr<FNGonUndoSession>& InSession)
{
    LastStatusText = TEXT("Check branches to control selective Undo / Redo.");

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(FLinearColor(0.045f, 0.055f, 0.075f, 1.0f))
        .Padding(6.0f)
        [
            SNew(SVerticalBox)
            // 1. Top Toolbar: Undo Checked, Redo Checked, Auto-Link, Limit SpinBox (default 20)
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
            [
                SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f))
                + SWrapBox::Slot()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("UndoCheckedBtn", "Undo Checked"))
                    .IsEnabled_Lambda([this] { return Session.IsValid() && Session->CanUndoChecked(); })
                    .OnClicked_Lambda([this]()
                    {
                        if (Session.IsValid())
                        {
                            Session->UndoChecked(&LastStatusText);
                        }
                        return FReply::Handled();
                    })
                ]
                + SWrapBox::Slot()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("RedoCheckedBtn", "Redo Checked"))
                    .IsEnabled_Lambda([this] { return Session.IsValid() && Session->CanRedoChecked(); })
                    .OnClicked_Lambda([this]()
                    {
                        if (Session.IsValid())
                        {
                            Session->RedoChecked(&LastStatusText);
                        }
                        return FReply::Handled();
                    })
                ]
                + SWrapBox::Slot().VAlign(VAlign_Center)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda([this]
                    {
                        return (Session.IsValid() && Session->GetPolicy().bAutoLinkDependencies)
                            ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                    {
                        if (Session.IsValid())
                        {
                            Session->SetAutoLinkDependencies(State == ECheckBoxState::Checked);
                        }
                    })
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("AutoLinkLbl", "Auto-Link"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                    ]
                ]
                + SWrapBox::Slot().VAlign(VAlign_Center)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 4.0f, 0.0f)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("MaxStepsLbl", "Limit:"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SBox).WidthOverride(52.0f)
                        [
                            SNew(SSpinBox<int32>)
                            .MinValue(1)
                            .MaxValue(256)
                            .Value_Lambda([this] { return Session.IsValid() ? Session->GetPolicy().MaxUndoSteps : 20; })
                            .OnValueChanged_Lambda([this](int32 NewLimit)
                            {
                                if (Session.IsValid())
                                {
                                    Session->SetMaxUndoSteps(NewLimit);
                                    LastStatusText = FString::Printf(TEXT("Undo limit set to %d step(s) per context."), NewLimit);
                                }
                            })
                        ]
                    ]
                ]
            ]
            // 2. Outliner / Bone-Tree STreeView
            + SVerticalBox::Slot().FillHeight(1.0f)
            [
                SAssignNew(TreeView, STreeView<TSharedPtr<FNGonUndoTreeRow>>)
                .TreeItemsSource(&RootRows)
                .SelectionMode(ESelectionMode::Single)
                .OnGenerateRow(this, &SNGonUndoTreePanel::GenerateRow)
                .OnGetChildren(this, &SNGonUndoTreePanel::GetChildrenForRow)
                .OnSelectionChanged(this, &SNGonUndoTreePanel::OnRowSelectionChanged)
                .HeaderRow
                (
                    SNew(SHeaderRow)
                    + SHeaderRow::Column(ColTreeName).DefaultLabel(LOCTEXT("ColTree", "Undo Outliner Tree")).FillWidth(0.56f)
                    + SHeaderRow::Column(ColTypeName).DefaultLabel(LOCTEXT("ColType", "Type")).FillWidth(0.16f)
                    + SHeaderRow::Column(ColStepsName).DefaultLabel(LOCTEXT("ColSteps", "Steps")).FillWidth(0.12f)
                    + SHeaderRow::Column(ColMemName).DefaultLabel(LOCTEXT("ColMem", "Mem")).FillWidth(0.16f)
                )
            ]
            // 3. Footer: Epoch + Memory + Status
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
                    .ColorAndOpacity(FLinearColor(0.55f, 0.72f, 0.92f, 1.0f))
                    .Text_Lambda([this]()
                    {
                        if (!Session.IsValid()) return LOCTEXT("NoSess", "No active Undo Session");
                        const FNGonEpochVector E = Session->GetCurrentEpochs();
                        return FText::FromString(FString::Printf(
                            TEXT("TopoEpoch: %llu | UVEpoch: %llu | MEM: %s / %s"),
                            E.TopoEpoch,
                            E.UVEpoch,
                            *FormatBytesCompact(Session->GetTotalMemoryBytes()),
                            *FormatBytesCompact(Session->GetPolicy().MaxMemoryBytes)));
                    })
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
                    .ColorAndOpacity(FLinearColor(0.70f, 0.74f, 0.80f, 1.0f))
                    .Text_Lambda([this] { return FText::FromString(LastStatusText); })
                ]
            ]
        ]
    ];

    SetSession(InSession);
}

SNGonUndoTreePanel::~SNGonUndoTreePanel()
{
    if (Session.IsValid() && TreeChangedHandle.IsValid())
    {
        Session->OnTreeChanged.Remove(TreeChangedHandle);
    }
}

void SNGonUndoTreePanel::SetSession(const TSharedPtr<FNGonUndoSession>& InSession)
{
    if (Session.IsValid() && TreeChangedHandle.IsValid())
    {
        Session->OnTreeChanged.Remove(TreeChangedHandle);
        TreeChangedHandle.Reset();
    }
    Session = InSession;
    if (Session.IsValid())
    {
        TreeChangedHandle = Session->OnTreeChanged.AddSP(this, &SNGonUndoTreePanel::RebuildTree);
    }
    RebuildTree();
}

void SNGonUndoTreePanel::BuildNodeRowsRecursive(
    const FNGonUndoContext& Ctx,
    uint64 NodeId,
    TArray<TSharedPtr<FNGonUndoTreeRow>>& OutSiblings)
{
    const FNGonUndoNode* Node = Ctx.FindNode(NodeId);
    if (!Node) return;

    TSharedPtr<FNGonUndoTreeRow> Row = MakeShared<FNGonUndoTreeRow>();
    Row->RowKind = FNGonUndoTreeRow::ERowKind::HistoryNode;
    Row->ContextId = Ctx.Id;
    Row->NodeId = Node->NodeId;
    Row->Label = Node->Label;
    Row->TypeTag = FNGonUndoSession::EditKindToTag(Node->EditKind);
    Row->StepsText = Node->ChildIds.Num() > 1 ? FString::Printf(TEXT("B%d"), Node->ChildIds.Num()) : TEXT("-");
    Row->MemText = FormatBytesCompact(Node->CachedBytes);
    Row->bIsActive = (Node->NodeId == Ctx.ActiveNodeId);
    Row->bIsEpochLocked = Session.IsValid() && Session->IsNodeEpochLocked(Ctx.Id, Node->NodeId, &Row->LockTooltip);

    for (const uint64 ChildId : Node->ChildIds)
    {
        BuildNodeRowsRecursive(Ctx, ChildId, Row->Children);
    }
    OutSiblings.Add(MoveTemp(Row));
}

void SNGonUndoTreePanel::ExpandAllRowsRecursive(const TArray<TSharedPtr<FNGonUndoTreeRow>>& Items)
{
    if (!TreeView.IsValid()) return;
    for (const TSharedPtr<FNGonUndoTreeRow>& Item : Items)
    {
        if (Item.IsValid() && !Item->Children.IsEmpty())
        {
            TreeView->SetItemExpansion(Item, true);
            ExpandAllRowsRecursive(Item->Children);
        }
    }
}

void SNGonUndoTreePanel::RebuildTree()
{
    RootRows.Reset();
    if (!Session.IsValid())
    {
        if (TreeView.IsValid()) TreeView->RequestTreeRefresh();
        return;
    }

    TMap<ENGonUndoContextId, ENGonUndoSelectState> EffStates;
    TMap<ENGonUndoContextId, FString> AutoReasons;
    Session->ComputeEffectiveSelection(EffStates, AutoReasons);

    TSharedPtr<FNGonUndoTreeRow> RootRow = MakeShared<FNGonUndoTreeRow>();
    RootRow->RowKind = FNGonUndoTreeRow::ERowKind::RootSession;
    RootRow->ContextId = ENGonUndoContextId::Root;
    RootRow->Label = Session->GetSessionName();
    RootRow->TypeTag = TEXT("ROOT");
    RootRow->MemText = FormatBytesCompact(Session->GetTotalMemoryBytes());

    int32 TotalNodes = 0;
    bool bAllChecked = true;
    bool bAnyChecked = false;

    for (const ENGonUndoContextId CtxId : Session->GetOrderedContextIds())
    {
        const FNGonUndoContext* Ctx = Session->GetContext(CtxId);
        if (!Ctx) continue;
        bAllChecked &= Ctx->bUserChecked;
        bAnyChecked |= Ctx->bUserChecked;
        TotalNodes += Ctx->Nodes.Num();

        const ENGonUndoSelectState SelState = EffStates.FindRef(CtxId);
        TSharedPtr<FNGonUndoTreeRow> BranchRow = MakeShared<FNGonUndoTreeRow>();
        BranchRow->RowKind = FNGonUndoTreeRow::ERowKind::ContextBranch;
        BranchRow->ContextId = CtxId;
        BranchRow->Label = Ctx->DisplayName;
        if (!Ctx->DependsOn.IsEmpty())
        {
            BranchRow->Label += FString::Printf(TEXT(" (<- %s)"), *FNGonUndoSession::ContextIdToName(Ctx->DependsOn[0]));
        }
        BranchRow->SelectState = SelState;
        BranchRow->TypeTag = (SelState == ENGonUndoSelectState::AutoLinked) ? TEXT("LINKED")
            : (SelState == ENGonUndoSelectState::UserChecked) ? TEXT("ACTIVE") : TEXT("FROZEN");
        BranchRow->StepsText = FString::Printf(TEXT("%d/%d"), Ctx->GetDepthOfActive(), FMath::Max(0, Ctx->Nodes.Num() - 1));
        BranchRow->MemText = FormatBytesCompact(Ctx->GetTotalBytes());

        BuildNodeRowsRecursive(*Ctx, Ctx->RootNodeId, BranchRow->Children);
        RootRow->Children.Add(MoveTemp(BranchRow));
    }

    RootRow->SelectState = bAllChecked ? ENGonUndoSelectState::UserChecked
        : bAnyChecked ? ENGonUndoSelectState::AutoLinked : ENGonUndoSelectState::None;
    RootRow->StepsText = FString::FromInt(TotalNodes);
    RootRows.Add(MoveTemp(RootRow));

    if (TreeView.IsValid())
    {
        bSuppressSelectionJump = true;
        TreeView->RequestTreeRefresh();
        ExpandAllRowsRecursive(RootRows);
        bSuppressSelectionJump = false;
    }
}

TSharedRef<ITableRow> SNGonUndoTreePanel::GenerateRow(
    TSharedPtr<FNGonUndoTreeRow> Item,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(SNGonUndoTreeTableRow, OwnerTable, Item, Session);
}

void SNGonUndoTreePanel::GetChildrenForRow(
    TSharedPtr<FNGonUndoTreeRow> Item,
    TArray<TSharedPtr<FNGonUndoTreeRow>>& OutChildren)
{
    if (Item.IsValid())
    {
        OutChildren = Item->Children;
    }
}

void SNGonUndoTreePanel::OnRowSelectionChanged(TSharedPtr<FNGonUndoTreeRow> Item, ESelectInfo::Type SelectInfo)
{
    if (bSuppressSelectionJump || SelectInfo == ESelectInfo::Direct || !Item.IsValid() || !Session.IsValid())
    {
        return;
    }
    if (Item->RowKind == FNGonUndoTreeRow::ERowKind::HistoryNode)
    {
        Session->JumpToNode(Item->ContextId, Item->NodeId, &LastStatusText);
    }
}

#undef LOCTEXT_NAMESPACE

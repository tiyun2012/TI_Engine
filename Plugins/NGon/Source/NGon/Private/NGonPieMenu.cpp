#include "NGonPieMenu.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "InputCoreTypes.h"

namespace
{
constexpr float PieTau = 2.0f * UE_PI;

bool IsInsideBox(const FBox2D& Box, const FVector2D& P)
{
    return P.X >= Box.Min.X && P.X < Box.Max.X && P.Y >= Box.Min.Y && P.Y < Box.Max.Y;
}

void DrawFilledCircle(FCanvas& Canvas, const FVector2D& Center, float Radius, const FLinearColor& Color)
{
    if (Radius <= 0.5f) return;
    // Draw clean anti-aliased horizontal scanline disk via thin tiles
    const int32 Steps = FMath::Clamp(FMath::CeilToInt(Radius), 6, 36);
    for (int32 i = -Steps; i <= Steps; ++i)
    {
        const float Dy = (static_cast<float>(i) / static_cast<float>(Steps)) * Radius;
        const float HalfW = FMath::Sqrt(FMath::Max(0.0f, Radius * Radius - Dy * Dy));
        if (HalfW <= 0.25f) continue;
        const float SliceH = (Radius / static_cast<float>(Steps)) * 1.35f;
        FCanvasTileItem Tile(
            FVector2D(Center.X - HalfW, Center.Y + Dy - SliceH * 0.5f),
            FVector2D(HalfW * 2.0f, SliceH),
            Color);
        Tile.BlendMode = SE_BLEND_Translucent;
        Canvas.DrawItem(Tile);
    }
}

void DrawCircleOutline(FCanvas& Canvas, const FVector2D& Center, float Radius, const FLinearColor& Color, float Thickness)
{
    if (Radius <= 0.5f) return;
    constexpr int32 Segs = 32;
    FVector2D Prev(Center.X + Radius, Center.Y);
    for (int32 i = 1; i <= Segs; ++i)
    {
        const float Angle = (PieTau * static_cast<float>(i)) / static_cast<float>(Segs);
        const FVector2D Curr(Center.X + FMath::Cos(Angle) * Radius, Center.Y + FMath::Sin(Angle) * Radius);
        FCanvasLineItem Line(Prev, Curr);
        Line.SetColor(Color);
        Line.LineThickness = Thickness;
        Canvas.DrawItem(Line);
        Prev = Curr;
    }
}

void DrawFilledRect(FCanvas& Canvas, const FBox2D& Rect, const FLinearColor& Color)
{
    const FVector2D Sz = Rect.GetSize();
    if (Sz.X <= 0.5 || Sz.Y <= 0.5) return;
    FCanvasTileItem Tile(Rect.Min, Sz, Color);
    Tile.BlendMode = SE_BLEND_Translucent;
    Canvas.DrawItem(Tile);
}

void DrawRectOutline(FCanvas& Canvas, const FBox2D& Rect, const FLinearColor& Color, float Thickness)
{
    const FVector2D Sz = Rect.GetSize();
    if (Sz.X <= 0.5 || Sz.Y <= 0.5) return;
    FCanvasBoxItem Box(Rect.Min, Sz);
    Box.SetColor(Color);
    Box.LineThickness = Thickness;
    Canvas.DrawItem(Box);
}

void DrawCenteredText(FCanvas& Canvas, UFont* Font, const FVector2D& Center, const FString& Text, const FLinearColor& Color)
{
    if (!Font || Text.IsEmpty()) return;
    int32 W = 0, H = 0;
    Font->GetStringHeightAndWidth(Text, H, W);
    FCanvasTextItem Item(FVector2D(Center.X - W * 0.5, Center.Y - H * 0.5), FText::FromString(Text), Font, Color);
    Item.EnableShadow(FLinearColor(0.0f, 0.0f, 0.0f, 0.85f));
    Canvas.DrawItem(Item);
}
} // anonymous namespace

void FNGonPieMenu::Open(const FVector2D& InCenterLogical, TArray<FNGonPieItem> InItems, TFunction<void(const FString&)> InOnChoose)
{
    if (InItems.IsEmpty() || InItems.Num() > 16 || !InOnChoose)
    {
        Close();
        return;
    }
    RequestedCenter = InCenterLogical;
    Center = InCenterLogical;
    Items = MoveTemp(InItems);
    OnChoose = MoveTemp(InOnChoose);
    Highlighted.Reset();
    Branches.Reset();
    BranchDirection = 1;
    bOpen = true;
}

void FNGonPieMenu::Close()
{
    bOpen = false;
    Highlighted.Reset();
    Branches.Reset();
    Items.Reset();
    OnChoose = nullptr;
}

const FNGonPieItem* FNGonPieMenu::FindItem(const TArray<int32>& Path) const
{
    if (Path.IsEmpty()) return nullptr;
    const TArray<FNGonPieItem>* List = &Items;
    const FNGonPieItem* Curr = nullptr;
    for (const int32 Index : Path)
    {
        if (!List->IsValidIndex(Index)) return nullptr;
        Curr = &(*List)[Index];
        List = &Curr->Children;
    }
    return Curr;
}

const TArray<FNGonPieItem>* FNGonPieMenu::FindChildren(const TArray<int32>& Path) const
{
    const FNGonPieItem* Item = FindItem(Path);
    return Item ? &Item->Children : nullptr;
}

TArray<int32> FNGonPieMenu::SelectedPath(int32 Level) const
{
    TArray<int32> Path;
    if (Branches.IsValidIndex(Level))
    {
        Path = Branches[Level].ParentPath;
        Path.Add(Branches[Level].Selected);
    }
    return Path;
}

void FNGonPieMenu::Arrange(const FVector2D& ViewLogicalSize)
{
    if (!bOpen) return;
    const double Margin = ButtonDiameter * 0.5 + 8.0;
    const double RadialExtent = VisualRadius + Margin;
    Center.X = FMath::Clamp(RequestedCenter.X, RadialExtent, FMath::Max(RadialExtent, ViewLogicalSize.X - RadialExtent));
    Center.Y = FMath::Clamp(RequestedCenter.Y, RadialExtent, FMath::Max(RadialExtent, ViewLogicalSize.Y - RadialExtent));
    ArrangeBranches(ViewLogicalSize);
}

void FNGonPieMenu::RevealBranch(int32 Level)
{
    if (!Branches.IsValidIndex(Level)) return;
    FBranchLevel& Branch = Branches[Level];
    const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
    if (!Children || Children->IsEmpty() || Branch.Visible <= 0) return;
    const int32 MaxFirst = FMath::Max(0, Children->Num() - FMath::Min(Children->Num(), Branch.Visible));
    Branch.First = FMath::Clamp(Branch.First, 0, MaxFirst);
    if (Branch.Selected < Branch.First) Branch.First = Branch.Selected;
    if (Branch.Selected >= Branch.First + Branch.Visible) Branch.First = Branch.Selected - Branch.Visible + 1;
}

void FNGonPieMenu::ArrangeBranches(const FVector2D& ViewLogicalSize)
{
    if (Branches.IsEmpty()) return;
    const double Width = FMath::Min(static_cast<double>(BranchWidth), FMath::Max(1.0, ViewLogicalSize.X));

    for (int32 Level = 0; Level < Branches.Num(); ++Level)
    {
        FBranchLevel& Branch = Branches[Level];
        const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
        if (!Children || Children->IsEmpty())
        {
            Branches.SetNum(Level);
            break;
        }
        const int32 MaxRowsByHeight = FMath::Max(1, FMath::FloorToInt(ViewLogicalSize.Y / BranchRowHeight));
        Branch.Visible = FMath::Min3(MaxBranchRows, Children->Num(), MaxRowsByHeight);
        const double Height = FMath::Min(ViewLogicalSize.Y, static_cast<double>(Branch.Visible) * BranchRowHeight);

        double Left = 0.0;
        double Top = 0.0;
        if (Level == 0)
        {
            if (!Highlighted.IsSet() || !Items.IsValidIndex(Highlighted.GetValue()))
            {
                Branches.Reset();
                return;
            }
            const float Step = PieTau / static_cast<float>(Items.Num());
            const float Angle = StartAngle + Step * static_cast<float>(Highlighted.GetValue());
            const FVector2D RootPt(Center.X + FMath::Cos(Angle) * VisualRadius, Center.Y + FMath::Sin(Angle) * VisualRadius);

            int32 Dir = BranchDirection;
            const double Candidate = (Dir > 0)
                ? (RootPt.X + ButtonDiameter * 0.5 + BranchGap)
                : (RootPt.X - ButtonDiameter * 0.5 - BranchGap - Width);
            if (Dir > 0 && Candidate + Width > ViewLogicalSize.X && RootPt.X - ButtonDiameter * 0.5 - BranchGap - Width >= 0.0)
            {
                Dir = -1;
            }
            else if (Dir < 0 && Candidate < 0.0 && RootPt.X + ButtonDiameter * 0.5 + BranchGap + Width <= ViewLogicalSize.X)
            {
                Dir = 1;
            }
            BranchDirection = Dir;
            Left = (Dir > 0)
                ? (RootPt.X + ButtonDiameter * 0.5 + BranchGap)
                : (RootPt.X - ButtonDiameter * 0.5 - BranchGap - Width);
            Top = RootPt.Y - BranchRowHeight * 0.5;
        }
        else
        {
            const FBranchLevel& Parent = Branches[Level - 1];
            const double RowTop = Parent.Bounds.Min.Y + static_cast<double>(Parent.Selected - Parent.First) * BranchRowHeight;
            Left = (BranchDirection > 0) ? (Parent.Bounds.Max.X + BranchGap) : (Parent.Bounds.Min.X - BranchGap - Width);
            if (Left + Width > ViewLogicalSize.X && Parent.Bounds.Min.X - BranchGap - Width >= 0.0)
            {
                Left = Parent.Bounds.Min.X - BranchGap - Width;
            }
            else if (Left < 0.0 && Parent.Bounds.Max.X + BranchGap + Width <= ViewLogicalSize.X)
            {
                Left = Parent.Bounds.Max.X + BranchGap;
            }
            Top = RowTop;
        }

        Left = FMath::Clamp(Left, 0.0, FMath::Max(0.0, ViewLogicalSize.X - Width));
        Top = FMath::Clamp(Top, 0.0, FMath::Max(0.0, ViewLogicalSize.Y - Height));
        Branch.Bounds = FBox2D(FVector2D(Left, Top), FVector2D(Left + Width, Top + Height));
        RevealBranch(Level);
    }
}

TOptional<int32> FNGonPieMenu::Sector(const FVector2D& Point) const
{
    if (Items.IsEmpty() || FVector2D::Distance(Point, Center) < DeadZoneRadius)
    {
        return TOptional<int32>();
    }
    const float Step = PieTau / static_cast<float>(Items.Num());
    float Angle = FMath::Atan2(static_cast<float>(Point.Y - Center.Y), static_cast<float>(Point.X - Center.X)) - StartAngle + Step * 0.5f;
    Angle = FMath::Fmod(Angle, PieTau);
    if (Angle < 0.0f) Angle += PieTau;
    return FMath::Min(Items.Num() - 1, static_cast<int32>(Angle / Step));
}

TOptional<int32> FNGonPieMenu::StepEnabled(int32 Start, int32 Direction) const
{
    if (Items.IsEmpty()) return TOptional<int32>();
    int32 Index = FMath::Clamp(Start, 0, Items.Num() - 1);
    for (int32 n = 0; n < Items.Num(); ++n)
    {
        Index = (Direction >= 0) ? ((Index + 1) % Items.Num()) : ((Index + Items.Num() - 1) % Items.Num());
        if (Items[Index].bEnabled) return Index;
    }
    return TOptional<int32>();
}

bool FNGonPieMenu::StepBranch(int32 Level, int32 Direction)
{
    if (!Branches.IsValidIndex(Level)) return false;
    FBranchLevel& Branch = Branches[Level];
    const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
    if (!Children || Children->IsEmpty()) return false;

    int32 Index = FMath::Clamp(Branch.Selected, 0, Children->Num() - 1);
    for (int32 n = 0; n < Children->Num(); ++n)
    {
        Index = (Direction >= 0) ? ((Index + 1) % Children->Num()) : ((Index + Children->Num() - 1) % Children->Num());
        if ((*Children)[Index].bEnabled)
        {
            Branch.Selected = Index;
            Branches.SetNum(Level + 1);
            RevealBranch(Level);
            return true;
        }
    }
    return false;
}

void FNGonPieMenu::OpenRootBranch(int32 RootIndex)
{
    if (!Items.IsValidIndex(RootIndex) || !Items[RootIndex].bEnabled || Items[RootIndex].Children.IsEmpty())
    {
        Branches.Reset();
        return;
    }
    if (!Branches.IsEmpty() && Branches[0].ParentPath.Num() == 1 && Branches[0].ParentPath[0] == RootIndex)
    {
        return;
    }
    const float Step = PieTau / static_cast<float>(Items.Num());
    const float Angle = StartAngle + Step * static_cast<float>(RootIndex);
    BranchDirection = (FMath::Cos(Angle) < -0.15f) ? -1 : 1;

    FBranchLevel Root;
    Root.ParentPath = { RootIndex };
    Root.Selected = 0;
    if (!Items[RootIndex].Children[0].bEnabled)
    {
        for (int32 i = 0; i < Items[RootIndex].Children.Num(); ++i)
        {
            if (Items[RootIndex].Children[i].bEnabled) { Root.Selected = i; break; }
        }
    }
    Branches.Reset();
    Branches.Add(MoveTemp(Root));
}

void FNGonPieMenu::OpenSubBranch(int32 Level)
{
    if (!Branches.IsValidIndex(Level)) return;
    const TArray<int32> Path = SelectedPath(Level);
    const FNGonPieItem* Sel = FindItem(Path);
    Branches.SetNum(Level + 1);
    if (!Sel || !Sel->bEnabled || Sel->Children.IsEmpty()) return;

    FBranchLevel Next;
    Next.ParentPath = Path;
    Next.Selected = 0;
    if (!Sel->Children[0].bEnabled)
    {
        for (int32 i = 0; i < Sel->Children.Num(); ++i)
        {
            if (Sel->Children[i].bEnabled) { Next.Selected = i; break; }
        }
    }
    Branches.Add(MoveTemp(Next));
}

void FNGonPieMenu::UpdateHighlight(const FVector2D& Point)
{
    TOptional<int32> Index = Sector(Point);
    if (Index.IsSet() && (!Items.IsValidIndex(Index.GetValue()) || !Items[Index.GetValue()].bEnabled))
    {
        Index.Reset();
    }
    if (Index != Highlighted)
    {
        Highlighted = Index;
        if (Highlighted.IsSet()) OpenRootBranch(Highlighted.GetValue());
        else Branches.Reset();
    }
    else if (Highlighted.IsSet())
    {
        OpenRootBranch(Highlighted.GetValue());
    }
}

TOptional<int32> FNGonPieMenu::BranchAt(const FVector2D& Point) const
{
    for (int32 i = Branches.Num() - 1; i >= 0; --i)
    {
        if (IsInsideBox(Branches[i].Bounds, Point)) return i;
    }
    return TOptional<int32>();
}

TOptional<int32> FNGonPieMenu::BranchRowAt(int32 Level, const FVector2D& Point) const
{
    if (!Branches.IsValidIndex(Level) || !IsInsideBox(Branches[Level].Bounds, Point)) return TOptional<int32>();
    const FBranchLevel& Branch = Branches[Level];
    const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
    if (!Children) return TOptional<int32>();
    const int32 Local = FMath::Max(0, FMath::FloorToInt((Point.Y - Branch.Bounds.Min.Y) / BranchRowHeight));
    const int32 Index = Branch.First + Local;
    return Children->IsValidIndex(Index) ? TOptional<int32>(Index) : TOptional<int32>();
}

void FNGonPieMenu::ActivateItem(const FNGonPieItem& Item)
{
    if (!Item.bEnabled || !Item.Children.IsEmpty() || Item.Command.IsEmpty()) return;
    TFunction<void(const FString&)> Callback = MoveTemp(OnChoose);
    const FString Cmd = Item.Command;
    Close();
    if (Callback)
    {
        Callback(Cmd);
    }
}

void FNGonPieMenu::OnMouseMove(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos)
{
    if (!bOpen) return;
    Arrange(ViewLogicalSize);
    if (const TOptional<int32> Level = BranchAt(LogicalPos); Level.IsSet())
    {
        if (const TOptional<int32> Row = BranchRowAt(Level.GetValue(), LogicalPos); Row.IsSet())
        {
            FBranchLevel& Branch = Branches[Level.GetValue()];
            const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
            if (Children && Children->IsValidIndex(Row.GetValue()) && (*Children)[Row.GetValue()].bEnabled)
            {
                Branch.Selected = Row.GetValue();
                Branches.SetNum(Level.GetValue() + 1);
                OpenSubBranch(Level.GetValue());
                Arrange(ViewLogicalSize);
            }
        }
        return;
    }
    UpdateHighlight(LogicalPos);
    Arrange(ViewLogicalSize);
}

bool FNGonPieMenu::OnMouseDown(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos)
{
    if (!bOpen) return false;
    OnMouseMove(ViewLogicalSize, LogicalPos);
    return true;
}

bool FNGonPieMenu::OnMouseUp(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos)
{
    if (!bOpen) return false;
    Arrange(ViewLogicalSize);

    if (const TOptional<int32> Level = BranchAt(LogicalPos); Level.IsSet())
    {
        if (const TOptional<int32> Row = BranchRowAt(Level.GetValue(), LogicalPos); Row.IsSet())
        {
            FBranchLevel& Branch = Branches[Level.GetValue()];
            const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
            if (Children && Children->IsValidIndex(Row.GetValue()) && (*Children)[Row.GetValue()].bEnabled)
            {
                Branch.Selected = Row.GetValue();
                Branches.SetNum(Level.GetValue() + 1);
                const FNGonPieItem& Item = (*Children)[Row.GetValue()];
                if (Item.Children.IsEmpty()) ActivateItem(Item);
                else { OpenSubBranch(Level.GetValue()); Arrange(ViewLogicalSize); }
            }
        }
        return true;
    }

    UpdateHighlight(LogicalPos);
    Arrange(ViewLogicalSize);
    if (Highlighted.IsSet() && Items.IsValidIndex(Highlighted.GetValue()))
    {
        const FNGonPieItem& Root = Items[Highlighted.GetValue()];
        if (Root.Children.IsEmpty()) ActivateItem(Root);
        else OpenRootBranch(Highlighted.GetValue());
    }
    else
    {
        Close();
    }
    return true;
}

bool FNGonPieMenu::OnMouseWheel(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos, float WheelDelta)
{
    if (!bOpen) return false;
    Arrange(ViewLogicalSize);
    if (const TOptional<int32> Level = BranchAt(LogicalPos); Level.IsSet())
    {
        FBranchLevel& Branch = Branches[Level.GetValue()];
        const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
        if (Children)
        {
            const int32 MaxFirst = FMath::Max(0, Children->Num() - Branch.Visible);
            Branch.First = FMath::Clamp(Branch.First - FMath::RoundToInt(WheelDelta * 2.0f), 0, MaxFirst);
        }
    }
    return true;
}

bool FNGonPieMenu::OnKeyDown(const FVector2D& ViewLogicalSize, const FKey& Key, bool bIsRepeat)
{
    if (!bOpen) return false;
    if (Key == EKeys::Escape) { Close(); return true; }
    if (bIsRepeat && Key == EKeys::SpaceBar) return true;
    Arrange(ViewLogicalSize);

    if (!Branches.IsEmpty())
    {
        const int32 Level = Branches.Num() - 1;
        FBranchLevel& Branch = Branches[Level];
        const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
        if (!Children || Children->IsEmpty()) return true;

        if (Key == EKeys::Down) { StepBranch(Level, 1); Arrange(ViewLogicalSize); return true; }
        if (Key == EKeys::Up) { StepBranch(Level, -1); Arrange(ViewLogicalSize); return true; }
        if (Key == EKeys::Right) { OpenSubBranch(Level); Arrange(ViewLogicalSize); return true; }
        if (Key == EKeys::Left)
        {
            if (Branches.Num() > 1) Branches.Pop(); else Branches.Reset();
            return true;
        }
        if (Key == EKeys::Enter || Key == EKeys::SpaceBar)
        {
            const FNGonPieItem& Sel = (*Children)[Branch.Selected];
            if (Sel.bEnabled)
            {
                if (Sel.Children.IsEmpty()) ActivateItem(Sel);
                else { OpenSubBranch(Level); Arrange(ViewLogicalSize); }
            }
            return true;
        }
        return true;
    }

    if (Key == EKeys::Right || Key == EKeys::Down)
    {
        Highlighted = StepEnabled(Highlighted.Get(Items.Num() - 1), 1);
        if (Highlighted.IsSet()) OpenRootBranch(Highlighted.GetValue());
        Arrange(ViewLogicalSize);
        return true;
    }
    if (Key == EKeys::Left || Key == EKeys::Up)
    {
        Highlighted = StepEnabled(Highlighted.Get(0), -1);
        if (Highlighted.IsSet()) OpenRootBranch(Highlighted.GetValue());
        Arrange(ViewLogicalSize);
        return true;
    }
    if (Key == EKeys::Enter || Key == EKeys::SpaceBar)
    {
        if (Highlighted.IsSet() && Items.IsValidIndex(Highlighted.GetValue()))
        {
            const FNGonPieItem& Sel = Items[Highlighted.GetValue()];
            if (Sel.Children.IsEmpty()) ActivateItem(Sel);
            else { OpenRootBranch(Highlighted.GetValue()); Arrange(ViewLogicalSize); }
        }
        return true;
    }
    return true;
}

void FNGonPieMenu::Draw(FCanvas& Canvas, const FVector2D& ViewLogicalSize) const
{
    if (!bOpen || Items.IsEmpty()) return;

    UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;

    const FLinearColor Accent(0.24f, 0.78f, 1.0f, 1.0f);
    const FLinearColor Track(0.085f, 0.095f, 0.12f, 0.94f);
    const FLinearColor ActiveBg(0.14f, 0.24f, 0.36f, 0.98f);
    const FLinearColor SurfaceBg(0.06f, 0.068f, 0.085f, 0.96f);
    const FLinearColor BorderCol(0.22f, 0.27f, 0.35f, 0.95f);
    const FLinearColor TextCol(0.92f, 0.95f, 0.98f, 1.0f);
    const FLinearColor MutedCol(0.55f, 0.60f, 0.68f, 0.90f);

    const float Step = PieTau / static_cast<float>(Items.Num());
    const float HalfBtn = ButtonDiameter * 0.5f;

    // 1. Subtle radial connector ring
    DrawCircleOutline(Canvas, Center, VisualRadius, FLinearColor(0.22f, 0.28f, 0.38f, 0.40f), 1.2f);

    // 2. Radial Root Buttons
    for (int32 i = 0; i < Items.Num(); ++i)
    {
        const float Angle = StartAngle + Step * static_cast<float>(i);
        const FVector2D Pos(Center.X + FMath::Cos(Angle) * VisualRadius, Center.Y + FMath::Sin(Angle) * VisualRadius);
        const bool bSel = Highlighted.IsSet() && Highlighted.GetValue() == i;
        const FNGonPieItem& Item = Items[i];

        if (bSel)
        {
            FCanvasLineItem Spoke(Center, Pos);
            Spoke.SetColor(FLinearColor(0.24f, 0.78f, 1.0f, 0.55f));
            Spoke.LineThickness = 1.6f;
            Canvas.DrawItem(Spoke);

            DrawFilledCircle(Canvas, Pos, HalfBtn + 2.5f, Accent);
        }

        DrawFilledCircle(Canvas, Pos, HalfBtn, Item.bEnabled ? (bSel ? ActiveBg : Track) : SurfaceBg);
        DrawCircleOutline(Canvas, Pos, HalfBtn, bSel ? Accent : BorderCol, bSel ? 1.8f : 1.1f);

        const FString Tag = Item.ShortTag.IsEmpty() ? Item.Label.Left(4) : Item.ShortTag;
        DrawCenteredText(Canvas, Font, Pos, Tag, Item.bEnabled ? (bSel ? Accent : TextCol) : MutedCol);

        // Outward branch indicator dot
        if (!Item.Children.IsEmpty())
        {
            const FVector2D DotPos(Pos.X + FMath::Cos(Angle) * (HalfBtn - 4.0f), Pos.Y + FMath::Sin(Angle) * (HalfBtn - 4.0f));
            DrawFilledCircle(Canvas, DotPos, 2.6f, Item.bEnabled ? Accent : MutedCol);
        }
    }

    // 3. Center Dead-Zone Hub + Highlighted Label Pill
    const float HubR = DeadZoneRadius * 0.72f;
    DrawFilledCircle(Canvas, Center, HubR, Track);
    DrawCircleOutline(Canvas, Center, HubR, BorderCol, 1.2f);
    DrawFilledCircle(Canvas, Center, 2.5f, Highlighted.IsSet() ? Accent : MutedCol);

    const FString CenterLabel = (Highlighted.IsSet() && Items.IsValidIndex(Highlighted.GetValue()))
        ? Items[Highlighted.GetValue()].Label
        : TEXT("Move to select");
    const FVector2D LabelPos(Center.X, Center.Y + DeadZoneRadius + 14.0);
    if (Font)
    {
        int32 W = 0, H = 0;
        Font->GetStringHeightAndWidth(CenterLabel, H, W);
        const FBox2D Pill(
            FVector2D(LabelPos.X - W * 0.5 - 8.0, LabelPos.Y - H * 0.5 - 3.0),
            FVector2D(LabelPos.X + W * 0.5 + 8.0, LabelPos.Y + H * 0.5 + 3.0));
        DrawFilledRect(Canvas, Pill, FLinearColor(0.03f, 0.035f, 0.045f, 0.88f));
        DrawRectOutline(Canvas, Pill, Highlighted.IsSet() ? Accent : BorderCol, 1.0f);
        DrawCenteredText(Canvas, Font, LabelPos, CenterLabel, Highlighted.IsSet() ? TextCol : MutedCol);
    }

    // 4. List-Shaped Fly-Out Branches
    for (int32 Level = 0; Level < Branches.Num(); ++Level)
    {
        const FBranchLevel& Branch = Branches[Level];
        const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
        if (!Children || Children->IsEmpty()) continue;

        const FBox2D B = Branch.Bounds;
        // Drop shadow + surface + border
        DrawFilledRect(Canvas, FBox2D(B.Min + FVector2D(-2.0, 2.0), B.Max + FVector2D(2.0, 4.0)), FLinearColor(0.0f, 0.0f, 0.0f, 0.35f));
        DrawFilledRect(Canvas, B, SurfaceBg);
        DrawRectOutline(Canvas, B, BorderCol, 1.2f);

        for (int32 Row = 0; Row < Branch.Visible; ++Row)
        {
            const int32 Idx = Branch.First + Row;
            if (!Children->IsValidIndex(Idx)) break;
            const FNGonPieItem& Item = (*Children)[Idx];

            const double RowTop = B.Min.Y + static_cast<double>(Row) * BranchRowHeight;
            const FBox2D RowRect(FVector2D(B.Min.X + 3.0, RowTop + 2.0), FVector2D(B.Max.X - 3.0, FMath::Min(B.Max.Y - 2.0, RowTop + BranchRowHeight - 2.0)));
            const bool bRowSel = (Idx == Branch.Selected);

            if (bRowSel)
            {
                DrawFilledRect(Canvas, RowRect, ActiveBg);
            }

            if (Font)
            {
                int32 W = 0, H = 0;
                Font->GetStringHeightAndWidth(Item.Label, H, W);
                const FVector2D TextPt(B.Min.X + 10.0, RowTop + FMath::Max(0.0, (BranchRowHeight - H) * 0.5));
                FCanvasTextItem TItem(TextPt, FText::FromString(Item.Label), Font,
                    Item.bEnabled ? (bRowSel ? Accent : TextCol) : MutedCol);
                TItem.EnableShadow(FLinearColor(0.0f, 0.0f, 0.0f, 0.85f));
                Canvas.DrawItem(TItem);
            }

            // Sub-branch chevron
            if (!Item.Children.IsEmpty())
            {
                const double Cx = B.Max.X - 10.0;
                const double Cy = RowTop + BranchRowHeight * 0.5;
                const FLinearColor ChevCol = Item.bEnabled ? TextCol : MutedCol;
                FCanvasLineItem L1(FVector2D(Cx - 2.5, Cy - 3.5), FVector2D(Cx + 1.5, Cy));
                L1.SetColor(ChevCol); L1.LineThickness = 1.3f; Canvas.DrawItem(L1);
                FCanvasLineItem L2(FVector2D(Cx + 1.5, Cy), FVector2D(Cx - 2.5, Cy + 3.5));
                L2.SetColor(ChevCol); L2.LineThickness = 1.3f; Canvas.DrawItem(L2);
            }
        }
    }
}
// NGon Unified ABI Sync v5.8.2

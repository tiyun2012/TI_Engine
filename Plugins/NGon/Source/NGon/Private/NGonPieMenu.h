#pragma once

#include "CoreMinimal.h"

class FCanvas;

struct FNGonPieItem
{
    FString Label;
    FString ShortTag;
    FString Command;
    bool bEnabled = true;
    TArray<FNGonPieItem> Children;

    static FNGonPieItem Leaf(const FString& InLabel, const FString& InShortTag, const FString& InCommand, bool bInEnabled = true)
    {
        FNGonPieItem Item;
        Item.Label = InLabel;
        Item.ShortTag = InShortTag;
        Item.Command = InCommand;
        Item.bEnabled = bInEnabled;
        return Item;
    }

    static FNGonPieItem Branch(const FString& InLabel, const FString& InShortTag, TArray<FNGonPieItem> InChildren, bool bInEnabled = true)
    {
        FNGonPieItem Item;
        Item.Label = InLabel;
        Item.ShortTag = InShortTag;
        Item.bEnabled = bInEnabled;
        Item.Children = MoveTemp(InChildren);
        return Item;
    }
};

/**
 * Direct Unreal Engine FCanvas port of tigui::PieMenu:
 * - Radial root ring (1..16 circular items) with dead-zone and flick-to-activate
 * - Auto-clamping within viewport bounds
 * - List-shaped fly-out sub-branches with automatic left/right flip and wheel scroll
 * - Keyboard navigation (Arrows, Home/End, Enter/Space, Escape)
 */
class FNGonPieMenu
{
public:
    float VisualRadius = 86.0f;
    float ButtonDiameter = 48.0f;
    float DeadZoneRadius = 22.0f;
    float StartAngle = -UE_HALF_PI; // 12 o'clock
    float BranchWidth = 176.0f;
    float BranchRowHeight = 26.0f;
    float BranchGap = 8.0f;
    int32 MaxBranchRows = 12;

    void Open(const FVector2D& InCenterLogical, TArray<FNGonPieItem> InItems, TFunction<void(const FString&)> InOnChoose);
    void Close();
    bool IsOpen() const { return bOpen; }

    void Arrange(const FVector2D& ViewLogicalSize);
    void OnMouseMove(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos);
    bool OnMouseDown(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos);
    bool OnMouseUp(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos);
    bool OnMouseWheel(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos, float WheelDelta);
    bool OnKeyDown(const FVector2D& ViewLogicalSize, const FKey& Key, bool bIsRepeat);

    void Draw(FCanvas& Canvas, const FVector2D& ViewLogicalSize) const;

private:
    struct FBranchLevel
    {
        TArray<int32> ParentPath;
        int32 Selected = 0;
        int32 First = 0;
        int32 Visible = 0;
        FBox2D Bounds{ForceInit};
    };

    const FNGonPieItem* FindItem(const TArray<int32>& Path) const;
    const TArray<FNGonPieItem>* FindChildren(const TArray<int32>& Path) const;
    TArray<int32> SelectedPath(int32 Level) const;
    void RevealBranch(int32 Level);
    void ArrangeBranches(const FVector2D& ViewLogicalSize);
    TOptional<int32> Sector(const FVector2D& Point) const;
    TOptional<int32> StepEnabled(int32 Start, int32 Direction) const;
    bool StepBranch(int32 Level, int32 Direction);
    void OpenRootBranch(int32 RootIndex);
    void OpenSubBranch(int32 Level);
    void UpdateHighlight(const FVector2D& Point);
    TOptional<int32> BranchAt(const FVector2D& Point) const;
    TOptional<int32> BranchRowAt(int32 Level, const FVector2D& Point) const;
    void ActivateItem(const FNGonPieItem& Item);

    bool bOpen = false;
    FVector2D RequestedCenter = FVector2D::ZeroVector;
    FVector2D Center = FVector2D::ZeroVector;
    TArray<FNGonPieItem> Items;
    TFunction<void(const FString&)> OnChoose;
    TOptional<int32> Highlighted;
    TArray<FBranchLevel> Branches;
    int32 BranchDirection = 1;
};

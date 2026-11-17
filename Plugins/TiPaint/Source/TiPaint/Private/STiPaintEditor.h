#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "TiPaintDocument.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"

class STiPaintCanvas final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(STiPaintCanvas) {}
    SLATE_END_ARGS()
    void Construct(const FArguments&, const TSharedRef<FTiPaintDocument>& InDocument);
    void Fit();

protected:
    FVector2D ComputeDesiredSize(float) const override { return FVector2D(640.0f, 480.0f); }
    int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&, int32, const FWidgetStyle&, bool) const override;
    FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent&) override;
    FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent&) override;
    FReply OnMouseMove(const FGeometry&, const FPointerEvent&) override;
    FReply OnMouseWheel(const FGeometry&, const FPointerEvent&) override;

private:
    FVector2f ScreenToUV(const FGeometry&, const FVector2D&) const;
    FVector2D UVToScreen(const FGeometry&, const FVector2f&) const;
    TSharedRef<FTiPaintDocument> Document = MakeShared<FTiPaintDocument>(1, 1);
    mutable FVector2f Center = FVector2f(0.5f, 0.5f);
    mutable float Pixels = 450.0f;
    mutable bool bNeedsFit = true;
    bool bStroke = false;
    bool bPan = false;
    FVector2D LastMouse = FVector2D::ZeroVector;
    FVector2f LastUV = FVector2f::ZeroVector;
};

class STiPaintEditor final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(STiPaintEditor) {}
    SLATE_END_ARGS()
    void Construct(const FArguments&);

private:
    TSharedRef<SWidget> MakeToolbar();
    TSharedRef<SWidget> MakeLayers();
    TSharedRef<SWidget> MakeStatus();
    void Refresh();
    void SetMode(ETiPaintMode);
    TSharedRef<SWidget> MakeLayerMenu();
    FReply Export();
    TSharedRef<FTiPaintDocument> Document = MakeShared<FTiPaintDocument>();
    TSharedPtr<STiPaintCanvas> Canvas;
    FText Status;
};
#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "Input/Reply.h"
#include "Styling/SlateBrush.h"
#include "TiPaintDocument.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"

class STiPaintCanvas final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(STiPaintCanvas) {}
    SLATE_END_ARGS()

    void Construct(const FArguments&, const TSharedRef<FTiPaintDocument>& InDocument);
    void SetDocument(const TSharedRef<FTiPaintDocument>& InDocument);
    void Fit();
    void SetUVTriangles(const TArray<FTiPaintUVTriangle>& InTriangles);
    void SetShowUVs(bool bInShow) { bShowUVs = bInShow; Invalidate(EInvalidateWidgetReason::Paint); }
    bool IsShowingUVs() const { return bShowUVs; }

protected:
    FVector2D ComputeDesiredSize(float) const override { return FVector2D(640.0f, 480.0f); }
    int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&, int32, const FWidgetStyle&, bool) const override;
    FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent&) override;
    FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent&) override;
    FReply OnMouseMove(const FGeometry&, const FPointerEvent&) override;
    void OnMouseLeave(const FPointerEvent&) override;
    FReply OnMouseWheel(const FGeometry&, const FPointerEvent&) override;

private:
    FVector2f ScreenToUV(const FGeometry&, const FVector2D&) const;
    FVector2D UVToScreen(const FGeometry&, const FVector2f&) const;
    void SyncGpuTextureIfNeeded() const;
    void DrawBrushCircle(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FVector2f& UV, const FLinearColor& Color) const;

    TSharedRef<FTiPaintDocument> Document = MakeShared<FTiPaintDocument>(1, 1);
    mutable TStrongObjectPtr<UTexture2D> CanvasTexture;
    mutable FSlateBrush CanvasBrush;
    mutable uint64 CachedRevision = 0;
    mutable int32 CachedTexW = 0;
    mutable int32 CachedTexH = 0;

    mutable FVector2f Center = FVector2f(0.5f, 0.5f);
    mutable float Pixels = 450.0f;
    mutable bool bNeedsFit = true;
    bool bShowUVs = true;
    bool bStroke = false;
    bool bPan = false;
    bool bHovering = false;
    FVector2D LastMouse = FVector2D::ZeroVector;
    FVector2f LastUV = FVector2f::ZeroVector;
    TArray<FTiPaintUVTriangle> UVTriangles;
};

class STiPaintEditor final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(STiPaintEditor) {}
    SLATE_END_ARGS()

    void Construct(const FArguments&);

    /** Show the active mesh's UVs and receive the composited paint texture. */
    void SetUVTriangles(const TArray<FTiPaintUVTriangle>& InTriangles);
    void SetPaintedCallback(TFunction<void(const TArray<FColor>&, int32, int32)> InCallback);
    void SetPaint3DModeCallback(TFunction<void(bool)> InCallback);
    void SetPaint3DMode(bool bEnabled);
    bool IsPaint3DMode() const { return bPaint3DMode; }
    void ResetForMesh(const TArray<FTiPaintUVTriangle>& InTriangles, const TArray<FTiPaintSubMeshInfo>& InSubMeshes = {});
    TSharedRef<FTiPaintDocument> GetDocument() const { return Document; }

private:
    TSharedRef<SWidget> MakeToolbar();
    TSharedRef<SWidget> MakeInspector();
    TSharedRef<SWidget> MakeBrushPanel();
    TSharedRef<SWidget> MakeLayersPanel();
    TSharedRef<SWidget> MakeStencilProjectionPanel();
    TSharedRef<SWidget> MakeAiJsonPanel();
    TSharedRef<SWidget> MakeStatus();

    TSharedRef<SWidget> MakeLayerMenu();
    TSharedRef<SWidget> MakeBlendModeMenu();
    TSharedRef<SWidget> MakeTipMenu();
    TSharedRef<SWidget> MakeSymmetryMenu();
    TSharedRef<SWidget> MakeStencilTypeMenu();
    TSharedRef<SWidget> MakeProjectionModeMenu();
    TSharedRef<SWidget> MakeSubMeshMenu();

    void Refresh();
    void SetMode(ETiPaintMode Mode);
    FReply Export();
    FReply ImportLayerImage();
    FReply LoadStencilImage();
    FReply RunProjection();
    FReply RunAiJson();

    TSharedRef<FTiPaintDocument> Document = MakeShared<FTiPaintDocument>();
    TSharedPtr<STiPaintCanvas> Canvas;
    int32 ActiveInspectorTab = 0; // 0=Brush, 1=Layers, 2=Stencil/3D, 3=AI JSON
    FText Status;
    FString AiJsonInput;
    FString AiLastOutput;
    TFunction<void(const TArray<FColor>&, int32, int32)> PaintedCallback;
    TFunction<void(bool)> Paint3DModeCallback;
    bool bPaint3DMode = false;
};

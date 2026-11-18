#include "STiPaintEditor.h"

#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformFileManager.h"
#include "IDesktopPlatform.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "TiPaintEditor"

void STiPaintCanvas::Construct(const FArguments&, const TSharedRef<FTiPaintDocument>& InDocument)
{
    Document = InDocument;
}

void STiPaintCanvas::SetDocument(const TSharedRef<FTiPaintDocument>& InDocument)
{
    Document = InDocument;
    bStroke = false;
    bPan = false;
    Fit();
}

void STiPaintCanvas::Fit()
{
    bNeedsFit = true;
    Invalidate(EInvalidateWidgetReason::Paint);
}

void STiPaintCanvas::SetUVTriangles(const TArray<FTiPaintUVTriangle>& InTriangles)
{
    UVTriangles = InTriangles;
    Invalidate(EInvalidateWidgetReason::Paint);
}

FVector2f STiPaintCanvas::ScreenToUV(const FGeometry& Geometry, const FVector2D& Screen) const
{
    const FVector2D Size = Geometry.GetLocalSize();
    if (bNeedsFit) { Pixels = FMath::Max(40.0f, static_cast<float>(FMath::Min(Size.X, Size.Y) * 0.82)); Center = FVector2f(0.5f, 0.5f); bNeedsFit = false; }
    const FVector2D C = Size * 0.5;
    return Center + FVector2f(static_cast<float>((Screen.X - C.X) / Pixels), static_cast<float>((C.Y - Screen.Y) / Pixels));
}

FVector2D STiPaintCanvas::UVToScreen(const FGeometry& Geometry, const FVector2f& UV) const
{
    const FVector2D Size = Geometry.GetLocalSize();
    if (bNeedsFit) { Pixels = FMath::Max(40.0f, static_cast<float>(FMath::Min(Size.X, Size.Y) * 0.82)); Center = FVector2f(0.5f, 0.5f); bNeedsFit = false; }
    const FVector2D C = Size * 0.5;
    return C + FVector2D((UV.X - Center.X) * Pixels, -(UV.Y - Center.Y) * Pixels);
}

int32 STiPaintCanvas::OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const
{
    static FSlateBrush Background; Background.DrawAs = ESlateBrushDrawType::Box;
    const FVector2D Size = Geometry.GetLocalSize();
    FSlateDrawElement::MakeBox(Out, Layer, Geometry.ToPaintGeometry(Size, FSlateLayoutTransform()), &Background, ESlateDrawEffect::None, FLinearColor(0.025f, 0.03f, 0.04f, 1));
    const float Grid = FMath::Max(12.0f, Pixels / 8.0f);
    for (float X = FMath::Fmod(Size.X * .5f - Center.X * Pixels, Grid); X < Size.X; X += Grid)
        FSlateDrawElement::MakeLines(Out, Layer + 1, Geometry.ToPaintGeometry(), { FVector2D(X, 0), FVector2D(X, Size.Y) }, ESlateDrawEffect::None, FLinearColor(0.11f, 0.14f, 0.18f), false, 1.0f);
    for (float Y = FMath::Fmod(Size.Y * .5f + Center.Y * Pixels, Grid); Y < Size.Y; Y += Grid)
        FSlateDrawElement::MakeLines(Out, Layer + 1, Geometry.ToPaintGeometry(), { FVector2D(0, Y), FVector2D(Size.X, Y) }, ESlateDrawEffect::None, FLinearColor(0.11f, 0.14f, 0.18f), false, 1.0f);

    const TArray<FColor>& Composite = Document->Composite();
    const FVector2D TL = UVToScreen(Geometry, FVector2f(0, 1));
    const FVector2D BR = UVToScreen(Geometry, FVector2f(1, 0));
    const int32 PreviewSize = 64;
    const int32 CellX = FMath::Max(1, Document->Width() / PreviewSize);
    const int32 CellY = FMath::Max(1, Document->Height() / PreviewSize);
    for (int32 GridY = 0; GridY < PreviewSize; ++GridY)
    {
        for (int32 GridX = 0; GridX < PreviewSize; ++GridX)
        {
            FLinearColor Color = FLinearColor::Transparent;
            int32 Samples = 0;
            const int32 MinX = GridX * CellX, MinY = GridY * CellY;
            const int32 MaxX = FMath::Min(Document->Width(), MinX + CellX);
            const int32 MaxY = FMath::Min(Document->Height(), MinY + CellY);
            for (int32 Y = MinY; Y < MaxY; ++Y)
            {
                for (int32 X = MinX; X < MaxX; ++X)
                {
                    Color += FLinearColor(Composite[Y * Document->Width() + X]);
                    ++Samples;
                }
            }
            if (Samples > 0) Color /= static_cast<float>(Samples);
            const float U0 = static_cast<float>(GridX) / PreviewSize;
            const float U1 = static_cast<float>(GridX + 1) / PreviewSize;
            const float V0 = 1.0f - static_cast<float>(GridY + 1) / PreviewSize;
            const float V1 = 1.0f - static_cast<float>(GridY) / PreviewSize;
            const FVector2D A = UVToScreen(Geometry, FVector2f(U0, V1));
            const FVector2D B = UVToScreen(Geometry, FVector2f(U1, V0));
            FSlateDrawElement::MakeBox(Out, Layer + 2, Geometry.ToPaintGeometry(FVector2D(B.X - A.X + 1.0, B.Y - A.Y + 1.0), FSlateLayoutTransform(A)), &Background, ESlateDrawEffect::None, Color);
        }
    }
    FSlateDrawElement::MakeLines(Out, Layer + 3, Geometry.ToPaintGeometry(), { TL, FVector2D(BR.X, TL.Y), BR, FVector2D(TL.X, BR.Y), TL }, ESlateDrawEffect::None, FLinearColor(0.5f, 0.8f, 1.0f), false, 2.0f);
    for (const FTiPaintUVTriangle& Triangle : UVTriangles)
    {
        const FVector2D A = UVToScreen(Geometry, Triangle.UV[0]);
        const FVector2D B = UVToScreen(Geometry, Triangle.UV[1]);
        const FVector2D C = UVToScreen(Geometry, Triangle.UV[2]);
        if ((A.X < -8.0 && B.X < -8.0 && C.X < -8.0) ||
            (A.X > Size.X + 8.0 && B.X > Size.X + 8.0 && C.X > Size.X + 8.0) ||
            (A.Y < -8.0 && B.Y < -8.0 && C.Y < -8.0) ||
            (A.Y > Size.Y + 8.0 && B.Y > Size.Y + 8.0 && C.Y > Size.Y + 8.0))
        {
            continue;
        }
        FSlateDrawElement::MakeLines(Out, Layer + 4, Geometry.ToPaintGeometry(),
            { A, B, C, A }, ESlateDrawEffect::None, FLinearColor(0.22f, 0.72f, 1.0f, 0.70f), false, 1.0f);
    }
    if (bStroke)
    {
        const FVector2D Cursor = UVToScreen(Geometry, LastUV);
        FSlateDrawElement::MakeLines(Out, Layer + 4, Geometry.ToPaintGeometry(), { Cursor - FVector2D(Document->Brush().Radius), Cursor + FVector2D(Document->Brush().Radius, -Document->Brush().Radius), Cursor + FVector2D(Document->Brush().Radius), Cursor + FVector2D(-Document->Brush().Radius, Document->Brush().Radius), Cursor - FVector2D(Document->Brush().Radius) }, ESlateDrawEffect::None, FLinearColor(1, 0.65f, 0.15f), false, 1.5f);
    }
    return Layer + 4;
}

FReply STiPaintCanvas::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
    const FVector2D P = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
    LastMouse = P; LastUV = ScreenToUV(Geometry, P);
    const FKey Button = Event.GetEffectingButton();
    if (Button == EKeys::MiddleMouseButton || (Button == EKeys::LeftMouseButton && Event.IsAltDown())) { bPan = true; return FReply::Handled().CaptureMouse(AsShared()); }
    if (Button == EKeys::LeftMouseButton) { Document->BeginStroke(LastUV.X, LastUV.Y); bStroke = Document->IsStroking(); Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled().CaptureMouse(AsShared()); }
    return FReply::Unhandled();
}

FReply STiPaintCanvas::OnMouseButtonUp(const FGeometry&, const FPointerEvent&)
{
    if (bStroke) Document->EndStroke(); bStroke = false; bPan = false; Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled().ReleaseMouseCapture();
}

FReply STiPaintCanvas::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
    const FVector2D P = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
    if (bPan) { const FVector2D D = P - LastMouse; Center -= FVector2f(static_cast<float>(D.X / Pixels), static_cast<float>(-D.Y / Pixels)); LastMouse = P; }
    else if (bStroke) { LastUV = ScreenToUV(Geometry, P); Document->AddStrokePoint(LastUV.X, LastUV.Y); }
    Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled();
}

FReply STiPaintCanvas::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event)
{
    const FVector2D P = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()); const FVector2f Before = ScreenToUV(Geometry, P); Pixels = FMath::Clamp(Pixels * FMath::Pow(1.15f, Event.GetWheelDelta()), 50.0f, 3000.0f); const FVector2f After = ScreenToUV(Geometry, P); Center += Before - After; Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled();
}

void STiPaintEditor::Construct(const FArguments&)
{
    Document = MakeShared<FTiPaintDocument>();
    Document->SetChanged([this] { Refresh(); });
    Canvas = SNew(STiPaintCanvas, Document);
    Status = LOCTEXT("Ready", "Ready");
    ChildSlot[SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[MakeToolbar()]
        + SVerticalBox::Slot().FillHeight(1.0f)[Canvas.ToSharedRef()]
        + SVerticalBox::Slot().AutoHeight()[MakeLayers()]
        + SVerticalBox::Slot().AutoHeight()[MakeStatus()]];
}

void STiPaintEditor::SetUVTriangles(const TArray<FTiPaintUVTriangle>& InTriangles)
{
    if (Canvas.IsValid())
    {
        Canvas->SetUVTriangles(InTriangles);
    }
}

void STiPaintEditor::SetPaintedCallback(TFunction<void(const TArray<FColor>&, int32, int32)> InCallback)
{
    PaintedCallback = MoveTemp(InCallback);
}

void STiPaintEditor::ResetForMesh(const TArray<FTiPaintUVTriangle>& InTriangles)
{
    Document = MakeShared<FTiPaintDocument>();
    Document->SetChanged([this] { Refresh(); });
    SetUVTriangles(InTriangles);
    if (Canvas.IsValid())
    {
        Canvas->SetDocument(Document);
    }
    Refresh();
}

void STiPaintEditor::Refresh()
{
    if (Canvas.IsValid()) Canvas->Invalidate(EInvalidateWidgetReason::Paint);
    if (PaintedCallback)
    {
        PaintedCallback(Document->Composite(), Document->Width(), Document->Height());
    }
}
void STiPaintEditor::SetMode(ETiPaintMode Mode) { FTiPaintBrush Brush = Document->Brush(); Brush.Mode = Mode; Document->SetBrush(Brush); Refresh(); }
FReply STiPaintEditor::Export()
{
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get(); if (!Desktop) return FReply::Handled();
    TArray<FString> Files; const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (Desktop->SaveFileDialog(Parent, LOCTEXT("ExportTitle", "Export Painted Texture").ToString(), FPaths::ProjectSavedDir(), TEXT("paint.png"), TEXT("PNG (*.png)|*.png"), 0, Files) && !Files.IsEmpty()) { FText Error; Status = Document->ExportPNG(Files[0], Error) ? LOCTEXT("Exported", "PNG exported") : Error; }
    return FReply::Handled();
}

TSharedRef<SWidget> STiPaintEditor::MakeToolbar()
{
    return SNew(SBorder).Padding(6)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4, 2))
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Paint", "Paint")).OnClicked_Lambda([this] { SetMode(ETiPaintMode::Paint); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Erase", "Erase")).OnClicked_Lambda([this] { SetMode(ETiPaintMode::Erase); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(STextBlock).Text(LOCTEXT("Radius", "Radius"))]
        + SWrapBox::Slot()[SNew(SBox).WidthOverride(150)[SNew(SSlider).MinValue(2).MaxValue(200).Value_Lambda([this] { return Document->Brush().Radius; }).OnValueChanged_Lambda([this](float V) { FTiPaintBrush B = Document->Brush(); B.Radius = V; Document->SetBrush(B); })]]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Undo", "Undo")).OnClicked_Lambda([this] { Document->Undo(); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Redo", "Redo")).OnClicked_Lambda([this] { Document->Redo(); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Clear", "Clear")).OnClicked_Lambda([this] { Document->Clear(); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Fit", "Fit")).OnClicked_Lambda([this] { Canvas->Fit(); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Export", "Export PNG")).OnClicked(this, &STiPaintEditor::Export)]];
}

TSharedRef<SWidget> STiPaintEditor::MakeLayers()
{
    return SNew(SBorder).Padding(6)[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth()[SNew(SComboButton).OnGetMenuContent(this, &STiPaintEditor::MakeLayerMenu).ButtonContent()[SNew(STextBlock).Text_Lambda([this] { return FText::Format(LOCTEXT("LayerText", "Layer {0}/{1}"), FText::AsNumber(Document->ActiveLayer() + 1), FText::AsNumber(Document->Layers().Num())); })]]
        + SHorizontalBox::Slot().AutoWidth().Padding(12, 0)[SNew(SButton).Text(LOCTEXT("AddLayer", "Add Layer")).OnClicked_Lambda([this] { Document->AddLayer(); return FReply::Handled(); })]
        + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("RemoveLayer", "Remove Layer")).OnClicked_Lambda([this] { Document->RemoveActiveLayer(); return FReply::Handled(); })]
        + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("Visibility", "Visibility")).OnClicked_Lambda([this] { Document->ToggleActiveVisibility(); return FReply::Handled(); })]
        + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("Lock", "Lock")).OnClicked_Lambda([this] { Document->ToggleActiveLock(); return FReply::Handled(); })]];
}

TSharedRef<SWidget> STiPaintEditor::MakeLayerMenu()
{
    FMenuBuilder Menu(true, nullptr);
    for (int32 Index = 0; Index < Document->Layers().Num(); ++Index)
    {
        const FString Label = FString::Printf(TEXT("%d: %s"), Index + 1, *Document->Layers()[Index].Name);
        Menu.AddMenuEntry(FText::FromString(Label), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([this, Index] { Document->SetActiveLayer(Index); })));
    }
    return Menu.MakeWidget();
}

TSharedRef<SWidget> STiPaintEditor::MakeStatus() { return SNew(SBorder).Padding(6)[SNew(STextBlock).Text_Lambda([this] { return FText::Format(LOCTEXT("StatusText", "{0}  |  {1} undo / {2} redo"), Status, FText::AsNumber(Document->UndoCount()), FText::AsNumber(Document->RedoCount())); })]; }

#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2

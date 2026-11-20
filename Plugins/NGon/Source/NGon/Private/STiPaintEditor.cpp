#include "STiPaintEditor.h"
#include "TiPaintAPI.h"

#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformFileManager.h"
#include "IDesktopPlatform.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "TiPaintEditor"

namespace
{
UTexture2D* CreateCanvasTransientTexture(int32 Width, int32 Height)
{
    UTexture2D* Tex = UTexture2D::CreateTransient(FMath::Max(1, Width), FMath::Max(1, Height), PF_B8G8R8A8);
    if (Tex)
    {
        Tex->SRGB = true;
        Tex->Filter = TF_Bilinear;
        Tex->AddressX = TA_Clamp;
        Tex->AddressY = TA_Clamp;
        Tex->NeverStream = true;
    }
    return Tex;
}

bool UploadCanvasTexture(UTexture2D* Texture, const TArray<FColor>& Pixels)
{
    if (!Texture || Pixels.IsEmpty()) return false;
    FTexturePlatformData* PlatformData = Texture->GetPlatformData();
    if (!PlatformData || PlatformData->Mips.Num() == 0) return false;
    FTexture2DMipMap& Mip = PlatformData->Mips[0];
    const int64 Bytes = static_cast<int64>(Pixels.Num()) * static_cast<int64>(sizeof(FColor));
    if (Mip.BulkData.GetBulkDataSize() < Bytes) return false;
    void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
    if (Dest)
    {
        FMemory::Memcpy(Dest, Pixels.GetData(), Bytes);
    }
    Mip.BulkData.Unlock();
    if (!Dest) return false;
    Texture->UpdateResource();
    return true;
}

TSharedRef<SWidget> MakeSliderRow(
    const FText& Label,
    float MinVal,
    float MaxVal,
    TAttribute<float> ValueAttr,
    FOnFloatValueChanged OnChanged)
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
        [
            SNew(SBox).WidthOverride(64.0f)
            [
                SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
            ]
        ]
        + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
        [
            SNew(SSlider).MinValue(MinVal).MaxValue(MaxVal).Value(ValueAttr).OnValueChanged(OnChanged)
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
        [
            SNew(SBox).WidthOverride(36.0f)
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                .Text_Lambda([ValueAttr]() { return FText::FromString(FString::Printf(TEXT("%.2f"), ValueAttr.Get(0.0f))); })
            ]
        ];
}
} // namespace

// ============================================================================
// STiPaintCanvas — Full-Resolution GPU-Textured Paint Canvas
// ============================================================================

void STiPaintCanvas::Construct(const FArguments&, const TSharedRef<FTiPaintDocument>& InDocument)
{
    Document = InDocument;
    CanvasBrush.DrawAs = ESlateBrushDrawType::Image;
    CanvasBrush.Tiling = ESlateBrushTileType::NoTile;
}

void STiPaintCanvas::SetDocument(const TSharedRef<FTiPaintDocument>& InDocument)
{
    Document = InDocument;
    CachedRevision = 0;
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
    if (bNeedsFit && Size.X > 4.0 && Size.Y > 4.0)
    {
        Pixels = FMath::Max(40.0f, static_cast<float>(FMath::Min(Size.X, Size.Y) * 0.86));
        Center = FVector2f(0.5f, 0.5f);
        bNeedsFit = false;
    }
    const FVector2D C = Size * 0.5;
    return Center + FVector2f(static_cast<float>((Screen.X - C.X) / Pixels), static_cast<float>((C.Y - Screen.Y) / Pixels));
}

FVector2D STiPaintCanvas::UVToScreen(const FGeometry& Geometry, const FVector2f& UV) const
{
    const FVector2D Size = Geometry.GetLocalSize();
    if (bNeedsFit && Size.X > 4.0 && Size.Y > 4.0)
    {
        Pixels = FMath::Max(40.0f, static_cast<float>(FMath::Min(Size.X, Size.Y) * 0.86));
        Center = FVector2f(0.5f, 0.5f);
        bNeedsFit = false;
    }
    const FVector2D C = Size * 0.5;
    return C + FVector2D((UV.X - Center.X) * Pixels, -(UV.Y - Center.Y) * Pixels);
}

void STiPaintCanvas::SyncGpuTextureIfNeeded() const
{
    const uint64 Rev = Document->Revision();
    const int32 W = Document->Width();
    const int32 H = Document->Height();
    if (CanvasTexture.IsValid() && CachedRevision == Rev && CachedTexW == W && CachedTexH == H)
    {
        return;
    }
    if (!CanvasTexture.IsValid() || CachedTexW != W || CachedTexH != H)
    {
        CanvasTexture.Reset(CreateCanvasTransientTexture(W, H));
        CachedTexW = W;
        CachedTexH = H;
    }
    if (CanvasTexture.IsValid())
    {
        const TArray<FColor>& Comp = Document->Composite();
        if (UploadCanvasTexture(CanvasTexture.Get(), Comp))
        {
            CanvasBrush.SetResourceObject(CanvasTexture.Get());
            CanvasBrush.ImageSize = FVector2D(W, H);
            CanvasBrush.DrawAs = ESlateBrushDrawType::Image;
            CachedRevision = Rev;
        }
    }
}

void STiPaintCanvas::DrawBrushCircle(
    FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
    const FVector2f& UV, const FLinearColor& Color) const
{
    const FVector2D CenterScr = UVToScreen(Geometry, UV);
    const float ScreenRadius = FMath::Max(3.0f, Document->Brush().Radius * (Pixels / static_cast<float>(FMath::Max(1, Document->Width()))));
    constexpr int32 Segs = 28;
    TArray<FVector2D> Ring;
    Ring.Reserve(Segs + 1);
    for (int32 i = 0; i <= Segs; ++i)
    {
        const float A = (2.0f * PI * i) / static_cast<float>(Segs);
        Ring.Add(CenterScr + FVector2D(FMath::Cos(A) * ScreenRadius, FMath::Sin(A) * ScreenRadius));
    }
    FSlateDrawElement::MakeLines(Out, Layer, Geometry.ToPaintGeometry(), Ring, ESlateDrawEffect::None, Color, true, 1.5f);
}

int32 STiPaintCanvas::OnPaint(
    const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
    FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const
{
    const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");
    const FVector2D Size = Geometry.GetLocalSize();

    // 1. Viewport backdrop + subtle grid
    FSlateDrawElement::MakeBox(Out, Layer, Geometry.ToPaintGeometry(Size, FSlateLayoutTransform()), WhiteBrush, ESlateDrawEffect::None, FLinearColor(0.024f, 0.028f, 0.036f, 1.0f));
    const float Grid = FMath::Max(16.0f, Pixels / 8.0f);
    for (float X = FMath::Fmod(Size.X * 0.5f - Center.X * Pixels, Grid); X < Size.X; X += Grid)
    {
        FSlateDrawElement::MakeLines(Out, Layer + 1, Geometry.ToPaintGeometry(), { FVector2D(X, 0), FVector2D(X, Size.Y) }, ESlateDrawEffect::None, FLinearColor(0.09f, 0.11f, 0.14f), false, 1.0f);
    }
    for (float Y = FMath::Fmod(Size.Y * 0.5f + Center.Y * Pixels, Grid); Y < Size.Y; Y += Grid)
    {
        FSlateDrawElement::MakeLines(Out, Layer + 1, Geometry.ToPaintGeometry(), { FVector2D(0, Y), FVector2D(Size.X, Y) }, ESlateDrawEffect::None, FLinearColor(0.09f, 0.11f, 0.14f), false, 1.0f);
    }

    // 2. Canvas UV [0..1] transparency checkerboard backdrop (16x16 tiles = 256 light boxes max, or dark base)
    const FVector2D TL = UVToScreen(Geometry, FVector2f(0.0f, 1.0f));
    const FVector2D BR = UVToScreen(Geometry, FVector2f(1.0f, 0.0f));
    const FVector2D CanvasSize = BR - TL;
    if (CanvasSize.X > 1.0 && CanvasSize.Y > 1.0)
    {
        FSlateDrawElement::MakeBox(Out, Layer + 2, Geometry.ToPaintGeometry(CanvasSize, FSlateLayoutTransform(TL)), WhiteBrush, ESlateDrawEffect::None, FLinearColor(0.12f, 0.13f, 0.15f, 1.0f));
        constexpr int32 Checks = 16;
        const FVector2D Cell = CanvasSize / static_cast<double>(Checks);
        for (int32 cy = 0; cy < Checks; ++cy)
        {
            for (int32 cx = (cy & 1); cx < Checks; cx += 2)
            {
                FSlateDrawElement::MakeBox(
                    Out, Layer + 2,
                    Geometry.ToPaintGeometry(Cell, FSlateLayoutTransform(TL + FVector2D(cx * Cell.X, cy * Cell.Y))),
                    WhiteBrush, ESlateDrawEffect::None, FLinearColor(0.18f, 0.19f, 0.22f, 1.0f));
            }
        }

        // 3. Single full-resolution GPU textured quad for the painted document!
        SyncGpuTextureIfNeeded();
        if (CanvasTexture.IsValid())
        {
            FSlateDrawElement::MakeBox(
                Out, Layer + 3,
                Geometry.ToPaintGeometry(CanvasSize, FSlateLayoutTransform(TL)),
                &CanvasBrush, ESlateDrawEffect::None, FLinearColor::White);
        }
    }

    // 4. Canvas border
    FSlateDrawElement::MakeLines(Out, Layer + 4, Geometry.ToPaintGeometry(),
        { TL, FVector2D(BR.X, TL.Y), BR, FVector2D(TL.X, BR.Y), TL },
        ESlateDrawEffect::None, FLinearColor(0.45f, 0.78f, 1.0f, 0.9f), false, 1.5f);

    // 5. Optional UV wireframe overlay
    if (bShowUVs && !UVTriangles.IsEmpty())
    {
        for (const FTiPaintUVTriangle& Triangle : UVTriangles)
        {
            if (!Document->IsSubMeshVisible(Triangle.SubMeshIndex)) continue;
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
            FSlateDrawElement::MakeLines(Out, Layer + 5, Geometry.ToPaintGeometry(),
                { A, B, C, A }, ESlateDrawEffect::None, FLinearColor(0.22f, 0.76f, 1.0f, 0.45f), false, 1.0f);
        }
    }

    // 6. Symmetry axis guide
    const ETiPaintSymmetryMode Sym = Document->Brush().Symmetry;
    if (Sym == ETiPaintSymmetryMode::MirrorU || Sym == ETiPaintSymmetryMode::Radial4)
    {
        const FVector2D Top = UVToScreen(Geometry, FVector2f(0.5f, 1.0f));
        const FVector2D Bot = UVToScreen(Geometry, FVector2f(0.5f, 0.0f));
        FSlateDrawElement::MakeLines(Out, Layer + 6, Geometry.ToPaintGeometry(), { Top, Bot }, ESlateDrawEffect::None, FLinearColor(1.0f, 0.55f, 0.15f, 0.65f), false, 1.0f);
    }
    if (Sym == ETiPaintSymmetryMode::MirrorV || Sym == ETiPaintSymmetryMode::Radial4)
    {
        const FVector2D L = UVToScreen(Geometry, FVector2f(0.0f, 0.5f));
        const FVector2D R = UVToScreen(Geometry, FVector2f(1.0f, 0.5f));
        FSlateDrawElement::MakeLines(Out, Layer + 6, Geometry.ToPaintGeometry(), { L, R }, ESlateDrawEffect::None, FLinearColor(1.0f, 0.55f, 0.15f, 0.65f), false, 1.0f);
    }

    // 7. Live circular brush cursor (plus mirrored cursors when symmetry is active)
    if (bHovering || bStroke)
    {
        const FLinearColor CursorCol = (Document->Brush().Mode == ETiPaintMode::Erase)
            ? FLinearColor(1.0f, 0.35f, 0.35f, 0.9f)
            : FLinearColor(1.0f, 0.72f, 0.22f, 0.9f);
        DrawBrushCircle(Out, Layer + 7, Geometry, LastUV, CursorCol);
        const FLinearColor SymCol(0.4f, 0.85f, 1.0f, 0.6f);
        if (Sym == ETiPaintSymmetryMode::MirrorU || Sym == ETiPaintSymmetryMode::Radial4)
        {
            DrawBrushCircle(Out, Layer + 7, Geometry, FVector2f(1.0f - LastUV.X, LastUV.Y), SymCol);
        }
        if (Sym == ETiPaintSymmetryMode::MirrorV || Sym == ETiPaintSymmetryMode::Radial4)
        {
            DrawBrushCircle(Out, Layer + 7, Geometry, FVector2f(LastUV.X, 1.0f - LastUV.Y), SymCol);
        }
        if (Sym == ETiPaintSymmetryMode::Radial4)
        {
            DrawBrushCircle(Out, Layer + 7, Geometry, FVector2f(1.0f - LastUV.X, 1.0f - LastUV.Y), SymCol);
        }
    }

    return Layer + 8;
}

FReply STiPaintCanvas::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
    const FVector2D P = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
    LastMouse = P;
    LastUV = ScreenToUV(Geometry, P);
    bHovering = true;
    const FKey Button = Event.GetEffectingButton();
    if (Button == EKeys::MiddleMouseButton || (Button == EKeys::LeftMouseButton && Event.IsAltDown()))
    {
        bPan = true;
        return FReply::Handled().CaptureMouse(AsShared());
    }
    if (Button == EKeys::LeftMouseButton)
    {
        Document->BeginStroke(LastUV.X, LastUV.Y);
        bStroke = Document->IsStroking();
        Invalidate(EInvalidateWidgetReason::Paint);
        return FReply::Handled().CaptureMouse(AsShared());
    }
    return FReply::Unhandled();
}

FReply STiPaintCanvas::OnMouseButtonUp(const FGeometry&, const FPointerEvent&)
{
    if (bStroke) Document->EndStroke();
    bStroke = false;
    bPan = false;
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled().ReleaseMouseCapture();
}

FReply STiPaintCanvas::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
    const FVector2D P = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
    bHovering = true;
    if (bPan)
    {
        const FVector2D D = P - LastMouse;
        Center -= FVector2f(static_cast<float>(D.X / Pixels), static_cast<float>(-D.Y / Pixels));
        LastMouse = P;
    }
    else
    {
        LastUV = ScreenToUV(Geometry, P);
        if (bStroke)
        {
            Document->AddStrokePoint(LastUV.X, LastUV.Y);
        }
    }
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled();
}

void STiPaintCanvas::OnMouseLeave(const FPointerEvent&)
{
    bHovering = false;
    Invalidate(EInvalidateWidgetReason::Paint);
}

FReply STiPaintCanvas::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (Event.IsControlDown())
    {
        FTiPaintBrush B = Document->Brush();
        B.Radius = FMath::Clamp(B.Radius * (Event.GetWheelDelta() > 0 ? 1.15f : 0.87f), 1.0f, 256.0f);
        Document->SetBrush(B);
        Invalidate(EInvalidateWidgetReason::Paint);
        return FReply::Handled();
    }
    const FVector2D P = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
    const FVector2f Before = ScreenToUV(Geometry, P);
    Pixels = FMath::Clamp(Pixels * FMath::Pow(1.15f, Event.GetWheelDelta()), 40.0f, 4096.0f);
    const FVector2f After = ScreenToUV(Geometry, P);
    Center += Before - After;
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled();
}

// ============================================================================
// STiPaintEditor — Modular Toolbar + GPU Canvas + Tabbed Inspector
// ============================================================================

void STiPaintEditor::Construct(const FArguments&)
{
    Document = MakeShared<FTiPaintDocument>(512, 512);
    Document->SetChanged([this] { Refresh(); });
    Canvas = SNew(STiPaintCanvas, Document);
    Status = LOCTEXT("Ready", "Ready (GPU Canvas + Dirty-Rect Compositor)");
    AiJsonInput = TEXT(
        "[\n"
        "  {\"op\": \"add_layer\", \"name\": \"AI Accent\"},\n"
        "  {\"op\": \"set_brush\", \"mode\": \"paint\", \"color\": [255, 140, 40, 255], \"radius\": 18, \"tip\": \"chalk\", \"symmetry\": \"mirror_u\"},\n"
        "  {\"op\": \"stroke\", \"points\": [[0.2, 0.2], [0.4, 0.5], [0.25, 0.8]]},\n"
        "  {\"op\": \"set_layer_blend_mode\", \"blend_mode\": \"screen\"},\n"
        "  {\"op\": \"set_layer_opacity\", \"opacity\": 0.85}\n"
        "]");
    AiLastOutput = TEXT("Ready. Run a JSON command or batch to control TiPaint.");

    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[MakeToolbar()]
        + SVerticalBox::Slot().FillHeight(1.0f)
        [
            SNew(SSplitter)
            .Orientation(Orient_Horizontal)
            + SSplitter::Slot().Value(0.70f)
            [
                Canvas.ToSharedRef()
            ]
            + SSplitter::Slot().Value(0.30f)
            [
                MakeInspector()
            ]
        ]
        + SVerticalBox::Slot().AutoHeight()[MakeStatus()]
    ];
}

void STiPaintEditor::SetUVTriangles(const TArray<FTiPaintUVTriangle>& InTriangles)
{
    Document->SetMeshTriangles(InTriangles);
    if (Canvas.IsValid())
    {
        Canvas->SetUVTriangles(InTriangles);
    }
}

void STiPaintEditor::SetPaintedCallback(TFunction<void(const TArray<FColor>&, int32, int32)> InCallback)
{
    PaintedCallback = MoveTemp(InCallback);
}

void STiPaintEditor::SetPaint3DModeCallback(TFunction<void(bool)> InCallback)
{
    Paint3DModeCallback = MoveTemp(InCallback);
}

void STiPaintEditor::SetPaint3DMode(bool bEnabled)
{
    if (bPaint3DMode == bEnabled) return;
    bPaint3DMode = bEnabled;
    if (Paint3DModeCallback)
    {
        Paint3DModeCallback(bPaint3DMode);
    }
    Refresh();
}

void STiPaintEditor::ResetForMesh(const TArray<FTiPaintUVTriangle>& InTriangles, const TArray<FTiPaintSubMeshInfo>& InSubMeshes)
{
    Document = MakeShared<FTiPaintDocument>(512, 512);
    Document->SetChanged([this] { Refresh(); });
    SetUVTriangles(InTriangles);
    Document->SetSubMeshes(InSubMeshes);
    if (Canvas.IsValid())
    {
        Canvas->SetDocument(Document);
    }
    Refresh();
}

void STiPaintEditor::Refresh()
{
    if (Canvas.IsValid())
    {
        Canvas->Invalidate(EInvalidateWidgetReason::Paint);
    }
    if (PaintedCallback)
    {
        PaintedCallback(Document->Composite(), Document->Width(), Document->Height());
    }
}

void STiPaintEditor::SetMode(ETiPaintMode Mode)
{
    FTiPaintBrush B = Document->Brush();
    B.Mode = Mode;
    Document->SetBrush(B);
    Refresh();
}

FReply STiPaintEditor::Export()
{
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    if (!Desktop) return FReply::Handled();
    TArray<FString> Files;
    const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (Desktop->SaveFileDialog(Parent, LOCTEXT("ExportTitle", "Export Painted Texture").ToString(), FPaths::ProjectSavedDir(), TEXT("paint.png"), TEXT("PNG (*.png)|*.png"), 0, Files) && !Files.IsEmpty())
    {
        FText Error;
        Status = Document->ExportPNG(Files[0], Error) ? LOCTEXT("Exported", "PNG exported") : Error;
    }
    return FReply::Handled();
}

FReply STiPaintEditor::ImportLayerImage()
{
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    if (!Desktop) return FReply::Handled();
    TArray<FString> Files;
    const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (Desktop->OpenFileDialog(Parent, LOCTEXT("ImportTitle", "Import Image as Layer").ToString(), FPaths::ProjectDir(), TEXT(""), TEXT("Images (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp"), 0, Files) && !Files.IsEmpty())
    {
        FText Error;
        Status = Document->ImportImageToNewLayer(Files[0], Error) ? LOCTEXT("Imported", "Imported image layer") : Error;
    }
    return FReply::Handled();
}

FReply STiPaintEditor::LoadStencilImage()
{
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    if (!Desktop) return FReply::Handled();
    TArray<FString> Files;
    const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (Desktop->OpenFileDialog(Parent, LOCTEXT("StencilImgTitle", "Load Stencil Image").ToString(), FPaths::ProjectDir(), TEXT(""), TEXT("Images (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp"), 0, Files) && !Files.IsEmpty())
    {
        FText Error;
        Status = Document->LoadStencilImage(Files[0], Error) ? LOCTEXT("StencilLoaded", "Stencil image loaded") : Error;
    }
    return FReply::Handled();
}

FReply STiPaintEditor::RunProjection()
{
    FString Msg;
    FTiPaintProjectionSettings P = Document->Projection();
    P.TintColor = Document->Brush().Color;
    const bool bOk = Document->ProjectOntoMesh(P, Msg);
    Status = FText::FromString(Msg);
    return bOk ? FReply::Handled() : FReply::Handled();
}

FReply STiPaintEditor::RunAiJson()
{
    const FTiPaintOpResult Res = FTiPaintAPI::ExecuteJsonCommand(*Document, AiJsonInput);
    AiLastOutput = Res.bOk ? (TEXT("OK: ") + Res.Message) : (TEXT("ERR: ") + Res.Message);
    if (!Res.PayloadJson.IsEmpty())
    {
        AiLastOutput += TEXT("\n") + Res.PayloadJson;
    }
    Status = FText::FromString(Res.Message);
    return FReply::Handled();
}

TSharedRef<SWidget> STiPaintEditor::MakeToolbar()
{
    return SNew(SBorder).Padding(4.0f)
    [
        SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 2.0f))
        + SWrapBox::Slot()
        [
            SNew(SButton)
            .ToolTipText(LOCTEXT("Paint3DTip", "Toggle direct 3D viewport painting (left-drag on 3D mesh to paint)"))
            .ButtonColorAndOpacity_Lambda([this] { return bPaint3DMode ? FLinearColor(0.16f, 0.72f, 0.56f, 1.0f) : FLinearColor(0.24f, 0.27f, 0.33f, 1.0f); })
            .OnClicked_Lambda([this] { SetPaint3DMode(!bPaint3DMode); return FReply::Handled(); })
            [
                SNew(STextBlock)
                .Text_Lambda([this] { return bPaint3DMode ? LOCTEXT("Paint3DOn", "3D Paint: ON") : LOCTEXT("Paint3DOff", "3D Paint"); })
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
            ]
        ]
        + SWrapBox::Slot()
        [
            SNew(SComboButton)
            .ToolTipText(LOCTEXT("PaintSubMeshTip", "Filter 2D/3D paint to a specific SubMesh to avoid overlapping UV islands"))
            .OnGetMenuContent(this, &STiPaintEditor::MakeSubMeshMenu)
            .ButtonContent()
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                .Text_Lambda([this]()
                {
                    const int32 Active = Document->GetActiveSubMeshFilter();
                    const TArray<FTiPaintSubMeshInfo>& Subs = Document->GetSubMeshes();
                    if (Subs.IsValidIndex(Active))
                    {
                        const FString Name = Subs[Active].Name.IsEmpty() ? FString::Printf(TEXT("Part %d"), Active) : Subs[Active].Name;
                        return FText::FromString(FString::Printf(TEXT("SubMesh: %s"), *Name));
                    }
                    return FText::FromString(Subs.Num() > 1 ? FString::Printf(TEXT("SubMesh: All (%d)"), Subs.Num()) : TEXT("SubMesh: All"));
                })
            ]
        ]
        + SWrapBox::Slot()
        [
            SNew(SButton)
            .ToolTipText(LOCTEXT("UVMaskTip", "Mask 2D strokes to the active SubMesh UV islands so overlapping UVs never bleed"))
            .ButtonColorAndOpacity_Lambda([this] { return Document->IsSubMeshMaskEnabled() ? FLinearColor(0.22f, 0.52f, 0.78f, 1.0f) : FLinearColor(0.24f, 0.27f, 0.33f, 1.0f); })
            .OnClicked_Lambda([this] { Document->SetSubMeshMaskEnabled(!Document->IsSubMeshMaskEnabled()); return FReply::Handled(); })
            [
                SNew(STextBlock)
                .Text_Lambda([this] { return Document->IsSubMeshMaskEnabled() ? LOCTEXT("UVMaskOn", "UV Mask: ON") : LOCTEXT("UVMaskOff", "UV Mask: OFF"); })
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
            ]
        ]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Paint", "Paint")).OnClicked_Lambda([this] { SetMode(ETiPaintMode::Paint); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Erase", "Erase")).OnClicked_Lambda([this] { SetMode(ETiPaintMode::Erase); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Smudge", "Smudge")).OnClicked_Lambda([this] { SetMode(ETiPaintMode::Smudge); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Blur", "Blur")).OnClicked_Lambda([this] { SetMode(ETiPaintMode::Blur); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Fill", "Fill")).OnClicked_Lambda([this] { SetMode(ETiPaintMode::Fill); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Pick", "Pick")).OnClicked_Lambda([this] { SetMode(ETiPaintMode::Eyedropper); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Undo", "Undo")).OnClicked_Lambda([this] { Document->Undo(); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Redo", "Redo")).OnClicked_Lambda([this] { Document->Redo(); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Clear", "Clear")).OnClicked_Lambda([this] { Document->Clear(); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Fit", "Fit")).OnClicked_Lambda([this] { if (Canvas.IsValid()) Canvas->Fit(); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("UVs", "UVs")).OnClicked_Lambda([this] { if (Canvas.IsValid()) Canvas->SetShowUVs(!Canvas->IsShowingUVs()); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Res512", "512")).OnClicked_Lambda([this] { Document->Resize(512, 512); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Res1024", "1024")).OnClicked_Lambda([this] { Document->Resize(1024, 1024); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Import", "Import")).OnClicked(this, &STiPaintEditor::ImportLayerImage)]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Export", "Export PNG")).OnClicked(this, &STiPaintEditor::Export)]
    ];
}

TSharedRef<SWidget> STiPaintEditor::MakeInspector()
{
    return SNew(SBorder).Padding(6.0f)
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(3.0f, 2.0f))
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("TabBrush", "Brush")).OnClicked_Lambda([this] { ActiveInspectorTab = 0; return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("TabLayers", "Layers")).OnClicked_Lambda([this] { ActiveInspectorTab = 1; return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("TabStencil", "Stencil / 3D")).OnClicked_Lambda([this] { ActiveInspectorTab = 2; return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("TabAI", "AI JSON")).OnClicked_Lambda([this] { ActiveInspectorTab = 3; return FReply::Handled(); })]
        ]
        + SVerticalBox::Slot().FillHeight(1.0f)
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            [
                SNew(SWidgetSwitcher)
                .WidgetIndex_Lambda([this] { return ActiveInspectorTab; })
                + SWidgetSwitcher::Slot()[MakeBrushPanel()]
                + SWidgetSwitcher::Slot()[MakeLayersPanel()]
                + SWidgetSwitcher::Slot()[MakeStencilProjectionPanel()]
                + SWidgetSwitcher::Slot()[MakeAiJsonPanel()]
            ]
        ]
    ];
}

TSharedRef<SWidget> STiPaintEditor::MakeBrushPanel()
{
    auto MakeSwatch = [this](FColor Col) -> TSharedRef<SWidget>
    {
        return SNew(SBox).WidthOverride(22.0f).HeightOverride(20.0f)
        [
            SNew(SButton)
            .ButtonColorAndOpacity(FLinearColor(Col))
            .OnClicked_Lambda([this, Col]()
            {
                FTiPaintBrush B = Document->Brush();
                B.Color = Col;
                Document->SetBrush(B);
                Refresh();
                return FReply::Handled();
            })
        ];
    };

    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(3.0f, 3.0f))
            + SWrapBox::Slot()[MakeSwatch(FColor(235, 64, 52, 255))]
            + SWrapBox::Slot()[MakeSwatch(FColor(255, 152, 40, 255))]
            + SWrapBox::Slot()[MakeSwatch(FColor(245, 220, 65, 255))]
            + SWrapBox::Slot()[MakeSwatch(FColor(72, 200, 110, 255))]
            + SWrapBox::Slot()[MakeSwatch(FColor(77, 128, 255, 255))]
            + SWrapBox::Slot()[MakeSwatch(FColor(175, 95, 240, 255))]
            + SWrapBox::Slot()[MakeSwatch(FColor(245, 245, 245, 255))]
            + SWrapBox::Slot()[MakeSwatch(FColor(28, 28, 32, 255))]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
        [
            MakeSliderRow(LOCTEXT("RadiusLbl", "Radius"), 1.0f, 200.0f,
                TAttribute<float>::CreateLambda([this] { return Document->Brush().Radius; }),
                FOnFloatValueChanged::CreateLambda([this](float V) { FTiPaintBrush B = Document->Brush(); B.Radius = V; Document->SetBrush(B); }))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            MakeSliderRow(LOCTEXT("OpacityLbl", "Opacity"), 0.01f, 1.0f,
                TAttribute<float>::CreateLambda([this] { return Document->Brush().Opacity; }),
                FOnFloatValueChanged::CreateLambda([this](float V) { FTiPaintBrush B = Document->Brush(); B.Opacity = V; Document->SetBrush(B); }))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            MakeSliderRow(LOCTEXT("FlowLbl", "Flow"), 0.05f, 1.0f,
                TAttribute<float>::CreateLambda([this] { return Document->Brush().Flow; }),
                FOnFloatValueChanged::CreateLambda([this](float V) { FTiPaintBrush B = Document->Brush(); B.Flow = V; Document->SetBrush(B); }))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            MakeSliderRow(LOCTEXT("HardLbl", "Hardness"), 0.0f, 1.0f,
                TAttribute<float>::CreateLambda([this] { return Document->Brush().Hardness; }),
                FOnFloatValueChanged::CreateLambda([this](float V) { FTiPaintBrush B = Document->Brush(); B.Hardness = V; Document->SetBrush(B); }))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            MakeSliderRow(LOCTEXT("SpaceLbl", "Spacing"), 0.05f, 1.5f,
                TAttribute<float>::CreateLambda([this] { return Document->Brush().Spacing; }),
                FOnFloatValueChanged::CreateLambda([this](float V) { FTiPaintBrush B = Document->Brush(); B.Spacing = V; Document->SetBrush(B); }))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            MakeSliderRow(LOCTEXT("ScatLbl", "Scatter"), 0.0f, 2.0f,
                TAttribute<float>::CreateLambda([this] { return Document->Brush().Scatter; }),
                FOnFloatValueChanged::CreateLambda([this](float V) { FTiPaintBrush B = Document->Brush(); B.Scatter = V; Document->SetBrush(B); }))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(0.5f).Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SNew(SComboButton).OnGetMenuContent(this, &STiPaintEditor::MakeTipMenu)
                .ButtonContent()[SNew(STextBlock).Text(LOCTEXT("TipBtn", "Brush Tip"))]
            ]
            + SHorizontalBox::Slot().FillWidth(0.5f)
            [
                SNew(SComboButton).OnGetMenuContent(this, &STiPaintEditor::MakeSymmetryMenu)
                .ButtonContent()[SNew(STextBlock).Text(LOCTEXT("SymBtn", "Symmetry"))]
            ]
        ];
}

TSharedRef<SWidget> STiPaintEditor::MakeLayersPanel()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            SNew(SComboButton).OnGetMenuContent(this, &STiPaintEditor::MakeLayerMenu)
            .ButtonContent()
            [
                SNew(STextBlock).Text_Lambda([this]
                {
                    const int32 Idx = Document->ActiveLayer();
                    const FString Name = Document->Layers().IsValidIndex(Idx) ? Document->Layers()[Idx].Name : TEXT("None");
                    return FText::FromString(FString::Printf(TEXT("Layer %d/%d: %s"), Idx + 1, Document->Layers().Num(), *Name));
                })
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
        [
            SNew(SComboButton).OnGetMenuContent(this, &STiPaintEditor::MakeBlendModeMenu)
            .ButtonContent()
            [
                SNew(STextBlock).Text_Lambda([this]
                {
                    const int32 Idx = Document->ActiveLayer();
                    const FString M = Document->Layers().IsValidIndex(Idx)
                        ? FTiPaintDocument::BlendModeToString(Document->Layers()[Idx].BlendMode)
                        : TEXT("normal");
                    return FText::FromString(FString::Printf(TEXT("Blend: %s"), *M));
                })
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
        [
            MakeSliderRow(LOCTEXT("LayerOpLbl", "Opacity"), 0.0f, 1.0f,
                TAttribute<float>::CreateLambda([this]
                {
                    const int32 Idx = Document->ActiveLayer();
                    return Document->Layers().IsValidIndex(Idx) ? Document->Layers()[Idx].Opacity : 1.0f;
                }),
                FOnFloatValueChanged::CreateLambda([this](float V) { Document->SetActiveLayerOpacity(V); }))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
        [
            SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f))
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("AddLyr", "+ Layer")).OnClicked_Lambda([this] { Document->AddLayer(); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("DupLyr", "Dup")).OnClicked_Lambda([this] { Document->DuplicateActiveLayer(); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("DelLyr", "Delete")).OnClicked_Lambda([this] { Document->RemoveActiveLayer(); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("UpLyr", "Up")).OnClicked_Lambda([this] { Document->MoveActiveLayer(1); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("DnLyr", "Down")).OnClicked_Lambda([this] { Document->MoveActiveLayer(-1); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("MrgLyr", "Merge Down")).OnClicked_Lambda([this] { Document->MergeActiveLayerDown(); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("FltLyr", "Flatten")).OnClicked_Lambda([this] { Document->FlattenVisibleLayers(); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("VisLyr", "Visible")).OnClicked_Lambda([this] { Document->ToggleActiveVisibility(); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("LckLyr", "Lock")).OnClicked_Lambda([this] { Document->ToggleActiveLock(); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("ALckLyr", "Alpha Lock")).OnClicked_Lambda([this] { Document->ToggleActiveAlphaLock(); return FReply::Handled(); })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("FillLyr", "Fill Color")).OnClicked_Lambda([this] { Document->FillActiveLayer(Document->Brush().Color); return FReply::Handled(); })]
        ];
}

TSharedRef<SWidget> STiPaintEditor::MakeStencilProjectionPanel()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            SNew(STextBlock).Text(LOCTEXT("StencilHdr", "STENCIL")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f))
            + SWrapBox::Slot()
            [
                SNew(SButton)
                .Text_Lambda([this] { return Document->Stencil().bEnabled ? LOCTEXT("StOn", "Stencil: ON") : LOCTEXT("StOff", "Stencil: OFF"); })
                .OnClicked_Lambda([this] { FTiPaintStencil S = Document->Stencil(); S.bEnabled = !S.bEnabled; Document->SetStencil(S); Refresh(); return FReply::Handled(); })
            ]
            + SWrapBox::Slot()
            [
                SNew(SComboButton).OnGetMenuContent(this, &STiPaintEditor::MakeStencilTypeMenu)
                .ButtonContent()[SNew(STextBlock).Text(LOCTEXT("StType", "Pattern"))]
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("StImg", "Load Image")).OnClicked(this, &STiPaintEditor::LoadStencilImage)
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("StInv", "Invert"))
                .OnClicked_Lambda([this] { FTiPaintStencil S = Document->Stencil(); S.bInvert = !S.bInvert; Document->SetStencil(S); Refresh(); return FReply::Handled(); })
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("StCol", "Color Stamp"))
                .OnClicked_Lambda([this] { FTiPaintStencil S = Document->Stencil(); S.bUseAsColorStamp = !S.bUseAsColorStamp; Document->SetStencil(S); Refresh(); return FReply::Handled(); })
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            MakeSliderRow(LOCTEXT("StScale", "Scale"), 0.25f, 16.0f,
                TAttribute<float>::CreateLambda([this] { return Document->Stencil().Scale; }),
                FOnFloatValueChanged::CreateLambda([this](float V) { FTiPaintStencil S = Document->Stencil(); S.Scale = V; Document->SetStencil(S); }))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            MakeSliderRow(LOCTEXT("StRot", "Rotate"), 0.0f, 360.0f,
                TAttribute<float>::CreateLambda([this] { return Document->Stencil().RotationDeg; }),
                FOnFloatValueChanged::CreateLambda([this](float V) { FTiPaintStencil S = Document->Stencil(); S.RotationDeg = V; Document->SetStencil(S); }))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 2.0f)
        [
            SNew(STextBlock).Text(LOCTEXT("ProjHdr", "3D PROJECTION & SEAMS")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f))
            + SWrapBox::Slot()
            [
                SNew(SComboButton).OnGetMenuContent(this, &STiPaintEditor::MakeProjectionModeMenu)
                .ButtonContent()[SNew(STextBlock).Text(LOCTEXT("ProjMode", "Projection Mode"))]
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("ProjRun", "Project to Mesh")).OnClicked(this, &STiPaintEditor::RunProjection)
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("DilateBtn", "Dilate Seams"))
                .OnClicked_Lambda([this] { Document->DilateSeams(2); Status = LOCTEXT("Dilated", "Dilated UV seams (2px)"); return FReply::Handled(); })
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
        [
            MakeSliderRow(LOCTEXT("PrjScale", "3D Scale"), 0.25f, 12.0f,
                TAttribute<float>::CreateLambda([this] { return Document->Projection().Scale; }),
                FOnFloatValueChanged::CreateLambda([this](float V) { FTiPaintProjectionSettings P = Document->Projection(); P.Scale = V; Document->SetProjection(P); }))
        ];
}

TSharedRef<SWidget> STiPaintEditor::MakeAiJsonPanel()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
        [
            SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f))
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("RunAi", "Run JSON")).OnClicked(this, &STiPaintEditor::RunAiJson)]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("CopySchema", "Copy Schema"))
                .OnClicked_Lambda([this]
                {
                    FPlatformApplicationMisc::ClipboardCopy(*FTiPaintAPI::GetApiSchemaJson());
                    AiLastOutput = TEXT("Copied TiPaintAPI JSON schema to clipboard.");
                    return FReply::Handled();
                })
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton).Text(LOCTEXT("InspectDoc", "Inspect"))
                .OnClicked_Lambda([this]
                {
                    AiLastOutput = FTiPaintAPI::ExportDocumentSummaryJson(*Document);
                    return FReply::Handled();
                })
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(SBox).HeightOverride(150.0f)
            [
                SNew(SMultiLineEditableTextBox)
                .Text_Lambda([this] { return FText::FromString(AiJsonInput); })
                .OnTextChanged_Lambda([this](const FText& T) { AiJsonInput = T.ToString(); })
            ]
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
            .Text_Lambda([this] { return FText::FromString(AiLastOutput); })
        ];
}

TSharedRef<SWidget> STiPaintEditor::MakeLayerMenu()
{
    FMenuBuilder Menu(true, nullptr);
    for (int32 Index = Document->Layers().Num() - 1; Index >= 0; --Index)
    {
        const FTiPaintLayer& L = Document->Layers()[Index];
        const FString Label = FString::Printf(TEXT("%d: %s [%s]%s%s"),
            Index + 1, *L.Name, *FTiPaintDocument::BlendModeToString(L.BlendMode),
            L.bVisible ? TEXT("") : TEXT(" (Hidden)"),
            L.bLocked ? TEXT(" (Locked)") : TEXT(""));
        Menu.AddMenuEntry(FText::FromString(Label), FText(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this, Index] { Document->SetActiveLayer(Index); })));
    }
    return Menu.MakeWidget();
}

TSharedRef<SWidget> STiPaintEditor::MakeBlendModeMenu()
{
    FMenuBuilder Menu(true, nullptr);
    const TPair<const TCHAR*, ETiPaintBlendMode> Modes[] = {
        { TEXT("Normal"), ETiPaintBlendMode::Normal },
        { TEXT("Multiply"), ETiPaintBlendMode::Multiply },
        { TEXT("Screen"), ETiPaintBlendMode::Screen },
        { TEXT("Overlay"), ETiPaintBlendMode::Overlay },
        { TEXT("Add"), ETiPaintBlendMode::Add },
        { TEXT("Darken"), ETiPaintBlendMode::Darken },
        { TEXT("Lighten"), ETiPaintBlendMode::Lighten }
    };
    for (const auto& Pair : Modes)
    {
        Menu.AddMenuEntry(FText::FromString(Pair.Key), FText(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this, M = Pair.Value] { Document->SetActiveLayerBlendMode(M); })));
    }
    return Menu.MakeWidget();
}

TSharedRef<SWidget> STiPaintEditor::MakeTipMenu()
{
    FMenuBuilder Menu(true, nullptr);
    const TPair<const TCHAR*, ETiPaintBrushTip> Tips[] = {
        { TEXT("Round"), ETiPaintBrushTip::Round },
        { TEXT("Soft Airbrush"), ETiPaintBrushTip::SoftAirbrush },
        { TEXT("Square"), ETiPaintBrushTip::Square },
        { TEXT("Chalk"), ETiPaintBrushTip::Chalk },
        { TEXT("Noise"), ETiPaintBrushTip::Noise }
    };
    for (const auto& Pair : Tips)
    {
        Menu.AddMenuEntry(FText::FromString(Pair.Key), FText(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this, T = Pair.Value] { FTiPaintBrush B = Document->Brush(); B.Tip = T; Document->SetBrush(B); })));
    }
    return Menu.MakeWidget();
}

TSharedRef<SWidget> STiPaintEditor::MakeSymmetryMenu()
{
    FMenuBuilder Menu(true, nullptr);
    const TPair<const TCHAR*, ETiPaintSymmetryMode> Syms[] = {
        { TEXT("None"), ETiPaintSymmetryMode::None },
        { TEXT("Mirror U"), ETiPaintSymmetryMode::MirrorU },
        { TEXT("Mirror V"), ETiPaintSymmetryMode::MirrorV },
        { TEXT("Radial 4"), ETiPaintSymmetryMode::Radial4 }
    };
    for (const auto& Pair : Syms)
    {
        Menu.AddMenuEntry(FText::FromString(Pair.Key), FText(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this, S = Pair.Value] { FTiPaintBrush B = Document->Brush(); B.Symmetry = S; Document->SetBrush(B); Refresh(); })));
    }
    return Menu.MakeWidget();
}

TSharedRef<SWidget> STiPaintEditor::MakeStencilTypeMenu()
{
    FMenuBuilder Menu(true, nullptr);
    const TPair<const TCHAR*, ETiPaintStencilType> Types[] = {
        { TEXT("Checker"), ETiPaintStencilType::Checker },
        { TEXT("Grid"), ETiPaintStencilType::Grid },
        { TEXT("Dots"), ETiPaintStencilType::Dots },
        { TEXT("Stripes"), ETiPaintStencilType::Stripes },
        { TEXT("Radial Vignette"), ETiPaintStencilType::RadialVignette },
        { TEXT("Noise"), ETiPaintStencilType::Noise },
        { TEXT("Image"), ETiPaintStencilType::Image }
    };
    for (const auto& Pair : Types)
    {
        Menu.AddMenuEntry(FText::FromString(Pair.Key), FText(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this, T = Pair.Value] { FTiPaintStencil S = Document->Stencil(); S.Type = T; S.bEnabled = true; Document->SetStencil(S); Refresh(); })));
    }
    return Menu.MakeWidget();
}

TSharedRef<SWidget> STiPaintEditor::MakeSubMeshMenu()
{
    FMenuBuilder Menu(true, nullptr);
    Menu.AddMenuEntry(
        LOCTEXT("AllSubMeshes", "All SubMeshes (Unfiltered)"),
        LOCTEXT("AllSubMeshesTip", "Show and paint across all SubMeshes"),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this]()
        {
            Document->SetActiveSubMeshFilter(INDEX_NONE);
            Refresh();
        })));

    const TArray<FTiPaintSubMeshInfo>& Subs = Document->GetSubMeshes();
    for (int32 i = 0; i < Subs.Num(); ++i)
    {
        const FString Name = Subs[i].Name.IsEmpty() ? FString::Printf(TEXT("SubMesh %d"), i) : Subs[i].Name;
        const FText Label = FText::FromString(FString::Printf(TEXT("[%d] %s (%d tris)"), i, *Name, Subs[i].TriangleCount));
        Menu.AddMenuEntry(
            Label,
            LOCTEXT("SubMeshEntryTip", "Isolate 2D UV overlay and mask painting to this SubMesh"),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this, i]()
            {
                Document->SetActiveSubMeshFilter(i);
                Refresh();
            })));
    }
    return Menu.MakeWidget();
}

TSharedRef<SWidget> STiPaintEditor::MakeProjectionModeMenu()
{
    FMenuBuilder Menu(true, nullptr);
    const TPair<const TCHAR*, ETiPaintProjectionMode> Modes[] = {
        { TEXT("Triplanar"), ETiPaintProjectionMode::Triplanar },
        { TEXT("Planar X"), ETiPaintProjectionMode::PlanarX },
        { TEXT("Planar Y"), ETiPaintProjectionMode::PlanarY },
        { TEXT("Planar Z"), ETiPaintProjectionMode::PlanarZ },
        { TEXT("Cylindrical"), ETiPaintProjectionMode::Cylindrical },
        { TEXT("Spherical"), ETiPaintProjectionMode::Spherical },
        { TEXT("UV 2D"), ETiPaintProjectionMode::UV2D }
    };
    for (const auto& Pair : Modes)
    {
        Menu.AddMenuEntry(FText::FromString(Pair.Key), FText(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this, M = Pair.Value] { FTiPaintProjectionSettings P = Document->Projection(); P.Mode = M; Document->SetProjection(P); })));
    }
    return Menu.MakeWidget();
}

TSharedRef<SWidget> STiPaintEditor::MakeStatus()
{
    return SNew(SBorder).Padding(4.0f)
    [
        SNew(STextBlock).Text_Lambda([this]
        {
            return FText::Format(
                LOCTEXT("StatusText", "{0}  |  {1}x{2}  |  {3} layer(s)  |  {4} undo / {5} redo"),
                Status,
                FText::AsNumber(Document->Width()),
                FText::AsNumber(Document->Height()),
                FText::AsNumber(Document->Layers().Num()),
                FText::AsNumber(Document->UndoCount()),
                FText::AsNumber(Document->RedoCount()));
        })
    ];
}

#undef LOCTEXT_NAMESPACE
